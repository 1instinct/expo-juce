# Sampler: voices, loading and realtime notes

The sampler (0.10.0+, iOS) is two pieces:

- `ios/SamplerEngine.h`: the voice core. Pure C++, header-only, no JUCE. It never owns, allocates
  or frees sample memory; it borrows each slot's buffer from a `SamplerBufferSource`.
- `ios/SamplerBank.h/.mm`: decoding, resampling and the hand-off of each slot's buffer to the
  audio thread (JUCE `AudioFormatManager`, on a loader queue).

Both are owned by the process (`ExpoJuceAudioEngine.mm`), so loaded samples and parameters work
before `setup()` and survive engine rebuilds. Each `PolySynthProcessor` the engine builds is
attached to them. The API is in the README ([Sampler](../README.md#sampler-0100-ios)).

## Signal flow

```
 samplerHit / samplerRelease / JucePadView (sampler) ─┐
 samplerNoteOn / samplerNoteOff ──────────────────────┴► NoteEventQueue (kinds SamplerHit, SamplerNote)
                                                          └► PolySynthProcessor::processBlock
 beginBlock: acquire each slot's buffer (SamplerBank) + snapshot the slot parameters
 drained events ─► SamplerEngine::hit(slot) / noteOn(note) ─► 32 voices ─► Σ stereo × level (ramped) ─► + synth and drums ─► effects
 after the drain: releaseHeld (release marks of the queue, allNotesOff included)
```

- The synth and the drums are mono and copied to both channels; the sampler then adds its stereo
  output to both, so the effects chain processes all three. With no voice sounding the sampler
  does not touch the buffer, so a sampler-free output is bit-identical to 0.9.
- Pads (`samplerHit`) play a slot at its root note. Keys (`samplerNoteOn`) play every loaded slot
  whose key zone (`lowNote`..`highNote`) holds the note; `lowNote > highNote` takes the slot off
  the keys (pads still play it).
- Pitch is the playback rate: `2^((note − rootNote + tune + fineCents / 100) / 12)` × the
  buffer's rate / the engine's rate, read with 4-point Hermite (Catmull-Rom) interpolation
  (`ios/Hermite.h`, shared with the backing track).
- Envelope: linear attack from 0 over `attackMs`, hold, linear release to exact 0 over
  `releaseMs` from wherever the level is. One-shot slots ignore releases and note-offs and play
  from `startMs` to `endMs`; gated slots (`oneShot: false`) play while held, then release.
- Loops (gated slots only): from `loop.startMs` to `loop.endMs`, crossfaded over
  `min(5 ms, loop length / 3)`. Approaching the loop's end the voice fades to the audio just
  before the loop's start, then jumps there, so the seam is continuous. A loop that starts within
  the crossfade's length of the sample's start begins that much later.
- Gain: `10^(gainDb / 20)` × `(1 − velocitySens) + velocitySens × velocity / 127`. Pan is a
  balance: left × `min(1, 1 − pan)`, right × `min(1, 1 + pan)` (centre leaves the sample as is).
- Choke groups 1–8 (open / closed hat behaviour): a voice starting in a group fades every other
  sounding voice of the group out over 5 ms. Voices started by the same call (one key note across
  several slots) are spared.
- Stealing: with every allowed voice busy (`polyphony`, 1–32), a new voice takes the one
  released earliest, else the oldest. A stolen voice, and every voice of a slot whose buffer
  changes (load, unload), stops reading its buffer at once and fades its last output out over 64
  samples.
- Level and mute ramp per 256-sample chunk; the first block after `prepare` starts at the target.

## Loading and the hand-off

Like the backing track:

1. `loadSamplerSlot` takes a generation for the slot (`beginLoad`) and queues the decode on a
   serial loader queue (never the audio or the main thread).
2. The file is decoded in chunks of 65536 frames, converted to stereo (mono to both channels,
   the first two of more) and resampled to the device rate (non-cyclic Hermite). Before
   allocating, it refuses a sample over 60 s (`ERR_SAMPLER_TOO_LONG`), more than the 256 MB cap
   minus the other slots, or more than the process may still allocate (`os_proc_available_memory`;
   `ERR_SAMPLER_MEMORY`). A later load or unload of the slot is noticed between chunks and before
   publishing (`ERR_SAMPLER_SUPERSEDED`).
3. The new buffer is published by an atomic pointer swap. The audio thread acknowledges the
   buffer it reads at the start of every block (a hazard pointer, re-validated against the
   published one). The replaced buffer goes to a retired list.
4. The main thread frees retired buffers the audio thread no longer marks (`collect`), retrying
   every 100 ms while one is still in use.

The audio thread never allocates, frees, locks or waits. Before `setup()` the device rate is
unknown and a sample keeps its file's rate; the engine then plays it at its own speed (and keeps
doing so if the device rate changes later), so a loaded sample is never reloaded.

## Parameters

`SamplerParamStore` holds every slot as atomics plus a version number. Writers (any thread but
the audio thread) are serialized by a mutex and bump the version around each write; the audio
thread copies a slot without waiting and keeps the copy only if no write overlapped it (a
sequence lock with a single attempt; otherwise it keeps the previous copy for that block). Voices
take their parameters when they start; `gainDb`, `pan` and the rest do not change sounding
voices.

## Realtime rules and cost

- Everything is preallocated: 32 voice states, 16 slot states, two 256-frame scratch buffers.
- 32 voices, all looping and pitched, stereo, at 48 kHz / 128 frames: about 23 µs per block on a
  desktop x86 core (harness case `smb`, 0.009 × realtime, budget 60 µs), no allocations. A phone
  core is a few times slower, still a small part of the 2.7 ms block.
- Sample memory: 8 bytes per frame (stereo float): one minute at 48 kHz is about 23 MB.

## Verified where

Harness cases ([test/harness/README.md](../test/harness/README.md)): `sm` (the voice core: pitch,
one-shot, release, loop seam, choke, steal, zones, gain / pan / velocity / level), `smb` (32-voice
timing), `sml` (WAV and MP3 loads, supersede, too long, memory cap, hand-off), `sms` (load / unload
while rendering, no allocation on the audio thread), `smq` (through `PolySynthProcessor` and the
note queue, arpeggiator off and on, `allNotesOff`) and `smd` (`src/samplerDefaults.ts` matches the
native defaults). The Objective-C++ / Swift glue and the pad's sampler mode are compiled first by
the iOS build.
