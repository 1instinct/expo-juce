# Copilot Instructions for expo-juce

## Project Overview

**expo-juce** is an Expo module that enables realtime DSP (Digital Signal Processing) with C++ and JUCE in React Native applications. It bridges JavaScript/TypeScript with native audio processing for ultra-low latency audio playback and synthesis.

### Key Technologies
- **Expo Modules API** - Modern React Native native module framework
- **JUCE Framework** - C++ audio framework (v8.0.1)
- **AudioProcessorGraph** - JUCE-owned audio stack (no AVAudioEngine)
- **Swift** - iOS module implementation
- **TypeScript** - API surface and type definitions

## Architecture

### File Structure
```
expo-juce/
├── src/                          # TypeScript API layer
│   ├── index.ts                  # Main exports, event emitters
│   ├── ExpoJuceModule.ts         # Native module bridge
│   ├── ExpoJuceModule.web.ts     # Web fallback implementation
│   ├── ExpoJuceView.tsx          # Native view component
│   ├── ExpoJuce.types.ts         # TypeScript type definitions
│   └── scripts/
│       └── vendor-juce.cjs       # Vendors JUCE modules
├── ios/                          # iOS native implementation
│   ├── ExpoJuceModule.swift      # Expo Module definition
│   ├── ExpoJuceBridge.h/m        # Objective-C wrapper
│   ├── ExpoJuceAudioEngine.h/mm    # Top-level JUCE coordinator
│   ├── PolySynthProcessor.h/mm   # 8-voice AudioProcessor
│   ├── Voice.h/mm                # Single synth voice (osc+filter+ADSR)
│   ├── VoiceAllocator.h/mm       # Note-to-voice mapping + stealing
│   ├── TransportEngine.h/mm      # Beat tracking + BPM
│   ├── JuceConfig.h              # JUCE module configuration
│   ├── JuceModules.mm            # JUCE module includes
│   ├── ExpoJuceView.swift        # Native view
│   ├── ExpoJuce.podspec          # CocoaPods specification
│   └── ExpoJuce-Bridging-Header.h
├── android/                      # Android native (future)
├── example/                      # Test app
└── JUCE/                         # Vendored JUCE modules
```

### Data Flow
```
JavaScript/TypeScript (App.tsx)
    ↓ calls
ExpoJuceModule (index.ts)
    ↓ bridges to
ExpoJuceModule.swift
    ↓ calls
ExpoJuceBridge.m (Obj-C wrapper)
    ↓ calls
ExpoJuceAudioEngine.mm (JUCE coordinator)
    ↓ owns
AudioDeviceManager → AudioProcessorPlayer → AudioProcessorGraph
    ↓ contains
PolySynthProcessor (8-voice polysynth)
    ↓ uses
Voice[8] + VoiceAllocator + TransportEngine
```

## Core Components

### 1. ExpoJuceAudioEngine (`ExpoJuceAudioEngine.mm`)
- **Purpose**: Top-level JUCE audio stack coordinator
- **Technology**: JUCE AudioDeviceManager + AudioProcessorGraph
- **Features**:
  - Singleton, initialized from `setup()`, torn down by `shutdown()`
  - Configures AVAudioSession: `playAndRecord` + `mixWithOthers` + `defaultToSpeaker`
  - Bootstraps JUCE MessageManager on main thread
  - Forwards all synth/transport calls to PolySynthProcessor/TransportEngine

### 2. PolySynthProcessor (`PolySynthProcessor.mm`)
- **Purpose**: 8-voice polyphonic AudioProcessor node in the graph
- **Features**:
  - Note input via `NoteEventQueue` (ios/NoteEventQueue.h): note-on ring (multi-producer, audio-thread consumer) plus release marks that cannot be dropped; owned by the engine for the life of the process
  - Global params as atomics (waveform, filter, detune, master level)
  - processBlock: drains FIFO, renders voices, mixes fade-out tails, duplicates mono→stereo

### 3. Voice (`Voice.mm`)
- **Purpose**: Single synth voice with full signal path
- **Features**:
  - 4 oscillators (sine/square/saw/triangle) with lookup tables
  - SVF lowpass filter with cutoff + resonance
  - Full ADSR envelope
  - captureAndRelease for click-free voice stealing via fade-out tail buffer

### 4. VoiceAllocator (`VoiceAllocator.mm`)
- **Purpose**: Note-to-voice mapping + voice stealing
- **Features**:
  - O(1) note→voice lookup via noteMap[128]
  - Steals quietest active voice when all 8 are busy

### 5. Objective-C Bridge (`ExpoJuceBridge`)
- **Purpose**: Simplify Swift ↔ Objective-C++ interop
- **Why Needed**: Swift cannot directly import Objective-C++ headers
- **Pattern**: Thin wrapper forwarding to ExpoJuceAudioEngine singleton

### 6. Expo Module (`ExpoJuceModule.swift`)
- **Purpose**: Define JavaScript API using Expo Modules DSL
- **Key Patterns**:
  - `OnDestroy` - Cleanup on module unload
  - `AsyncFunction` with `.runOnQueue(.main)` for all audio methods
  - `Events` - `onChange` and `onBeat` event emitters

### 7. TypeScript API (`src/index.ts`)
- **Purpose**: Type-safe JavaScript interface
- **Features**:
  - MIDI-style: `noteOn(note, velocity)` / `noteOff(note)` / `allNotesOff()`
  - Global params: `setWaveform()`, `setFilterCutoff()`, `setFilterResonance()`, ADSR, `setDetune()`, `setMasterLevel()`
  - Transport: `startTransport()`, `stopTransport()`, `setTempo()`, position control
  - Lifecycle: `setup()` / `shutdown()`

## Development Guidelines

### Thread Safety Rules
- **Audio thread (processBlock)**: No allocations, locks, or exceptions. Fixed-size arrays. Read atomics.
- **Main thread (JS calls)**: Write atomics for params. Note events go through lock-free FIFO.
- **Voice state**: Only mutated on audio thread (via FIFO). Never from main thread.
- **Transport**: Non-atomic `beatPosition` on audio thread only. Atomic `positionSnapshot` for main thread reads.

### When Adding New Features

1. **Audio DSP Features**:
   - Add to appropriate class (Voice for per-voice, PolySynthProcessor for global)
   - Keep processBlock lock-free and real-time safe
   - Use atomics for thread-safe parameter changes
   - Use FIFO for state mutations that affect audio thread objects

2. **JavaScript API**:
   - Add TypeScript types in `ExpoJuce.types.ts`
   - Export from `src/index.ts`
   - Implement web fallback in `ExpoJuceModule.web.ts`

3. **Native Bridge**:
   - Add Swift function in `ExpoJuceModule.swift` (AsyncFunction + .runOnQueue(.main))
   - Add Objective-C wrapper in `ExpoJuceBridge.m`
   - Add forwarding in `ExpoJuceAudioEngine.mm`

### Code Style

**Swift**:
- Use Expo Modules DSL patterns
- All audio methods use `AsyncFunction` with `.runOnQueue(.main)`
- Use explicit return types for clarity

**Objective-C/C++**:
- Keep C++ isolated to `.mm` files
- Use modern Objective-C features (`@property`, blocks)
- Follow Apple naming conventions
- Use `inline constexpr` for constants in headers

**TypeScript**:
- Use strict typing
- Export types alongside functions
- Prefer named exports over default
- All synth/transport functions return `Promise<void>`

### Build System

**iOS CocoaPods**:
- `ExpoJuce.podspec` defines module configuration
- 11 JUCE modules vendored (including GUI stack as compile-time deps)
- Frameworks: AVFoundation, CoreGraphics, CoreImage, CoreText, QuartzCore, UIKit, WebKit, Metal, MetalKit
- `CLANG_CXX_LANGUAGE_STANDARD = c++17`

**TypeScript Build**:
- `expo-module build` compiles TypeScript
- Output goes to `build/` directory

**Vendoring**:
- `src/scripts/vendor-juce.cjs` vendors JUCE modules from downloaded source
- Run `node src/scripts/vendor-juce.cjs` after JUCE download

## Testing

### Example App
- Located in `example/`
- Interactive keyboard (C4-C5), waveform selector, parameter sliders
- Run with: `cd example && yarn ios`

### Manual Testing Checklist
- [ ] setup() initializes without errors
- [ ] Notes play with < 10ms latency
- [ ] Polyphonic chords (hold multiple keys)
- [ ] Voice stealing works without clicks
- [ ] All waveforms produce distinct sound
- [ ] Filter cutoff/resonance affect timbre
- [ ] ADSR envelope shapes the sound correctly
- [ ] allNotesOff() silences immediately
- [ ] shutdown() + setup() re-initialization works
- [ ] App survives backgrounding/foregrounding

## Roadmap

See README.md for the 6 sub-project roadmap:
1. Polyphonic Synth Engine (current)
2. Effects Chain
3. Sample Playback / Drum Machine
4. Audio Input Processing
5. Modular Routing
6. Electron / Desktop

## Resources

- [Expo Modules API](https://docs.expo.dev/modules/module-api/)
- [JUCE Documentation](https://juce.com/learn/documentation)
- [Real-Time Audio Programming](https://www.rossbencina.com/code/real-time-audio-programming-101-time-waits-for-nothing)

## Notes for AI Assistants

- Always preserve lock-free audio thread characteristics
- Avoid allocations in processBlock
- Use FIFO for note events, atomics for params
- Test on real hardware, not simulators
- Swift/Objective-C++ requires bridging - use wrapper pattern
- Module lifecycle (OnDestroy) is critical for audio cleanup
