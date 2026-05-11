# Polyphonic Synth Engine — Design Spec

**Date:** 2026-05-11
**Sub-project:** 1 of 6 (see README roadmap)
**Status:** Draft

## Overview

Replace the current mono `SynthEngine` + `AVAudioEngine` architecture with a JUCE-owned audio stack built around `AudioProcessorGraph`. The first node in the graph is an 8-voice polyphonic synthesizer with per-voice DSP, MIDI-style note control, and global parameter management.

This is the foundation for a full audio workstation — every future feature (effects, sampler, audio input, modular routing) plugs into the same graph as additional `AudioProcessor` nodes.

## Goals

- JUCE owns the entire audio stack (device management, rendering, output) — no `AVAudioEngine`
- 8-voice polyphonic synth with per-voice oscillator, filter, and ADSR envelope
- MIDI-style TypeScript API: `noteOn(note, velocity)` / `noteOff(note)`
- Cross-platform ready (iOS now, macOS/Windows/Linux via Electron later)
- Clean architecture that scales to DAW-level complexity

## Architecture

### Audio Stack

```
JUCE AudioDeviceManager
  (owns CoreAudio device, sample rate, buffer size)
  │
  └── AudioProcessorPlayer
        (bridges device audio callback → graph)
        │
        └── AudioProcessorGraph
              │
              ├── [PolySynthProcessor] ──→ [Audio Output Node]
              │    (8 voices, voice allocator)
              │
              └── (future nodes: effects, sampler, audio input)
```

### Component Breakdown

**BeeperAudioEngine** — Top-level coordinator (replaces `JuceToneGenerator`)
- Owns `AudioDeviceManager`, `AudioProcessorPlayer`, `AudioProcessorGraph`
- Singleton, initialized from `setup()`, torn down by `shutdown()`
- Configures audio session: `playAndRecord` category with `mixWithOthers` + `defaultToSpeaker`
- Exposes methods forwarded from the ObjC bridge

**PolySynthProcessor** — Custom `AudioProcessor` subclass
- Contains 8 `Voice` instances and a `VoiceAllocator`
- `processBlock(AudioBuffer, MidiBuffer)`: renders all active voices, sums into output buffer
- Holds global params as atomics (waveform, filter cutoff/reso, ADSR, detune, master level)
- Global params are read at block boundaries, not per-sample

**Voice** — Single synth voice
- Owns: `juce::dsp::Oscillator<float>` (4 waveform lookup tables), `juce::dsp::StateVariableTPTFilter<float>`, `juce::ADSR`
- Tracks: `noteNumber` (-1 = free), `velocity`, `currentLevel` (envelope x velocity, used for stealing)
- `renderBlock(AudioBuffer, int numSamples)`: renders its block using current global params
- `startFadeOut(int samples)`: triggers a linear fade-out for voice stealing

**VoiceAllocator** — Note-to-voice mapping
- `noteOn(note, velocity)`: assigns to a free voice, or steals the quietest active voice
- `noteOff(note)`: triggers ADSR release on the matching voice
- `allNotesOff()`: releases all voices immediately
- Maintains note→voice index map for O(1) noteOff lookup

**TransportEngine** — Extracted from current codebase, unchanged logic
- Owned by `BeeperAudioEngine` (not by PolySynthProcessor)
- Sample-accurate beat tracking, BPM control (20-400)
- `advance()` called from `PolySynthProcessor::processBlock` via pointer (BeeperAudioEngine passes transport pointer to synth at setup time)
- Beat boundary events forwarded to JS via dispatch timer (same ~60Hz polling pattern)
- For this sub-project, transport only provides beat timing to JS — it does not drive note playback (sequencer is a future sub-project)

### Bridge Stack

```
ExpoJuceModule.swift          (Expo Module definitions)
  → ExpoJuceBridge.m          (ObjC static methods — thin forwarding)
    → BeeperAudioEngine.mm    (C++ JUCE stack)
       ├── AudioDeviceManager
       ├── AudioProcessorPlayer
       ├── AudioProcessorGraph
       └── PolySynthProcessor
            ├── Voice[8]
            └── VoiceAllocator
```

## TypeScript API

### Lifecycle

```typescript
setup(): void       // Initialize JUCE AudioDeviceManager + graph
shutdown(): void    // Tear down audio
```

**Engine state machine:**
- `uninitialized` → `setup()` → `ready`
- `ready` → `shutdown()` → `uninitialized`
- `ready` → `setup()` → no-op (already initialized)
- `uninitialized` → any synth/transport call → no-op with console warning

**`shutdown()` behavior:**
1. Calls `allNotesOff()` (immediate release of all voices)
2. Removes audio callback from `AudioDeviceManager`
3. Closes `AudioDeviceManager` (releases CoreAudio device)
4. Destroys graph, processor, player
5. Engine returns to `uninitialized` state
6. `setup()` can be called again to re-initialize

### Polyphonic Synth (MIDI-style)

```typescript
noteOn(note: number, velocity: number): void  // note: 0-127, velocity: 0-127
noteOff(note: number): void
allNotesOff(): void                           // panic — release all voices
```

### Global Synth Params

```typescript
setWaveform(waveform: 'sine' | 'square' | 'saw' | 'triangle'): void
setFilterCutoff(hz: number): void             // 20-20000
setFilterResonance(q: number): void           // 0-1
setAttack(ms: number): void
setDecay(ms: number): void
setSustain(level: number): void               // 0-1
setRelease(ms: number): void
setDetune(cents: number): void                // global pitch offset applied to all voices
setMasterLevel(level: number): void           // 0-1
```

### Parameter Validation

All parameters are clamped silently — no errors thrown. Out-of-range values are clamped to nearest valid value.

| Parameter | Min | Max | Default | Unit |
|-----------|-----|-----|---------|------|
| note | 0 | 127 | — | MIDI note |
| velocity | 0 | 127 | — | MIDI velocity |
| waveform | — | — | sine | enum |
| filterCutoff | 20 | 20000 | 20000 | Hz |
| filterResonance | 0 | 1 | 0 | normalized |
| attack | 1 | 10000 | 10 | ms |
| decay | 1 | 10000 | 0 | ms |
| sustain | 0 | 1 | 1 | normalized |
| release | 1 | 10000 | 200 | ms |
| detune | -1200 | 1200 | 0 | cents |
| masterLevel | 0 | 1 | 0.5 | normalized |
| bpm | 20 | 400 | 120 | BPM |

### Transport

```typescript
startTransport(): void
stopTransport(): void
setTempo(bpm: number): void
setTransportPosition(beat: number): void
getTransportPosition(): number
getTempo(): number
isTransportPlaying(): boolean
```

### Events

```typescript
addBeatListener(cb: (event: { beat: number, bpm: number }) => void): Subscription
addChangeListener(cb: (event: ChangeEventPayload) => void): Subscription
```

### Added (new in this version)

| Function | Description |
|----------|-------------|
| `noteOn(note, velocity)` | MIDI-style polyphonic note trigger |
| `noteOff(note)` | Release note by MIDI number |
| `allNotesOff()` | Panic — release all voices |
| `setDecay(ms)` | ADSR decay time |
| `setSustain(level)` | ADSR sustain level (0-1) |
| `setMasterLevel(level)` | Master output level (0-1) |
| `shutdown()` | Tear down audio engine |

### Removed from v0.3.1

| Removed | Replaced by |
|---------|------------|
| `playTone(freq, duration)` | `noteOn(note, velocity)` + `noteOff(note)` |
| `setFrequency(freq)` | `noteOn` with MIDI note number |
| `setLevel(level)` | `velocity` param + `setMasterLevel()` |
| `setHarmonicLevels(levels)` | Deferred to future additive synth sub-project |
| `noteOn()` / `noteOff()` (no-arg) | `noteOn(note, velocity)` / `noteOff(note)` |
| `setDSPValue(param, value)` | Explicit typed setters |

## File Structure

### New Files

| File | Purpose |
|------|---------|
| `ios/BeeperAudioEngine.h` | Top-level engine header |
| `ios/BeeperAudioEngine.mm` | AudioDeviceManager + Graph + Player |
| `ios/PolySynthProcessor.h` | AudioProcessor with 8 voices |
| `ios/PolySynthProcessor.mm` | processBlock, voice mixing |
| `ios/Voice.h` | Single voice (osc + filter + ADSR) |
| `ios/Voice.mm` | Voice rendering |
| `ios/VoiceAllocator.h` | Note-to-voice mapping + stealing |
| `ios/VoiceAllocator.mm` | Allocation logic |
| `ios/TransportEngine.h` | Extracted transport |
| `ios/TransportEngine.mm` | Transport logic (unchanged) |

### Modified Files

| File | Change |
|------|--------|
| `ios/ExpoJuceModule.swift` | New function signatures |
| `ios/ExpoJuceBridge.h/.m` | Forward to BeeperAudioEngine |
| `ios/JuceConfig.h` | Config for juce_audio_processors + juce_audio_devices |
| `ios/JuceModules.mm` | Include new JUCE modules |
| `src/index.ts` | New API surface |
| `src/ExpoJuceModule.ts` | New native method declarations |
| `src/ExpoJuce.types.ts` | Updated types |
| `src/scripts/vendor-juce.cjs` | Add new modules to vendor list |

### Deleted Files

| File | Reason |
|------|--------|
| `ios/JuceToneGenerator.h` | Replaced by BeeperAudioEngine |
| `ios/JuceToneGenerator.mm` | Replaced by BeeperAudioEngine |

### Vendoring

The full transitive dependency chain requires vendoring these new JUCE modules (in addition to the 6 already vendored):

| Module | Why | Key dependency |
|--------|-----|----------------|
| `juce_events` | Required by all below; provides MessageManager | juce_core |
| `juce_graphics` | Required by juce_gui_basics | juce_events |
| `juce_gui_basics` | Required by juce_gui_extra | juce_graphics, juce_data_structures |
| `juce_gui_extra` | Required by juce_audio_processors | juce_gui_basics |
| `juce_audio_processors` | AudioProcessor, AudioProcessorGraph | juce_gui_extra, juce_audio_basics |
| `juce_audio_devices` | AudioDeviceManager | juce_audio_basics, juce_events |

Total after vendoring: 12 JUCE modules (up from 6). The GUI modules are not used for rendering — they are compile-time dependencies of `juce_audio_processors`.

iOS system frameworks added: CoreGraphics, CoreImage, CoreText, QuartzCore, UIKit, Cocoa, WebKit, Metal, MetalKit, CoreAudio, CoreMIDI, AudioToolbox, UserNotifications.

## JUCE MessageManager Integration

The current `JuceConfig.h` explicitly disables `juce_events` with the comment "no juce_events" because `MessageManager` conflicts with React Native's app lifecycle. Enabling the full JUCE stack requires resolving this.

**Strategy:** Initialize `MessageManager` on the main thread during `setup()`, before creating the `AudioDeviceManager`. On iOS, JUCE's `MessageManager` attaches to the existing `CFRunLoop` — it does not create its own run loop or fight UIKit for control. The key constraint is that `MessageManager::getInstance()` must be called from the main thread so that JUCE recognizes the main thread as the "message thread."

**Bootstrap sequence in `BeeperAudioEngine::setup()`:**
1. Assert we're on the main thread
2. `juce::MessageManager::getInstance()` — creates singleton, attaches to current runloop
3. Create `AudioDeviceManager`
4. `AudioDeviceManager::initialise(0, 2, ...)` — 0 inputs, 2 outputs (stereo)
5. Create `AudioProcessorGraph`, add `PolySynthProcessor` node, connect to output
6. Create `AudioProcessorPlayer`, set processor to graph
7. `AudioDeviceManager::addAudioCallback(&player)`

**Why this is safe:** JUCE's iOS `MessageManager` implementation (`juce_MessageManager_ios.mm`) uses `CFRunLoopSourceRef` on the main runloop — the same runloop React Native uses. It does not call `UIApplicationMain` or take over the app lifecycle. It simply posts callbacks to the existing runloop. The risk of conflict is low as long as we never call `MessageManager::runDispatchLoop()` (which we won't — React Native owns the runloop).

**`JuceConfig.h` changes:**
- Enable all previously disabled modules: `juce_events`, `juce_data_structures`, `juce_gui_basics`, `juce_graphics`, `juce_gui_extra`, `juce_audio_processors`, `juce_audio_devices`
- Add `JUCE_APPLICATION_NAME` and `JUCE_APPLICATION_VERSION` defines
- Keep `JUCE_STANDALONE_APPLICATION 0`
- Add `JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP 0` (prevent JUCE from trying to create a standalone app window)

## Audio Session

- Category: `playAndRecord` (ready for future mic input)
- Options: `mixWithOthers` + `defaultToSpeaker`
- JUCE's iOS backend handles `AVAudioSessionInterruptionNotification` internally — no manual interruption handling needed
- Sample rate: device default (no hardcoding)
- Channel config: voices render mono, output is duplicated to stereo in `processBlock`

## Thread Safety

All patterns carried forward from the current codebase:

- **Audio thread (processBlock):** No allocations, no locks, no exceptions. Fixed-size voice array. Read atomics for params.
- **Main thread (JS calls):** Write atomics for param changes. Graph topology changes (add/remove nodes) happen on message thread only.
- **Voice array:** Stack-allocated, fixed size 8. No dynamic allocation.
- **Voice stealing:** Fade-out buffer is pre-allocated per voice at `prepare()` time.
- **RAII ownership:** All DSP objects owned by their parent. No raw `new`/`delete` on audio thread.

### Voice Stealing Algorithm

When all 8 voices are active and a new `noteOn` arrives:

1. **Select victim:** Scan all voices, pick the one with the lowest `currentLevel` (envelope gain x velocity). Ties broken by oldest note.
2. **Trigger fade-out:** Call `victim.startFadeOut(88)` — 88 samples at 44100Hz = ~2ms linear ramp to zero. The fade-out counter is stored per voice.
3. **Render overlap:** During the current `processBlock`, the victim continues rendering with its fade-out applied. The new note starts its attack simultaneously. Both voices produce output in the same block — they are summed together.
4. **Reclaim:** Once the fade-out counter reaches zero, the victim voice is marked free and stops rendering. The new voice is already playing.

At 48kHz with a 512-sample block (~10.7ms), a 2ms fade-out is ~96 samples — always completes within one block.

## Error Handling

- `AudioDeviceManager::initialise()` fails → engine stays in "not ready" state, all JS calls are no-ops with console warnings
- Device disconnected → JUCE fires `changeListenerCallback`, forwarded to JS via `onChange` event
- Voice overflow → stealing handles gracefully, no drops or crashes
- Audio interruption (phone call, Siri) → JUCE handles restart internally

## Milestones

1. **1.1** — Vendor 6 new JUCE modules (`juce_events`, `juce_graphics`, `juce_gui_basics`, `juce_gui_extra`, `juce_audio_processors`, `juce_audio_devices`), update `JuceConfig.h`, `JuceModules.mm`, and vendor script. Verify project compiles.
2. **1.2** — `BeeperAudioEngine` with MessageManager bootstrap, `AudioDeviceManager` + `AudioProcessorGraph`, verify audio output (test tone through graph)
3. **1.3** — `Voice` + `VoiceAllocator` + `PolySynthProcessor` — 8-voice polysynth, block rendering
4. **1.4** — Bridge updates — new TypeScript API, remove old mono API
5. **1.5** — End-to-end device verification — chords, voice stealing, no clicks/crashes

## Testing

- **VoiceAllocator:** Unit-testable with mock voices (note assignment, stealing logic)
- **Audio output:** Manual on-device testing with headphones (no substitute for ears)
- **Regression guard:** Integration test in example app — `setup()` → `noteOn(60, 127)` → wait → verify no crash
