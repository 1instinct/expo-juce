# expo-juce

Realtime DSP w/C++ & JUCE

expo-juce embeds [JUCE](https://juce.com) 8 in an Expo / React Native app as an Expo
module: a JUCE-owned audio device drives an `AudioProcessorGraph` with an 8-voice
polyphonic synth and a transport, controlled from JS through synchronous JSI calls.
iOS is complete. Android (0.9.0+) is being ported: notes, the synth parameters, the test tone
and diagnostics play through JUCE's Oboe device; the rest of the API exists on Android but is not
implemented yet (see [Android](#android-090)). The web module is a stub.

> **Branches and releases.** `main` is the current line; work lands on it by PR. Pushing a tag
> `v<version>` publishes to npm (`next` for `-rc` versions, `latest` otherwise); see
> [RELEASING.md](RELEASING.md).

## Architecture

```
 JS (src/index.ts)          setup / noteOn / params / transport / getDiagnostics
        │  JSI (Expo Modules API)
 ExpoJuceModule.swift       Function (sync) / AsyncFunction (setup, shutdown), onBeat events
        │
 ExpoJuceBridge (ObjC)      plain ObjC surface for Swift
        │
 ExpoJuceAudioEngine (ObjC++) AVAudioSession Playback + MixWithOthers; setup steps 0–11 on main thread
        │
 juce::AudioDeviceManager ─► GraphPlayer (AudioIODeviceCallback: graph, peaks, NaNs, test tone)
        │
 juce::AudioProcessorGraph  PolySynthProcessor ─► FxChainProcessor ─► audio output node
                            AuxProcessor (metronome + backing track) ───► audio output node (summed)
                            ├─ NoteEventQueue: note-on ring (256) drained at block start; releases as marks
                            ├─ 8 × Voice: dsp::Oscillator → StateVariableTPTFilter (LP) → ADSR
                            ├─ DrumMachine (0.8.0+): 8 synthesized drum voices, mixed after the synth voices
                            └─ TransportEngine (tempo / position / beat crossings)
                            FxChainProcessor: drive → crush → eq → chorus → phaser → echo → reverb →
                            pump → compressor → safety limiter; settings in a process-lifetime store
                            AuxProcessor: clicks rendered at prepare; track buffer swapped in from
                            a process-lifetime BackingTrack holder (decoded on a background queue)
 GraphPlayer also writes the output into an AnalysisRing (getAnalysis: peak / rms / scope)
 FxChainProcessor and AuxProcessor write the instrument and the backing track into SourceRings
 (getSourceAnalysis, 0.10.0+)
```

## Roadmap: Full Audio Workstation

expo-juce is being built toward full audio workstation capabilities. The work is decomposed into independent sub-projects, each building on the previous:

### Sub-project 1: Polyphonic Synth Engine
**Status:** Done — working on device (iPhone, 2026-10-22)

Replaced the mono SynthEngine + AVAudioEngine with a JUCE-owned audio stack built on `AudioProcessorGraph`. 8-voice polyphonic synth with per-voice oscillator, SVF filter, and full ADSR envelope. MIDI-style API (`noteOn(note, velocity)` / `noteOff(note)`). JUCE owns device management and output for cross-platform support (iOS, macOS, Windows, Linux).

### Diagnostics, recovery and latency
**Status:** Diagnostics done (0.5.2-diag.x); recovery and latency margin in 0.5.2 — see the [latency guide](docs/latency.md). TODO: latency measurement and interruption / route-change device checks ([list](docs/latency.md#todo-device-checks-pending-for-052))

`setup()` rejects with the failing step, `getDiagnostics()` exposes every layer from device callbacks to synth peak, `playTestTone()` bypasses the graph, an offline harness reproduces the device callback on Linux. Root causes of the first on-device silence (fixed 2026-10-22): a stale Metro bundle running the app on stubs, infinite recursion in `-setBeatCallback:`, `setup()` always resolving, and expo-av switching the shared session to Ambient. 0.5.2 adds: 128-sample buffer, no allocation on the audio thread, native `JucePadView` touch path, 2 ms default attack, session re-assert and interruption / media-reset recovery (keeping synth settings and transport state across rebuilds).

### Sub-project 2: Effects Chain
**Status:** In progress (0.6.0) — engine + JS API done and verified in the offline harness; not yet run on a device

One `FxChainProcessor` node between the synth and the output with nine effects in a fixed order: drive → crush → eq → chorus → phaser → echo → reverb → pump → compressor, then an always-on safety limiter (-0.3 dBFS). Each effect is enabled / bypassed independently (`setEffectEnabled`, `setEffectParam`, `getEffectsState`, see [Effects](#effects-060)); echo sync and pump follow the transport tempo and beat. Also in 0.6.0: an `AuxProcessor` node summed into the output after the effects, with a sample-accurate metronome (5 click styles, bar accent) and a backing track (off-thread decode + resample, gapless MP3 loop regions, lock-free swap, follows transport play / stop / seek; see [Metronome and backing track](#metronome-and-backing-track-060)); `getAnalysis()` (output peak / rms, 128-point scope, last note, transport) for HUDs; a `hitPolygon` touch shape on `JucePadView`. Later: reorderable / per-node effects via modular routing (sub-project 5).

### Sub-project 3: Sample Playback / Drum Machine
**Status:** Drum machine in progress (0.8.0-rc.0) — engine + JS API verified in the offline harness, not yet run on a device; sampler planned

0.8.0 adds a synthesized drum part (kick, snare, closed / open hat, clap, low / mid / high tom, rim; nine voices, the mid tom since 0.8.0-rc.2; no sample files) inside the synth node, before the effects chain: `drumHit` through the same lock-free queue as `noteOn`, per-voice tune / decay / tone / level, kit level / mute / drive, and a drums mode on `JucePadView` (see [Drums](#drums-080)). Next: a `SamplerProcessor` that loads audio files into the same voice slots (multi-sample mapping, transport-sequenced playback).

### Sub-project 4: Audio Input Processing
**Status:** Planned

Route mic/line input through the `AudioProcessorGraph` for realtime effects processing (guitar pedal board style). The session is currently `Playback` + `MixWithOthers` with no input channels (`initialise(0, 2)`); this sub-project switches to `PlayAndRecord`, opens input channels and adds an input node routed through effects processors.

### Sub-project 5: Modular Routing
**Status:** Planned

Expose the `AudioProcessorGraph` topology to TypeScript — add, remove, and connect nodes from JS. Enables user-configurable signal chains, parallel processing, send/return buses, and sidechain routing.

### Sub-project 6: Electron / Desktop
**Status:** Planned

Compile the same C++ engine for macOS, Windows, and Linux. JUCE abstracts platform audio (CoreAudio, WASAPI, ALSA). Package as an Electron native addon. Same graph, same processors, same API — different platform.

## Quick Start

### Installation

Requires an Expo dev client or EAS build (not Expo Go). Pin an exact version; prereleases are
published under a dist-tag:

```bash
npm install expo-juce@0.5.2-diag.1 --save-exact   # dist-tag "diag"; "latest" is 0.5.1
```

The native code is compiled by CocoaPods during prebuild / EAS build (`ios/ExpoJuce.podspec`,
JUCE vendored in `ios/juce_modules`).

### Usage

```tsx
import * as ExpoJuce from 'expo-juce';

// Once, e.g. on first mount. Rejects with code ERR_AUDIO_SETUP, message "step N: <error>".
await ExpoJuce.setup();
// 0.5.2+: optional device request (defaults 128 samples / 48 kHz; actual values may differ)
// await ExpoJuce.setup({ bufferSize: 128, sampleRate: 48000 });

// Notes — synchronous, never wait on a lock; the audio thread picks them up at the next block
// (note-offs and allNotesOff are never dropped, even mid-recovery)
ExpoJuce.noteOn(60, 127);   // note 0–127, velocity 0–127
ExpoJuce.noteOff(60);
ExpoJuce.allNotesOff();

// Global synth parameters — synchronous, std::atomic
ExpoJuce.setWaveform('saw');          // 'sine' | 'square' | 'saw' | 'triangle'
ExpoJuce.setFilterCutoff(2000);       // Hz, 20–20000 (default 20000)
ExpoJuce.setFilterResonance(0.3);     // 0–1 → Q 0.707–10
ExpoJuce.setAttack(10);               // ms, 1–10000 (default 10; 2 from 0.5.2+)
ExpoJuce.setDecay(1);                 // ms (default 1)
ExpoJuce.setSustain(1);               // 0–1 (default 1)
ExpoJuce.setRelease(200);             // ms (default 200)
ExpoJuce.setDetune(0);                // cents, −1200…1200
ExpoJuce.setMasterLevel(0.5);         // 0–1 (default 0.5)

// Transport
ExpoJuce.setTempo(120);               // BPM, 20–400
ExpoJuce.startTransport();
const sub = ExpoJuce.addBeatListener(({ beat, bpm }) => { /* … */ });
ExpoJuce.getTransportPosition(); ExpoJuce.getTempo(); ExpoJuce.isTransportPlaying();
ExpoJuce.setTransportPosition(0);
ExpoJuce.stopTransport();
sub.remove();

// Diagnostics
const d = ExpoJuce.getDiagnostics();  // AudioDiagnostics snapshot, see src/ExpoJuce.types.ts
ExpoJuce.playTestTone(500);           // 440 Hz into the device output, bypasses graph + synth;
                                      // false if the engine/device is not running

await ExpoJuce.shutdown();
```

### Effects (0.6.0+)

A master effects chain sits between the synth and the output. All effects are off by
default; order is fixed (drive → crush → eq → chorus → phaser → echo → reverb → pump →
compressor), followed by a safety limiter (-0.3 dBFS ceiling, always on, not user-facing).

```ts
ExpoJuce.setEffectEnabled('reverb', true);            // boolean: false = unknown effect
ExpoJuce.setEffectParam('reverb', 'mix', 0.4);        // boolean: false = unknown effect/param or NaN
ExpoJuce.setEffectParam('echo', 'division', 1);       // synced echo, 1/8 at the transport tempo
const fx = ExpoJuce.getEffectsState();                // { reverb: { enabled, params: { size, … } }, … }
```

- Synchronous and lock-free, any thread. Values are clamped to the ranges below (switches and
  division indices rounded); `getEffectsState()` reports the stored values.
- Settings are remembered natively whether or not the engine is running: calls made before
  `setup()`, or during an automatic engine rebuild, take effect when the chain runs.
- Turning an effect on or off crossfades it (10 ms); a disabled effect is skipped entirely and
  an effect turned on starts clean (no stale echo or reverb tail). Echo and reverb spill over:
  turning them off stops feeding them, and their tail keeps ringing out until it has faded
  below -90 dBFS (at most 5 s, faded out over the last 50 ms); only then are they bypassed.
  With every effect off (and tails ended) the output is the synth output, bit for bit (while
  it stays under the limiter ceiling).
- Parameter changes are smoothed where they would click (mix, gains, delay time, EQ bands).
- Echo `sync` and `pump` follow the transport tempo; pump is locked to the transport beat
  while it plays and free-runs at the tempo while it is stopped.
- Binaries older than 0.6.0: the setters return `false` and `getEffectsState()` returns the
  defaults. Web: no-ops.

| effect | params (range, default) |
|---|---|
| `drive` | `amount` 0–1 (0.4), `tone` 0–1 (0.6), `mix` 0–1 (1) — pre-gain 1–40×, tanh, tone LP 800 Hz–20 kHz |
| `crush` | `bits` 2–16 (8), `downsample` 1–32 (4), `noise` 0–1 (0.1), `mix` 0–1 (1) |
| `eq` | `low` −12–12 dB (0, shelf 120 Hz), `mid` −12–12 dB (0, peak Q 0.9), `midFreq` 200–5000 Hz (1000), `high` −12–12 dB (0, shelf 6 kHz) |
| `chorus` | `rate` 0.05–5 Hz (0.8), `depth` 0–1 (0.4), `mix` 0–1 (0.5) |
| `phaser` | `rate` 0.05–5 Hz (0.4), `depth` 0–1 (0.6), `feedback` 0–0.9 (0.5), `mix` 0–1 (0.5) |
| `echo` | `time` 0.02–2 s (0.375), `sync` 0/1 (1), `division` 0–5 (2; 1/4, 1/8, 1/8 dotted, 1/16, 1/4 dotted, 1/2), `feedback` 0–0.95 (0.4), `tone` 0–1 (0.5), `pingPong` 0/1 (0), `mix` 0–1 (0.35) |
| `reverb` | `size` 0–1 (0.6), `damping` 0–1 (0.5), `width` 0–1 (1), `mix` 0–1 (0.3) |
| `pump` | `depth` 0–1 (0.6), `shape` 0–1 (0.5; soft → snappy), `division` 0–3 (0; 1/4, 1/8, 1/2, 1/1) |
| `compressor` | `threshold` −60–0 dB (−18), `ratio` 1–20 (4), `attack` 0.1–100 ms (10), `release` 10–1000 ms (120), `makeup` 0–24 dB (4) |

`mix` is a dry/wet crossfade (0 = dry). Synced echo time is capped at 2 s.

### Metronome and backing track (0.6.0+)

An aux source next to the synth: its output is summed into the device output **after** the
effects chain (clicks and the track are not effected). Both follow the transport.

```ts
ExpoJuce.setMetronome({ enabled: true, style: 'wood', level: 0.7, accent: true, beatsPerBar: 4 }); // false: unknown style

const info = await ExpoJuce.loadBackingTrack(fileUri, {   // gapless info: optional, MP3 only
  loopSamples: 682839, leadingSamples: 4561, trailingSamples: 1496,
});
// { durationSec, sampleRate, channels, loopStartSample, loopSamples, decodedSamples, regionRule, decoder }
// or, with the file's own tempo (0.8.0-rc.3+), it follows the transport tempo:
// await ExpoJuce.loadBackingTrack(fileUri, gapless, { sourceBpm: 124 });
ExpoJuce.setBackingTrackOptions({ level: 0.8, loop: true, enabled: true });
ExpoJuce.startTransport();                                 // the track and the clicks start
ExpoJuce.unloadBackingTrack();
```

- **Metronome**: a click on every beat while the transport plays, starting on the exact
  sample where the beat falls (any block size), accented on beat 1 of the bar. Styles
  `classic` (sine blip 1.5 kHz / accent 2.5 kHz), `wood` (resonant noise burst), `beep`
  (square 1 / 2 kHz), `cowbell` (two squares 540 / 800 Hz through a band-pass), `hat`
  (high-passed noise). Clicks are rendered once when the engine prepares (no allocation while
  playing). Options: `enabled` (false), `style` ('classic'), `level` 0–1 (0.8), `accent`
  (true), `beatsPerBar` 1–16 (4). `setMetronome` returns `false` and applies nothing if
  `style` is not one of the five.
- **Backing track**: `loadBackingTrack(uri, gapless?)` takes a local file (`file://` URI or
  absolute path; download remote files first). It decodes WAV, AIFF, FLAC and MP3 (plus
  CoreAudio formats such as M4A / AAC on iOS) on a background queue straight to the device
  rate and swaps the buffer in without blocking or allocating on the audio thread; the old
  buffer is freed on the main thread after the audio thread has let go of it. Only the
  playable buffer stays in memory (after a device-rate change the file is decoded again).
  Limit: 6 minutes (17 280 000 frames per channel at 48 kHz, about 138 MB; the same duration
  at other rates), and the load is refused if the process has less memory available than the
  decode needs (about twice the final buffer). A newer load or `unloadBackingTrack()`
  supersedes a load in flight (it rejects with `ERR_BACKING_TRACK_SUPERSEDED`); other errors:
  `ERR_BACKING_TRACK_URI`, `_FILE`, `_FORMAT`, `_TOO_LONG`, `_MEMORY`.
- **Gapless MP3 loops**: MP3 encoders pad the start and end. With `gapless` (the LAME / Xing
  tag values, frames at the file's rate) the loop region is, by decoded length:
  - `loopSamples` (the decoder honoured the tag) → `[0, loopSamples)`, `regionRule: 'tag'`;
  - 1152 + leading + loopSamples + trailing (the decoder ignored the tag and decoded the tag
    frame) → `[1152 + leading, +loopSamples)`, `'padded'`;
  - leading + loopSamples + trailing (the decoder skipped the tag frame but kept the encoder
    padding, as JUCE's own MP3 decoder does) → `[leading, +loopSamples)`, `'paddedNoTagFrame'`;
  - anything else, or no `gapless` → the whole file, `'whole'` (with `gapless` given, this
    means the fallback fired and the loop will include the padding).

  The region is cut before resampling; `decodedSamples` / `decoder` tell what the decoder did.
- **Playback** follows the transport. Track sample 0 is beat 0: when the transport starts or
  is moved (`setTransportPosition`), the track goes to beat × 60 / bpm seconds (modulo its
  length when looping), so pause + resume continues where it stopped and seeks land on the
  beat grid. Without a `sourceBpm`, bpm is the transport's and the track simply advances with
  the audio: a tempo change does not move or stretch it (it keeps its own speed).
- **Following the tempo** (0.8.0-rc.3+): with `loadBackingTrack(uri, gapless, { sourceBpm })`,
  bpm above is the track's own tempo and the track follows the transport tempo at its own pitch
  (WSOLA: two read heads at normal speed crossfade every 20 ms; each new head starts where the
  transport beat says the track should be and moves up to 10 ms to continue the sound best).
  Its beats stay on the transport's: within about 10 ms between 0.8× and 1.25×, never adding
  up, and a tempo change while playing holds at once. Far from its tempo (towards half or
  double speed) transients smear and repeat more. At exactly its own tempo there is no stretch:
  the file plays sample for sample. The result's `sourceBpm` says what the binary took (absent
  before 0.8.0-rc.3); `getDiagnostics().backingTrackTempoRatio` is transport / track tempo. Starts away from the top, seeks, stops, and
  replacing or unloading the track while it plays are crossfaded over 5 ms. With `loop` off it
  plays once and then stays silent until the transport is moved back.
- Options: `level` 0–1 (0.8), `loop` (true), `enabled` (true).
- Metronome and backing-track options and the loaded track are kept natively for the life of
  the app: they work before `setup()` and survive engine rebuilds.
- Binaries older than 0.6.0: the setters are no-ops (`setMetronome` returns `false`) and
  `loadBackingTrack` rejects with `ERR_UNSUPPORTED`. Web: the same.

### Arpeggiator (0.7.0+)

A sample-accurate arpeggiator between the note queue and the voices. While it is enabled, every
note the synth receives (pads, keys, `noteOn`) joins its held list instead of sounding, and the
arp plays the held notes step by step on the transport's grid; disabled, notes play directly as
before (voices that were sounding when the arp was enabled keep sounding until released).

```ts
ExpoJuce.setArpeggiator({ enabled: true, mode: 'upDown', rate: '1/16', octaves: 2, gate: 0.6 });
ExpoJuce.setArpeggiator({ pattern: [true, false, true, true], swing: 0.2, ratchet: 2 }); // partial
const state = ExpoJuce.getArpeggiatorState(); // every option as stored (ArpOptions)
ExpoJuce.ARP_DEFAULTS;                        // the defaults below
```

- Options (`ArpOptions`, omitted keys keep their value, numbers are clamped; `setArpeggiator`
  returns `false` and applies nothing for an unknown `mode` / `rate` or a `pattern` that is not
  1–16 booleans):
  - `enabled` (false)
  - `mode` ('up'): `'up'` held notes ascending, `'down'`, `'upDown'` (up, then the interior
    reversed: `[60, 64, 72, 76]` → `60 64 72 76 72 64`), `'played'` (press order), `'random'`
    (a uniform pick per step), `'chord'` (every held note on every step)
  - `rate` ('1/16'): `'1/4'`, `'1/8'`, `'1/8T'`, `'1/8D'`, `'1/16'`, `'1/16T'`, `'1/32'`
    (1, 2, 3, 4/3, 4, 6, 8 steps per beat)
  - `octaves` 1–4 (1): the cycle repeats +12 per extra octave
  - `gate` 0.05–1.5 of a step (0.6): note length; above 1 the note overlaps the next step
    (legato; a one-note cycle retriggers the same voice)
  - `swing` 0–0.5 of a step (0): odd steps start that much later
  - `latch` (false): the list is kept when every note is released; the next press replaces it
  - `pattern` 1–16 booleans (16 × true), all or nothing: a false step is a rest and does not
    advance the cycle
  - `accent` 0–1 (0): steps other than the first of the pattern play at
    velocity × (1 − 0.6 × accent)
  - `ratchet` 1–4 (1): hits per step, each with `gate` of its share
  - `transposeSteps` 0–8 (0) and `transposeDistance` −12..12 (0): each pass through the pattern
    rises by `transposeDistance` semitones, wrapping after `transposeSteps` passes
- Timing: step `k` starts `k × L` after the arp starts (`L = 60 / bpm / stepsPerBeat`), on the
  exact sample whatever the block size. While the transport plays the steps sit on the beat grid
  (`beat × stepsPerBeat` integer; a chord landing up to ¼ step after a grid line plays at once,
  later ones wait for the next line) and a tempo change re-derives the grid from the new bpm.
  Stopped, the grid starts at the first note-on and runs free at the current tempo.
- The cycle restarts when the held list goes from empty to non-empty; adding or removing notes
  while it plays takes effect at the next step. `allNotesOff()` stops the arp and clears its
  list (a latched list included). Disabling stops the sounding arp notes at once.
- 16 voices (`MAX_VOICES`), so a 16-note `chord` or ratchets with long releases do not steal.
- Options are kept natively for the life of the app in one store the synth reads once per block:
  they work before `setup()`, survive engine rebuilds and apply within a block even while a
  rebuild is in progress. Binaries older than 0.7.0: `setArpeggiator` returns `false`,
  `getArpeggiatorState` returns `ARP_DEFAULTS`. Web: the same.

### Drums (0.8.0+)

Nine synthesized voices (no samples): `0` kick, `1` snare, `2` closed hat, `3` open hat, `4`
clap, `5` low tom, `6` high tom, `7` rim, `8` mid tom (0.8.0-rc.2+). Indices 0–7 are the same
as in 0.8.0-rc.0 / rc.1; binaries from those report 8 voices in `getDrumState().voices` and
ignore voice 8 (`drumHit` does nothing, `setDrumVoice` returns `false`), so read the voice count
from `getDrumState()` rather than `DRUM_VOICE_COUNT` when the binary may be older. The drum part is mixed with the synth
before the effects chain (so drive, echo, reverb, … apply to both) with its own level and mute;
the synth's `setMasterLevel` does not affect it and the arpeggiator never applies to it.

```ts
if (ExpoJuce.hasDrums()) {
  ExpoJuce.drumHit(0, 120);                                  // kick, velocity 1–127
  ExpoJuce.setDrumVoice(1, { tune: -2, decay: 0.3, tone: 0.8 }); // partial; clamped
  ExpoJuce.setDrumKit({ level: 0.9, drive: 0.25 });
  const { kit, voices } = ExpoJuce.getDrumState();           // as stored (after clamping)
}
```

- `drumHit(voice, velocity)`: any thread, lock-free, through the same queue as `noteOn` (hits and
  notes keep their order and start at the next audio block). Each voice is monophonic: a new hit
  restarts it (the previous sound fades out over 64 samples, so retriggers do not click). The
  closed hat chokes the open hat (it fades out over about 12 ms). A voice outside 0–8 (or not an
  integer) is ignored; velocity is clamped to 1–127 and scales the level linearly. Hits are
  dropped while the engine is not set up, like note-ons.
- `setDrumVoice(voice, params)` (`DrumVoiceParams`; omitted and unknown keys unchanged, numbers
  clamped; `false` for a voice outside 0–8): `tune` −24..24 semitones (0), `decay` 0–1 (0.5:
  scales every envelope of the voice, 0 = ×0.25, 1 = ×4), `tone` 0–1 (0.5, per voice below),
  `level` 0–1 (0.8).
- `setDrumKit(params)` (`DrumKitParams`): `level` 0–1 (0.8), `mute` (false), `drive` 0–1 (0: soft
  saturation of the kit sum, blended in with the amount). Level and mute ramp over one block.
- `DRUM_VOICE_DEFAULTS`, `DRUM_KIT_DEFAULTS`, `DRUM_VOICE_COUNT` (9; 8 before 0.8.0-rc.2) are exported.
- Parameters are kept natively for the life of the app (they work before `setup()` and survive
  engine rebuilds). Binaries older than 0.8.0 and web: `hasDrums()` is `false`, `drumHit` does
  nothing, the setters return `false`, `getDrumState()` returns the defaults.
- Diagnostics: `drumHits` (hits started, cumulative since launch), `drumPeak` (the drum part's
  peak before the effects, decaying hold). `synthPeak` no longer includes drums.

| # | Voice | Model | `tone` |
|---|---|---|---|
| 0 | Kick | sine, pitch envelope from 4× the base (52 Hz) down within ~40 ms, noise click, amp decay 0.6 s | click amount |
| 1 | Snare | two sines (185 / 330 Hz, short pitch bend) + band-passed noise, separate decays (0.2 / 0.3 s) | noise vs body, noise band 2.5–7 kHz |
| 2 | Closed hat | six square oscillators at 808 ratios + noise, band-pass then high-pass, 0.09 s | brighter, more noise |
| 3 | Open hat | same source, 0.7 s; choked by the closed hat | same |
| 4 | Clap | band-passed noise, three bursts 10 ms apart, then a 0.35 s tail | band centre 0.9–2.5 kHz |
| 5 | Low tom | sine (95 Hz) with a small pitch drop, 0.5 s | attack noise |
| 6 | High tom | same at 150 Hz, 0.4 s | attack noise |
| 7 | Rim | band-passed noise click (3.5 kHz) + sine ping (1660 + 480 Hz), 0.07 s | click vs ping |
| 8 | Mid tom | same as the toms at 119.4 Hz (the log-centre of 95 and 150 Hz), 0.45 s | attack noise |

Decay times above are to −60 dB at `decay` 0.5. More on the voices in
[docs/drums.md](docs/drums.md).

### Analysis (0.6.0+)

```ts
const a = ExpoJuce.getAnalysis();
// { peak, rms, scope: number[128], bands: number[24], sampleRate, lastNote, bpm, beat, playing,
//   arp: { active, step, steps, note } }
```

Synchronous and lock-free, cheap enough to call every animation frame (from one place: each
call consumes the levels). `peak` (peak hold) and `rms` (both channels) cover the output
since the previous call (synth, effects, metronome and backing track as sent to the device);
`scope` is 128 points, oldest first, taken every 16th sample from the last 2048 output samples
(mono, may tear across a block; it is for display). `bpm` / `beat` / `playing` are the
transport as of the last rendered block. `lastNote` is the last note the synth started (-1 if
none). Zeros when the engine is not running.

0.7.0+ adds `bands`: 24 log-spaced spectrum bands between 40 Hz and 16 kHz, each 0–1 for
−60..0 dBFS of the loudest FFT bin in the band, from a Hann-windowed 2048-point FFT of the same
output samples, computed on the calling thread (about 50 µs per call). Band edges are
`edge_0 = 40`, `edge_i = max(40 × 400^(i/24), edge_(i−1) + sampleRate / 2048)`,
`edge_24 = 16000`: the lowest bands are one FFT bin wide (23.4 Hz at 48 kHz) until the
geometric spacing takes over around 140 Hz, so no two bands read the same bin. `sampleRate` is
the device rate the bands were computed at. `arp` is the arpeggiator as of the last block:
`active` (enabled and holding notes), `step` (pattern slot of the last step started), `steps`
(pattern length), `note` (last note the arp started, -1 if none). Older binaries: 24 zeros,
`sampleRate` 0, `arp` inactive.

### Per-source analysis (0.10.0+)

`getAnalysis()` is the whole output. `getSourceAnalysis()` splits it into two sources, each with
its own spectrum, level and scope:

- `'instrument'`: the synth and the drums after the effects chain (what is played, as heard);
- `'backing'`: the backing track alone, at its level.

The metronome is in neither (it stays in `getAnalysis()`, which is unchanged).

```ts
if (ExpoJuce.hasSourceAnalysis()) {
  const backing = ExpoJuce.getSourceAnalysis('backing', { scope: 'none' });
  const inst = ExpoJuce.getSourceAnalysis('instrument', { scope: 'cycle', scopePoints: 32 });
  // { active, peak, rms, bands: number[24], scope: number[n], scopeMode, scopePeriod, sampleRate }
}
```

Options (all optional; numbers are clamped):

| Option | Default | |
|---|---|---|
| `bands` | `true` | 24 bands, the table and scaling of `getAnalysis().bands` (one 2048-point FFT); `false` returns 24 zeros |
| `scope` | `'window'` | `'none'` (`scope: []`), `'window'` or `'cycle'` |
| `scopePoints` | 64 | 16–128 |
| `cycleHz` | the last note the synth started | `'cycle'`: the fundamental |
| `windowMs` | 20 | 5–42, `'window'` and the `'cycle'` fallback |

- `active`: the source is producing sound. Backing: a track is loaded at the device rate, enabled,
  at a level above 0, the transport plays and the position is inside the track (or it loops), so
  a track that ended with `loop: false` or a stopped transport reads `false` by itself.
  Instrument: its output reached −80 dBFS within the last 100 ms.
- `peak` / `rms`: since the previous call for that source (each source keeps its own).
- `scope` (−1..1, not normalised):
  - `'window'`: the last `windowMs`, each point a Hann-weighted average over its own span (no
    aliasing: a 9 kHz tone reads under 0.002 of its level in 64 points over 20 ms, where picking
    samples would show it at full level), centred on the largest positive peak of the last two
    window lengths, refined between samples. A steady tone stands still from call to call; a hit
    sits in the middle once it is half a window old.
  - `'cycle'`: one period P = sampleRate / f, starting at its peak (point 0 is the maximum,
    placed between samples) and resampled to `scopePoints` with the same anti-alias weighting, so
    consecutive calls on a steady tone return the same shape (a synth saw moves by under 1% of its
    range between calls). The period after the latest rising zero crossing must correlate with
    the one before it (≥ 0.9); otherwise, and for frequencies whose three and a half periods do not
    fit the 7168-sample history (below about 24 Hz at 48 kHz), the scope comes back as `'window'`
    (`scopeMode` says which). Chords, drums, noise, glides and silence fall back this way.
  - `scopePeriod`: P in samples for `'cycle'`, else 0.
- Zeros, `active: false` and `sampleRate: 0` while the engine is not running. Older binaries
  (and web): the same inactive shape, and `hasSourceAnalysis()` is `false`.

Cost: on the audio thread, two ring writes per block (per sample one relaxed atomic store, a max
and a multiply-add; about 0.25 µs per 256-sample block each on a desktop core in the harness), no
FFT, allocation or lock. On the calling thread, per call: a copy of the source's ring, one FFT
when `bands` is on (about 50 µs) and the scope (bands + cycle about 40 µs in the harness). Two
calls per frame at 25–60 Hz are fine on the JS thread. Calls for one source are serialised (a
reader-side mutex the audio thread never takes).

Taps: the instrument is written at the end of `FxChainProcessor` (after the limiter); the backing
track inside `AuxProcessor`, which now renders the track first, writes the tap, then adds the
metronome: both add into the cleared block, so the node's output is the same bit for bit (the
harness compares a graph with the taps against one without).

Android: `'instrument'` is analysed; `'backing'` is always inactive (no backing track on Android
yet).

### `JucePadView` (0.5.2+)

A native view that plays a note on touch from the iOS main thread, without the JS thread,
and reports to JS afterwards for visuals and haptics. Multi-touch safe: note off is sent when
the last touch on the view lifts.

```tsx
import { JucePadView } from 'expo-juce';

<JucePadView
  note={60}
  velocity={127}
  enabled
  onNoteOn={() => {/* light the pad, haptic */}}
  onNoteOff={() => {/* unlight */}}
  style={{ width: 80, height: 80 }}
/>
```

Check the shipped type definitions for the exact event payloads once 0.5.2 is published.

`mode` (0.8.0+): `'notes'` (default) holds `note` on the synth while touched; `'drums'` makes
every touch-down hit drum voice `note` (0–8) natively, with the same `velocity` (one-shot; a
second finger on the pad retriggers it). `onNoteOn` fires per hit and `onNoteOff` when the last
touch lifts; no note-off reaches the engine. Changing `mode` releases the pad. Older binaries
ignore the prop (the pad keeps playing notes), so check `hasDrums()` first.

```tsx
<JucePadView mode="drums" note={0} velocity={110} style={{ width: 80, height: 80 }} />
```

`hitPolygon` (0.6.0+) makes the touch area non-rectangular: a flat `[x0, y0, x1, y1, …]`
polygon normalised to the view (0–1). Touches outside it (including on its children) pass
through to the views underneath, so pads with overlapping bounding boxes do not steal each
other's touches. Empty, odd-length or fewer than 3 points: the whole view.

```tsx
<JucePadView note={62} hitPolygon={[0.5, 0, 1, 0.5, 0.5, 1, 0, 0.5]} style={{ width: 96, height: 96 }} />
```

### MIDI byte input (0.9.0-rc.2+, iOS)

For MIDI that arrives in the app's own native code, such as a Bluetooth LE notification callback
or a serial port. That code passes the raw bytes to the engine on its own thread, the engine
plays mapped notes straight from its note queue (no JavaScript on the way), and JS gets every
parsed message afterwards for logging, learning and recording.

```objc
#include <dlfcn.h>
typedef int (*MidiInputFn)(const uint8_t *, size_t, int, double);
static MidiInputFn fn;  // dlsym(RTLD_DEFAULT, "expojuce_midi_input_bytes"), once
// In the notification callback, first thing:
double arrival = CACurrentMediaTime();
if (fn) fn(bytes, length, 0 /* EXPOJUCE_MIDI_FORMAT_BLE_MIDI */, arrival);
```

Formats: `0` one BLE-MIDI packet per call (header and timestamp bytes, running status, SysEx
skipped), `1` a raw MIDI stream (any split across calls), `2` raw packets that must each be
complete MIDI (anything else, such as a text log a serial bridge interleaves, is ignored whole,
except that its complete note-offs still end what the input holds).
`ios/ExpoJuceMidiInput.h` documents the function; code that must run without this library looks
it up with `dlsym`, or `+[ExpoJuceBridge midiInputBytesFunction]` when the executable's global
symbols are stripped.

```ts
import {
  hasMidiInput, setMidiInputMap, setMidiInputEnabled, addMidiInputListener,
  releaseMidiInputHolds,
} from 'expo-juce';

if (hasMidiInput()) {
  // Input note → what it plays: a synth note, a drum voice, or null (passed to JS).
  setMidiInputMap({
    actions: Array.from({ length: 128 }, (_, n) => (n === 36 ? { drum: 0 } : { note: n })),
    velocity: Array.from({ length: 128 }, (_, v) => Math.max(1, v)), // optional curve
  });
  setMidiInputEnabled(true);
  const sub = addMidiInputListener(({ events }) => {
    for (const e of events) {
      if (!e.played && e.type === 'noteOn') { /* not mapped (or map off): play it from JS */ }
      // e.arrival: seconds on getMidiInputClock()'s clock (CACurrentMediaTime)
    }
  });
  // The device went away: end the notes it holds.
  releaseMidiInputHolds();
}
```

- The map is swapped atomically (readers see the old map or the new one) and stays off until
  `setMidiInputEnabled(true)`; while off, every note-on passes to JS.
- A note-off releases what its note-on played, whatever the map says by then. Two inputs holding
  the same synth note are counted; a note held by another source (`noteOn` from JS, a
  `JucePadView`) is left to that source's release. A second note-on of a held input note
  releases the first (an event with `synthetic: true`).
- Events carry the release epoch: `releaseMidiInputHolds()` and `allNotesOff()` bump it, so
  events from before a release can be told apart.
- `midiInputBytes(bytes, format, arrival?)` feeds the same path from JS (tests, a JS transport);
  one call is one packet of at most 512 bytes (more returns -1).
- `midiInputToRenderMs` starts at the caller's `arrivalSeconds`: stamp it first thing in the
  callback (work done before the stamp is not counted).
- Android and older binaries: `hasMidiInput()` is false, the setters return false.

### Diagnostics

`getDiagnostics()` returns setup state (`isReady`, `setupStep`, `lastError`), device
(`deviceName`, `sampleRate`, `bufferSize`, `outputLatencySamples`), counters
(`callbackCount`, `graphProcessCount`, `synthProcessCount`, `notesQueued/Consumed/Dropped`,
`nanCount`, `toneSamplesMixed`), peaks (`outputPeak`, `synthPeak`), timing
(`lastNoteToRenderMs`) and the AVAudioSession state (`sessionCategory`, `sessionSampleRate`,
`sessionIOBufferMs`, `sessionOutputLatencyMs`, `otherAudioPlaying`, `routeOutputs`). 0.5.2+ adds
`lastTouchToRenderMs`, `preferredBufferSize`, `preferredSampleRate`, `sessionReassertCount`, `interruptionCount`,
`mediaResetCount`, `lastRecoveryError`, `engineState`. A debug screen in the app can show them
as a pass/fail ladder — see [docs/latency.md](docs/latency.md#5-a-diagnostics-panel).

0.6.0+ adds `fxProcessCount`, `fxLimiterGain` (1 = the safety limiter is idle),
`fxNonFiniteResets`, `metronomeClicks`, `backingTrackLoaded` and `backingTrackPositionSec`.
0.8.0+ adds `drumHits` and `drumPeak`.

0.9.0-rc.1+ (iOS) splits the touch → sound path into legs and reports what the device was asked
for. `touchLatency` holds `last` / `p50` / `p95` / `max` / `count` over the last 64 pad notes and
hits for `touchDeliveryMs` (`UITouch.timestamp` → `touchesBegan`: iOS delivering the touch),
`touchHandlerMs` (`touchesBegan` → note queue), `queueToRenderMs` and `touchToRenderMs`, plus
`bufferMs` and `outputLatencyMs`. `touchPath` lists the gesture recognizers and scroll views
between the pad and its window with their delay flags, `lastTouch` the recognizers the last touch
was delivered to, and `mainThreadMaxGapMs` / `mainThreadStalls50` come from a display-link stall
probe. `requestedSampleRate` / `grantedSampleRate`, `requestedBufferSize` / `grantedBufferSize`,
`hardwareSampleRate`, `availableBufferSizes`, `deviceOpens` (the last 8 opens) and `xrunCount`
show the device format. See [docs/latency.md](docs/latency.md#touch-latency-legs-and-device-format-090-rc1).

0.9.0-rc.4+ (iOS) adds `mainThreadStallLog` (the last 32 main-thread gaps over 50 ms with their
start on the `clockSec` clock), `mainThreadStallIntervalP50Ms` (their median spacing),
`clockSec`, and the display rate: `displayHz` (the main display link's measured rate),
`maxFramesPerSecond` and `displayRateHint` (see "Display-rate hint" below).

0.9.0-rc.2+ (iOS) adds `midiInput` (MIDI byte input counters: `packets`, `messages`, `played`,
`passed`, `dropped`, `rejectedPackets`, `eventsDropped`, `held`, `epoch`, `mapVersion`,
`enabled`) and the `midiInputToRenderMs` leg (bytes' arrival → render block), also in
`touchLatency`.

### Display-rate hint

`setDisplayRateHint(hz)` (0.9.0-rc.4+, iOS) keeps a ProMotion display at `hz` (above 60, up to
the screen's `maximumFramesPerSecond`) with a main-thread `CADisplayLink` whose
`preferredFrameRateRange` is (80, hz, hz); 0 or 60 removes it. UIKit delivers touches on display
frames, so at 120 Hz a touch can reach the app up to about 8 ms sooner than at 60 Hz; compare
`touchLatency.touchDeliveryMs` with the hint on and off. On iPhone the app also needs
`CADisableMinimumFrameDurationOnPhone: true` in its Info.plist, or the display stays at 60.

- Any thread; applied on the main thread. Does not need `setup()`, survives engine rebuilds,
  pauses while the app is inactive, and is cleared when a module is created and when the module
  that set it goes away (after a reload, JS sets it again).
- With the Info.plist key, every display link in the app (`requestAnimationFrame`, animation
  libraries, GL render loops) may run at the higher rate while the display does. Cap the ones that
  should not (a render loop can draw every other frame).
- `getDiagnostics()`: `displayHz` (the link's measured rate), `maxFramesPerSecond`,
  `displayRateHint`. Without a hint above 60 the link is held to 60, so `displayHz` 60 then
  says nothing about other links that may run the display faster.
- Android, web and older binaries: `setDisplayRateHint` returns false and does nothing.

### Beat events

`addBeatListener(fn)` gets `{ beat, bpm, position }` each time the playing transport crosses a
beat (polled every 16 ms on the main thread): `beat` is the fractional transport position at the
end of the audio block that crossed it (`Math.floor(beat)` is the beat number), `position`
(0.8.0+) the fractional position when the event is sent. 0.8.0 fixes two cases where no events
arrived: a listener added before `setup()` (the timer now starts on setup), and listeners added
again after all had been removed (removing the last listener no longer drops the native callback).

Synth settings, effect, metronome and backing-track settings (and the loaded track) and transport state (tempo, position, playing) survive the
engine's automatic rebuilds after a media-services reset or a stalled device; held notes do not.

### Android (0.9.0+)

The Android module builds the same engine (`ios/*.mm` engine units, the vendored JUCE and its
Oboe) into `libexpojuce.so` with the NDK (`android/CMakeLists.txt`, run by Gradle's
`externalNativeBuild`). It plays through JUCE's Oboe device (AAudio, or OpenSL ES on devices
without it), output only, so no microphone permission is involved.

| | Android 0.9.0 |
|---|---|
| `setup` / `shutdown`, `noteOn` / `noteOff` / `allNotesOff`, the nine synth setters, `playTestTone`, `getDiagnostics` | implemented |
| `getSourceAnalysis` (0.10.0+) | `'instrument'` implemented; `'backing'` always inactive |
| effects, metronome / backing track, analysis, arpeggiator, drums, transport, beat events | present, not implemented: setters return `false` (nothing applied), getters return the defaults, `loadBackingTrack` rejects with `ERR_UNSUPPORTED`, `hasDrums()` is `false` |
| `JucePadView` | not available: `isJucePadViewAvailable()` is `false` |

Differences from iOS:

- The engine prefers the device's native output sample rate (`nativeSampleRate` in
  diagnostics) over `setup({ sampleRate })`, so the stream is not resampled; `sampleRate` reports
  the rate in use. The buffer is the nearest size the device lists to `bufferSize`, and at least
  two of the device's bursts.
- In the background the audio device is closed (held notes are released, nothing renders) and it
  reopens with the same setup when the app returns. Note-ons sent while in the background are
  dropped.
- Diagnostics add `platform: 'android'`, `nativeSampleRate`, `xrunCount`, `backgrounded`,
  `backgroundStops` and `foregroundRestarts`; the AVAudioSession fields are empty or zero.
- If the native library cannot be loaded, the getters still return their full default shapes and
  `getDiagnostics()` reports `isReady: false`, `engineState: 'failed'` and the reason in `lastError`.
- No audio-focus handling, interruption recovery or engine rebuilds yet.

Build requirements: the app's NDK (`ndkVersion` from the root project, 27.1.12297006 if unset),
CMake (Gradle's default), and the ABIs in `reactNativeArchitectures`. The library uses
`libc++_shared.so` like React Native, and is 16 KB page aligned.

## Latency

Budget < 40 ms touch-to-sound, target ≤ 25 ms. Baseline on iPhone 16 Pro speaker: 48 kHz,
257-sample buffer, panel estimate 24.2 ms (before the touch path). The signal path, every
change, how to read the panel and the **slow-motion measurement procedure** are in
[docs/latency.md](docs/latency.md).

## Development

### Repository structure

```
expo-juce/
├── src/                     # TypeScript API (index.ts, ExpoJuce.types.ts), web stub, JUCE vendoring scripts
├── ios/                     # Swift module, ObjC bridge, ObjC++ engine, JuceModule_*.mm, podspec
│   └── juce_modules/        # Vendored JUCE 8.0.1 modules
├── android/                 # Kotlin module, JNI + Android host (src/main/cpp), CMakeLists.txt
├── test/harness/            # Offline JUCE graph harness (WSL, CMake)
├── example/                 # Old example app (Expo 51, pinned expo-juce 0.1.1) — stale
├── docs/
│   └── latency.md           # Latency guide and measurement procedure
└── package.json
```

### Build and publish (Windows)

There is no Mac or Xcode on the dev machine. The `expo-module-scripts` lifecycle scripts are
bash and fail under npm on Windows, so build and publish by hand from Git Bash:

```bash
npm install
# 1. bump "version" in package.json, e.g. 0.5.2-diag.2
CI=1 bash node_modules/expo-module-scripts/bin/expo-module-build   # tsc → build/
npm publish --tag next --ignore-scripts --otp=<code>               # npm 2FA code required
```

`--ignore-scripts` skips `prepublishOnly`, so step 1's build is mandatory. Prereleases go
under a dist-tag (`next`, earlier `diag`) so `latest` is not moved.

Then in the consuming app: `npm install expo-juce@<version> --save-exact`, check that
`package-lock.json` still has its macOS-only optional entries (a lockfile regenerated on
Windows can drop them, and EAS's `npm ci` on macOS then fails), and run a new EAS build (EAS
cannot resolve `file:` dependencies, which is why every native change goes through npm).

### Verifying without Xcode

1. **TypeScript:** `npx tsc --noEmit` here and in the consuming app.
2. **Engine C++:** the offline harness compiles the vendored JUCE headless with `Voice`,
   `VoiceAllocator`, `TransportEngine`, `PolySynthProcessor`, `FxChainProcessor` and a copy of
   `GraphPlayer`, replays the `doSetup` order (synth → fx → output) and drives the callback with
   iOS-like block sizes; separate cases check every effect, bypass and echo sync. From `test/harness` in
   WSL (or any Linux shell):
   ```sh
   cmake -S . -B build && cmake --build build -j16 && ./build/harness
   ```
   See [test/harness/README.md](test/harness/README.md) (case `g2` is an expected-FAIL negative
   control). Extend it for every new graph node.
3. **Android host + NDK:** `./build/host_test` (same directory) runs the Android host against a
   fake Oboe-like device. The native library also builds with the NDK directly, without Gradle:
   ```sh
   cmake -S android -B build-android -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake      -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 -DANDROID_STL=c++_shared -DCMAKE_BUILD_TYPE=Release
   cmake --build build-android -j16
   ```
4. **ObjC++ / Swift / Kotlin:** careful review (no compiler locally); the EAS build is the first compile.
5. **Device:** a diagnostics panel built on `getDiagnostics()`
   ([docs/latency.md](docs/latency.md#5-a-diagnostics-panel)), then the slow-motion
   measurement in [docs/latency.md](docs/latency.md#6-slow-motion-measurement-procedure).

### JUCE vendoring

`npm run download-juce` fetches JUCE 8.0.1 into `JUCE/` and `npm run vendor-juce` copies the needed modules into
`ios/juce_modules` (stripping tests and demos; the Android sources stay, the Android build uses them)
and applies the local patches in `src/scripts/juce-patches.cjs`. `npm run check:juce-patches` checks
that every patched vendored file equals pristine JUCE 8.0.1 plus those patches. Each JUCE module is compiled as
its own translation unit (`ios/JuceModule_*.mm`); `ios/JuceConfig.h` holds the module flags and
`JUCE_STANDALONE_APPLICATION 0`.

## Documentation

- [docs/latency.md](docs/latency.md) — latency budget, signal path, changes, panel fields, slow-motion measurement
- [docs/drums.md](docs/drums.md) — drum voices: models, parameters, realtime rules
- [CHANGELOG.md](CHANGELOG.md)
- [test/harness/README.md](test/harness/README.md) — the offline engine harness

## License

expo-juce's own code is MIT (`LICENSE`). It bundles JUCE 8, which is dual licensed under the JUCE
licence or AGPLv3, so an app built with expo-juce needs a JUCE licence (Starter is free below a
revenue limit) unless the whole app is released under AGPLv3. Details, and the other bundled
code (Oboe, libFLAC), are in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
