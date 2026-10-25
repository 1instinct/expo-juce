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
 juce::AudioProcessorGraph  PolySynthProcessor ─► FxChainProcessor ─► audio output node
                            AuxProcessor (metronome + backing track) ───► audio output node (summed)
                            ├─ NoteEventQueue: note-on ring (256) drained at block start; releases as marks
                            ├─ 8 × Voice: dsp::Oscillator → StateVariableTPTFilter (LP) → ADSR
                            └─ TransportEngine (tempo / position / beat crossings)
                            FxChainProcessor: drive → crush → eq → chorus → phaser → echo → reverb →
                            pump → compressor → safety limiter; settings in a process-lifetime store
                            AuxProcessor: clicks rendered at prepare; track buffer swapped in from
                            a process-lifetime BackingTrack holder (decoded on a background queue)
 GraphPlayer also writes the output into an AnalysisRing (getAnalysis: peak / rms / scope)
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
  an effect turned on starts clean (no stale echo or reverb tail). With every effect off the
  output is the synth output, bit for bit (while it stays under the limiter ceiling).
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
ExpoJuce.setMetronome({ enabled: true, style: 'wood', level: 0.7, accent: true, beatsPerBar: 4 });

const info = await ExpoJuce.loadBackingTrack(fileUri, {   // gapless info: optional, MP3 only
  loopSamples: 682839, leadingSamples: 4561, trailingSamples: 1496,
});
// { durationSec, sampleRate, channels, loopStartSample, loopSamples, decodedSamples, decoder }
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
  (true), `beatsPerBar` 1–16 (4).
- **Backing track**: `loadBackingTrack(uri, gapless?)` takes a local file (`file://` URI or
  absolute path; download remote files first). It decodes WAV, AIFF, FLAC and MP3 (plus
  CoreAudio formats such as M4A / AAC on iOS) on a background queue, resamples to the device
  rate and swaps the buffer in without blocking or allocating on the audio thread; the old
  buffer is freed on the main thread after the audio thread has let go of it. Up to 600 s.
  A newer load or `unloadBackingTrack()` supersedes a load in flight (it rejects with
  `ERR_BACKING_TRACK_SUPERSEDED`); other errors: `ERR_BACKING_TRACK_URI`, `_FILE`, `_FORMAT`,
  `_TOO_LONG`.
- **Gapless MP3 loops**: MP3 encoders pad the start and end. With `gapless` (the LAME / Xing
  tag values, frames at the file's rate) the loop region is: decoded length == `loopSamples`
  (the decoder honoured the tag) → `[0, loopSamples)`; decoded length == 1152 + leading +
  loopSamples + trailing (the decoder ignored the tag and decoded the tag frame) → `[1152 +
  leading, +loopSamples)`; anything else → the whole file. The region is cut before
  resampling; `decodedSamples` / `decoder` in the result tell which case applied.
- **Playback** follows the transport: it plays while the transport plays; its position is
  the transport position converted to seconds at the current tempo, counted from the start
  of the bar the transport started in. `setTransportPosition()` seeks the track (5 ms
  crossfade), stopping fades it out (5 ms). With `loop` off it plays once and then stays
  silent until the transport is restarted or moved back. The track is not time-stretched: a
  tempo change while playing moves it to the position the new tempo implies (5 ms crossfade).
- Options: `level` 0–1 (0.8), `loop` (true), `enabled` (true).
- Metronome and backing-track options and the loaded track are kept natively for the life of
  the app: they work before `setup()` and survive engine rebuilds (after a sample-rate change
  the track is resampled again off the audio thread).
- Binaries older than 0.6.0: the setters are no-ops and `loadBackingTrack` rejects with
  `ERR_UNSUPPORTED`. Web: the same.

### Analysis (0.6.0+)

```ts
const a = ExpoJuce.getAnalysis();
// { peak, rms, scope: number[128], lastNote, bpm, beat, playing }
```

Synchronous and cheap enough to call every animation frame. `peak` / `rms` are of the last
output block (synth, effects, metronome and backing track as sent to the device); `scope` is
128 points, oldest first, taken every 16th sample from the last 2048 output samples (mono, may
tear across a block; it is for display). `lastNote` is the last note the synth started (-1 if
none). Zeros when the engine is not running.

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

`hitPolygon` (0.6.0+) makes the touch area non-rectangular: a flat `[x0, y0, x1, y1, …]`
polygon normalised to the view (0–1). Touches outside it (including on its children) pass
through to the views underneath, so pads with overlapping bounding boxes do not steal each
other's touches. Empty, odd-length or fewer than 3 points: the whole view.

```tsx
<JucePadView note={62} hitPolygon={[0.5, 0, 1, 0.5, 0.5, 1, 0, 0.5]} style={{ width: 96, height: 96 }} />
```

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

Synth settings, effect, metronome and backing-track settings (and the loaded track) and transport state (tempo, position, playing) survive the
engine's automatic rebuilds after a media-services reset or a stalled device; held notes do not.

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
   `VoiceAllocator`, `TransportEngine`, `PolySynthProcessor`, `FxChainProcessor` and a copy of
   `GraphPlayer`, replays the `doSetup` order (synth → fx → output) and drives the callback with
   iOS-like block sizes; separate cases check every effect, bypass and echo sync. From `test/harness` in
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
