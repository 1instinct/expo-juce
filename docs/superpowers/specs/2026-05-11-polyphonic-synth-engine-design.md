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
- Fast fade-out (1-2ms) on voice steal to prevent clicks

**VoiceAllocator** — Note-to-voice mapping
- `noteOn(note, velocity)`: assigns to a free voice, or steals the quietest active voice
- `noteOff(note)`: triggers ADSR release on the matching voice
- `allNotesOff()`: releases all voices immediately
- Maintains note→voice index map for O(1) noteOff lookup

**TransportEngine** — Extracted from current codebase, unchanged logic
- Sample-accurate beat tracking, BPM control (20-400)
- Integrated into `PolySynthProcessor::processBlock` timing
- Beat boundary events forwarded to JS via dispatch timer (same ~60Hz polling pattern)

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
setDetune(cents: number): void
setMasterLevel(level: number): void           // 0-1
```

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

Add two JUCE modules to the vendor script and `ios/juce_modules/`:
- `juce_audio_processors` (AudioProcessor, AudioProcessorGraph)
- `juce_audio_devices` (AudioDeviceManager, AudioDeviceType)

## Audio Session

- Category: `playAndRecord` (ready for future mic input)
- Options: `mixWithOthers` + `defaultToSpeaker`
- JUCE's iOS backend handles `AVAudioSessionInterruptionNotification` internally — no manual interruption handling needed
- Sample rate: device default (no hardcoding)

## Thread Safety

All patterns carried forward from the current codebase:

- **Audio thread (processBlock):** No allocations, no locks, no exceptions. Fixed-size voice array. Read atomics for params.
- **Main thread (JS calls):** Write atomics for param changes. Graph topology changes (add/remove nodes) happen on message thread only.
- **Voice array:** Stack-allocated, fixed size 8. No dynamic allocation.
- **Voice stealing:** Fade-out buffer is pre-allocated per voice at `prepare()` time.
- **RAII ownership:** All DSP objects owned by their parent. No raw `new`/`delete` on audio thread.

## Error Handling

- `AudioDeviceManager::initialise()` fails → engine stays in "not ready" state, all JS calls are no-ops with console warnings
- Device disconnected → JUCE fires `changeListenerCallback`, forwarded to JS via `onChange` event
- Voice overflow → stealing handles gracefully, no drops or crashes
- Audio interruption (phone call, Siri) → JUCE handles restart internally

## Milestones

1. **1.1** — Vendor `juce_audio_processors` + `juce_audio_devices`, update build config
2. **1.2** — `BeeperAudioEngine` with JUCE `AudioDeviceManager` + `AudioProcessorGraph`, verify audio output (test tone)
3. **1.3** — `Voice` + `VoiceAllocator` + `PolySynthProcessor` — 8-voice polysynth, block rendering
4. **1.4** — Bridge updates — new TypeScript API, remove old mono API
5. **1.5** — End-to-end device verification — chords, voice stealing, no clicks/crashes

## Testing

- **VoiceAllocator:** Unit-testable with mock voices (note assignment, stealing logic)
- **Audio output:** Manual on-device testing with headphones (no substitute for ears)
- **Regression guard:** Integration test in example app — `setup()` → `noteOn(60, 127)` → wait → verify no crash
