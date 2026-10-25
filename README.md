# expo-juce

Realtime DSP w/C++ & JUCE

expo-juce embeds [JUCE](https://juce.com) 8 in an Expo / React Native app as an Expo
module: a JUCE-owned audio device drives an `AudioProcessorGraph` with an 8-voice
polyphonic synth and a transport, controlled from JS through synchronous JSI calls.
iOS only for now; the Android and web modules are scaffolds.

> **Branches.** `main` is a stale scaffold. Current work is on `@aaron.smulktis/develop`;
> the diagnostics and latency work is on `debug/audio-diagnostics`.

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
 juce::AudioProcessorGraph  PolySynthProcessor ─► audio output node
                            ├─ NoteEventQueue: note-on ring (256) drained at block start; releases as marks
                            ├─ 8 × Voice: dsp::Oscillator → StateVariableTPTFilter (LP) → ADSR
                            └─ TransportEngine (tempo / position / beat crossings)
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
**Status:** Planned

Add `AudioProcessor` nodes for standard effects: reverb, delay, distortion, EQ, compressor. Inserted into the graph after the synth node on a master effects bus. Each effect is an independent processor — can be bypassed, reordered, or removed.

### Sub-project 3: Sample Playback / Drum Machine
**Status:** Planned — an earlier JUCE sampler prototype is the starting point

`SamplerProcessor` node that loads audio files and triggers playback via `noteOn`. Supports multi-sample mapping (e.g., different samples per MIDI note). Integrates with the transport for sequenced playback.

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

Synth settings and transport state (tempo, position, playing) survive the engine's automatic
rebuilds after a media-services reset or a stalled device; held notes do not.

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
├── android/                 # Kotlin scaffold (no audio engine yet)
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
npm publish --tag diag --ignore-scripts                            # npm 2FA code required
```

`--ignore-scripts` skips `prepublishOnly`, so step 1's build is mandatory. Prereleases go
under a dist-tag (`diag`) so `latest` is not moved.

Then in the consuming app: `npm install expo-juce@<version> --save-exact`, check that
`package-lock.json` still has its macOS-only optional entries (a lockfile regenerated on
Windows can drop them, and EAS's `npm ci` on macOS then fails), and run a new EAS build (EAS
cannot resolve `file:` dependencies, which is why every native change goes through npm).

### Verifying without Xcode

1. **TypeScript:** `npx tsc --noEmit` here and in the consuming app.
2. **Engine C++:** the offline harness compiles the vendored JUCE headless with `Voice`,
   `VoiceAllocator`, `TransportEngine`, `PolySynthProcessor` and a copy of `GraphPlayer`, replays the
   `doSetup` order and drives the callback with iOS-like block sizes. From `test/harness` in
   WSL (or any Linux shell):
   ```sh
   cmake -S . -B build && cmake --build build -j16 && ./build/harness
   ```
   See [test/harness/README.md](test/harness/README.md) (case `g2` is an expected-FAIL negative
   control). Extend it for every new graph node.
3. **ObjC++ / Swift:** careful review (no compiler locally); the EAS build is the first compile.
4. **Device:** a diagnostics panel built on `getDiagnostics()`
   ([docs/latency.md](docs/latency.md#5-a-diagnostics-panel)), then the slow-motion
   measurement in [docs/latency.md](docs/latency.md#6-slow-motion-measurement-procedure).

### JUCE vendoring

`npm run download-juce` fetches JUCE 8.0.1 into `JUCE/` and `npm run vendor-juce` copies the needed modules into
`ios/juce_modules` (stripping tests, demos and non-iOS code). Each JUCE module is compiled as
its own translation unit (`ios/JuceModule_*.mm`); `ios/JuceConfig.h` holds the module flags and
`JUCE_STANDALONE_APPLICATION 0`.

## Documentation

- [docs/latency.md](docs/latency.md) — latency budget, signal path, changes, panel fields, slow-motion measurement
- [test/harness/README.md](test/harness/README.md) — the offline engine harness

## License

MIT
