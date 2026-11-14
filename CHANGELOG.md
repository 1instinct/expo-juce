# Changelog

## 0.10.0-rc.0

### Added (iOS)

- **Sampler.** 16 sample slots (`0`–`15`) on 32 preallocated stereo voices, mixed with the synth
  and the drums before the effects chain. Pads play a slot at its root note (`samplerHit`,
  `samplerRelease`); keys play every loaded slot whose key zone holds the note (`samplerNoteOn`,
  `samplerNoteOff`). Per slot: root note, key zone, start / end, a loop crossfaded over 5 ms,
  one-shot or gated, gain, pan, tune and fine tune, linear attack / release, velocity
  sensitivity and choke groups 1–8. Pitch by playback rate with 4-point Hermite interpolation.
  Voice stealing takes the voice released earliest, else the oldest; stolen voices fade over 64
  samples. Sampler-wide `level`, `mute` and `polyphony` (1–32). See `docs/sampler.md`.
- `loadSamplerSlot(slot, uri, params?)`: decodes a local file (WAV, AIFF, FLAC, MP3, CoreAudio
  formats) and resamples it to the device rate on a loader queue, then publishes it by an atomic
  pointer swap with a hazard pointer, like the backing track; replaced samples are freed on the
  main thread once the audio thread has moved past them. Up to 60 s per sample and 256 MB across
  slots. A later load or unload of the slot supersedes a load in flight. Resolves with
  `{ frames, sampleRate, channels, durationMs }`; rejects with `ERR_SAMPLER_SLOT`,
  `ERR_SAMPLER_FILE`, `_FORMAT`, `_TOO_LONG`, `_MEMORY` or `_SUPERSEDED`.
- `hasSampler`, `unloadSamplerSlot`, `setSamplerSlot`, `setSampler`, `getSamplerState`, and the
  exports `SAMPLER_SLOT_COUNT`, `SAMPLER_VOICE_COUNT`, `SAMPLER_SLOT_DEFAULTS`, `SAMPLER_DEFAULTS`
  with the types `SamplerSlotParams`, `SamplerLoop`, `SamplerSettings`, `SamplerSlotState`,
  `SamplerState`, `SamplerLoadResult`.
- `JucePadView` `mode="sampler"`: every touch-down hits slot `note` (a second finger retriggers);
  the last touch-up releases it.
- The note queue carries sampler pad hits and key note-ons with their own release marks;
  `allNotesOff` releases them too.
- Diagnostics: `samplerHits`, `samplerVoices`, `samplerPeak`, `samplerBytes`.
- Harness cases `sm`, `smb` (32 voices: about 23 µs per 128-frame block at 48 kHz on a desktop
  core, budget 60), `sml`, `sms`, `smq`, `smd`.

### Changed

- The backing track's resampler uses the shared `ios/Hermite.h` (output unchanged, sample for
  sample).

### Android

- Stubs only: `hasSampler()` is `false`, `loadSamplerSlot` rejects with `ERR_UNSUPPORTED`, the
  setters return `false` and `getSamplerState()` returns the defaults (`DefaultShapes.SAMPLER`).
  Diagnostics carry the sampler keys (zeros).

### Compatibility

- New entry points and optional diagnostics keys only. Without sampler calls the output is
  unchanged: an idle sampler does not touch the buffer.
- New native code: apps must rebuild the binary (and bump their runtime version for OTA updates).

## 0.9.0-rc.4

### Added (iOS)

- **Display-rate hint.** `setDisplayRateHint(hz)`: above 60, on a screen that goes faster
  (`maximumFramesPerSecond`), a main-thread `CADisplayLink` asks for
  `preferredFrameRateRange` (80, hz, hz) clamped to the screen's maximum, which keeps a ProMotion
  display at that rate. UIKit delivers touches on display frames, so this is for testing whether
  touch delivery gets shorter at 120 Hz. 0 or 60 removes it. The hint does not need `setup()`,
  survives engine rebuilds and is cleared when a module is created (launch, reload) and when the
  module that set it goes away. On iPhone the app
  must also set `CADisableMinimumFrameDurationOnPhone` in its Info.plist. Returns true when taken;
  false for a non-finite value, on Android (a no-op) and on web.
- The hint shares the stall probe's display link. That link now runs while the engine is set up
  or a hint above 60 is set, pauses while the app is inactive (and starts paused when the app is
  not active), and without a hint above 60 is held to 60 on screens that go faster, so the probe
  alone never raises the display rate.
- Diagnostics: `displayHz` (the link's measured rate: median of `targetTimestamp - timestamp`
  over its last 32 ticks, 0 while it is not running), `maxFramesPerSecond`, `displayRateHint`.
- **Stall cadence.** The stall probe keeps a fixed ring of the last 32 main-thread gaps over
  50 ms: `mainThreadStallLog` (`[{ atSec, ms }]`, the start of each gap on the
  `CACurrentMediaTime()` clock and its length) and `mainThreadStallIntervalP50Ms` (median time
  between consecutive stall starts), so a periodic main-thread job shows as a steady interval.
  `clockSec` is `CACurrentMediaTime()` at the snapshot, to place the log in time. Preallocated,
  no allocation or lock in the display-link callback.
- Harness cases `rate` (hint range, measured rate) and `stlog` (stall log, median interval).

### Compatibility

- New entry point and optional diagnostics keys only. Apps that never call `setDisplayRateHint`
  keep the stall probe at 60 Hz, as before.

## 0.9.0-rc.3

### Fixed (iOS)

- **0.9.0-rc.1 and 0.9.0-rc.2 do not compile for iOS.** `ExpoJuceAudioEngine.mm` failed with
  "cannot jump from switch statement to this case label" in `-handleSessionRouteChange:`: under
  ARC, a block literal capturing a `__weak` variable opens a cleanup scope, and the later `case`
  labels jumped past it. The case bodies holding blocks are now braced. Use rc.3 or later.
- CI compiles every `ios/*.mm` and `ios/*.m` for the iOS simulator (`clang++ -fsyntax-only` with
  the podspec's flags) on pull requests and pushes to `main`.

## 0.9.0-rc.2

### Added (iOS)

- **MIDI byte input that plays without JavaScript.** For MIDI that reaches the app's own native
  code (a Bluetooth notification callback, a serial port): that code calls the C function
  `expojuce_midi_input_bytes(bytes, length, format, arrivalSeconds)` (`ios/ExpoJuceMidiInput.h`)
  on its own thread. The engine parses the bytes (`bleMidi`: one BLE-MIDI packet per call;
  `rawStream`; `rawPackets`: each call must be complete MIDI, anything else is ignored whole) and a
  mapped note-on or drum hit goes straight onto the note queue. The 128-entry input map and
  velocity curve come from JS (`setMidiInputMap`, swapped atomically; `setMidiInputEnabled`).
  A note-off releases what its note-on played whatever the map says by then; synth notes two
  inputs share are counted; a note another source of the engine holds (`noteOn` from JS, a
  `JucePadView`) is left to that source's release.
- Every parsed message reaches JS (`addMidiInputListener`, event `onMidiInput`) in batches about
  4 ms after the first of a burst, with its arrival time, the release epoch, and whether it was
  played (`part`, `out`, `outVelocity`, `last`, `synthetic`). Anything not played is the app's
  to handle.
- `releaseMidiInputHolds()` ends the input's holds (its device went away) and returns the new
  release epoch; `allNotesOff()` also ends them. `getMidiInputClock()` is the clock of
  `arrival`. `midiInputBytes(bytes, format, arrival?)` feeds the same path from JS.
  `hasMidiInput()` is false on older binaries and on Android.
- `+[ExpoJuceBridge midiInputBytesFunction]` returns the entry point through the Objective-C
  runtime, for code that cannot `dlsym` it (an executable whose global symbols are stripped).
- Diagnostics: `midiInput` (`enabled`, `mapVersion`, `packets`, `messages`, `played`, `passed`,
  `dropped`, `rejectedPackets`, `eventsDropped`, `held`, `epoch`, `midiInputToRenderMs`) and
  `touchLatency.midiInputToRenderMs`: bytes' arrival → the render block that consumed the note.
- `rawPackets`: a packet rejected as not complete MIDI still releases the input's holds through
  the complete note-offs in it (`8n kk vv`, `9n kk 00`); its note-ons never play.
- `midiInputBytes` returns -1 for more than 512 bytes (one call is one packet; never truncated).
- Reload safety: the module's MIDI input handler and the engine's lifetime are claimed with tokens,
  so a module destroyed after its replacement was created (an `expo-updates` reload, a dev reload)
  no longer clears the new handler, stops its beat timer or shuts the engine down under it; it only
  releases held notes. Events are not built while no JS listener is subscribed.
- The input's producer lock is an `os_unfair_lock` on Apple platforms (priority donation).
- Harness cases `mip`, `mim`, `mih`, `mic`, `own`.

### Compatibility

- New entry points only. Nothing plays from the input until the app enables the map, so apps
  that never call the new functions behave as before.

## 0.9.0-rc.1

### Added (iOS)

- **Touch latency per leg.** `getDiagnostics().touchLatency`: `touchDeliveryMs`
  (`UITouch.timestamp` → `touchesBegan`), `touchHandlerMs` (`touchesBegan` → note queue),
  `queueToRenderMs` and `touchToRenderMs`, each `last` / `p50` / `p95` / `max` / `count` over the
  last 64 notes and drum hits started by a `JucePadView`, plus `bufferMs` and `outputLatencyMs`.
  Lock-free single-writer windows; no lock or allocation on the audio thread.
  `lastTouchToRenderMs` is unchanged.
- **Touch path report.** `touchPath`: the gesture recognizers (class, owning view, enabled,
  `delaysTouchesBegan`, `delaysTouchesEnded`, `cancelsTouchesInView`) and scroll views
  (`delaysContentTouches`, `canCancelContentTouches`) between the most recently attached pad and
  its window, at most 30. `lastTouch`: the recognizers the last pad touch was delivered to, its
  delivery time and distance to the nearest screen edge (at most one report every 0.5 s, after
  the note is queued).
- **Main-thread stall probe.** A display link on the main run loop while the engine is set up:
  `mainThreadMaxGapMs` (longest gap in the last 10 s) and `mainThreadStalls50` (gaps over 50 ms).
- **Device format.** `hardwareSampleRate`, `requestedSampleRate`, `grantedSampleRate`,
  `requestedBufferSize`, `grantedBufferSize`, `availableBufferSizes`, `deviceOpens` (the last 8
  opens) and `xrunCount` (since the last `setup()`, over every device opened).
- Harness cases `lat`, `stall`, `bsel`, `broute`.

### Changed (iOS)

- **The engine runs at the session's current sample rate** (read after activation, before the
  device is created: the route's own rate on a fresh session) instead of forcing 48 kHz;
  `setup({ sampleRate })` is now a hint used only when the session reports no music rate. The IO
  buffer duration is computed at that rate. After a route change the rate changes only when the
  new route cannot run the current one.
- **Recoveries keep the preferred buffer size.** The dead-device restart, the stopped-device
  restart, the interruption recovery and route changes reapply the session's preferred rate and
  IO buffer duration, and reopen the device with the nearest listed buffer size. Before, they went
  through JUCE's `restartLastAudioDevice`, which fell back to JUCE's default 256 frames (+1,
  rounded up by the route, e.g. 288 at 44.1 kHz) because the last granted size is not in the new
  device's list. After a route change the device is asked again only when the nearest listed size
  differs from the one it was opened with; a request equal to the running setup reopens nothing.

### Compatibility

- New native entry points only (`padNoteOn` / `padDrumHit` with an entry time, the touch path
  reports); the existing ones are unchanged. Every new diagnostics key is optional in
  `AudioDiagnostics` and absent on older binaries and on Android.

## 0.9.0-rc.0

### Added

- **Android audio (first slice).** The Android module now builds the engine with the NDK (JUCE 8 +
  its vendored Oboe + the engine units shared with iOS) and plays through JUCE's Oboe device:
  `setup` / `shutdown`, notes, the nine synth setters, `playTestTone` and `getDiagnostics`.
  The rest of the API is present with the iOS defaults shape (setters return `false`, getters
  return the defaults, `loadBackingTrack` rejects with `ERR_UNSUPPORTED`, `hasDrums()` and
  `isJucePadViewAvailable()` are `false`). See the README's Android section.
- Android closes the audio device while the app is in the background and reopens it on return.
- If the Android native library cannot be loaded, the getters return the same shapes as when it is
  (generated `DefaultShapes.kt`, checked by the harness), with `lastError` saying why.
- `getDiagnostics()` on Android: `platform`, `nativeSampleRate`, `xrunCount`, `backgrounded`,
  `backgroundStops`, `foregroundRestarts`.
- `npm run check:juce-patches`: the patched vendored JUCE files equal pristine 8.0.1 plus
  `src/scripts/juce-patches.cjs`. The lazy MIDI device-list connection (0.8.0-rc.4) is now one of
  those patches, so a re-vendor keeps it.
- Harness: `host_test`, the Android host against a fake Oboe-like device.

### Changed

- **Licensing is stated in full.** Added `LICENSE` (MIT, for expo-juce's own code) and
  `THIRD_PARTY_NOTICES.md` (JUCE 8: the JUCE licence or AGPLv3; Oboe: Apache 2.0; libFLAC: BSD).
  `package.json` `license` is now `MIT AND (AGPL-3.0-only OR LicenseRef-JUCE-8-EULA)`, and both
  files ship in the package. No code change.

### Unchanged

- iOS: the only shared-source change is `ios/JuceConfig.h` enabling Oboe under `__ANDROID__`.

## 0.8.0-rc.3

### Added

- **Backing tracks follow the tempo.** `loadBackingTrack(uri, gapless, { sourceBpm })` takes the
  file's own tempo. The track then follows the transport tempo at its own pitch (time stretch,
  WSOLA over the resident buffer: two read heads at normal speed, a 20 ms crossfade, each new
  head placed by the transport beat and moved up to 10 ms to the place that best continues the
  sound). Every beat of the track stays on the transport's beat: a click lands within about 10 ms
  of it between 0.8× and 1.25× (offline harness, real music), errors never add up, and a tempo
  change while playing holds from the next grain. Seeks, starts and resumes map onto the track's
  own beat grid. No allocation and no lock on the audio thread.
- `BackingTrackInfo.sourceBpm`: the tempo the track follows from (0 if none). `getDiagnostics()`:
  `backingTrackTempoRatio` (transport tempo / track tempo, 1 = untouched).
- Harness case `btf`.

### Unchanged

- At exactly its own tempo a track is the file, sample for sample (no stretch runs), and a track
  loaded without `sourceBpm` plays as before: at its own speed whatever the tempo.

### Compatibility

- The tempo travels inside the `gapless` dictionary, so the native signature is unchanged: this
  JS on an older binary loads the track as before (no stretch), and `sourceBpm` is then missing
  from the result. Use its presence, not `typeof`, to tell whether the binary follows the tempo.

## 0.8.0-rc.2

### Added

- **Mid tom**, a ninth drum voice at index 8: the tom model (sine with a small pitch drop and an
  attack-noise click) at 119.4 Hz, the log-centre of the low (95 Hz) and high (150 Hz) toms,
  with a 0.45 s decay; the same `tune` / `decay` / `tone` / `level` parameters as every voice.
  `drumHit(8, v)`, `setDrumVoice(8, …)` and `JucePadView` `mode="drums"` `note={8}` play and set
  it; `getDrumState().voices` has 9 entries; `DRUM_VOICE_COUNT` is 9. A mid tom hit chokes
  nothing, and the closed hat still chokes only the open hat.

### Compatibility

- Indices 0–7 are unchanged, so saved hits and kit settings for them stay valid.
- Binaries from 0.8.0-rc.0 / rc.1 have 8 voices: `getDrumState().voices.length` is 8, voice 8
  is ignored (`drumHit` does nothing, `setDrumVoice` returns `false`). Use that length, not
  `DRUM_VOICE_COUNT`, to tell whether the mid tom exists in the running binary.

## 0.8.0-rc.1

### Fixed

- iOS build: `Arpeggiator.h`, `SpectrumAnalyser.h` and `DrumMachine.h` are now private headers.
  As public headers they went into the module umbrella and were compiled as Objective-C, so
  `<algorithm>` was not found and the `ExpoJuce` module failed to build (0.7.0-rc.0 and
  0.8.0-rc.0). `npm run check:headers` fails if a C++ header is missing from the list.

## 0.8.0-rc.0

Includes everything in 0.7.0-rc.0 (16 voices, arpeggiator, 24 spectrum bands).

### Added

- **Drums.** A synthesized eight-voice drum part (kick, snare, closed hat, open hat, clap, low
  tom, high tom, rim; no samples), mixed with the synth before the effects chain with its own
  level and mute. `hasDrums()`, `drumHit(voice, velocity)`, `setDrumVoice(voice, params)`,
  `setDrumKit(params)`, `getDrumState()`; types `DrumVoiceParams`, `DrumKitParams`, `DrumState`;
  constants `DRUM_VOICE_DEFAULTS`, `DRUM_KIT_DEFAULTS`, `DRUM_VOICE_COUNT`. Hits are lock-free
  and share the note queue; each voice is monophonic and retriggers; the closed hat chokes the
  open hat; the arpeggiator never applies to drums. See README "Drums" and `docs/drums.md`.
- `JucePadView` `mode: 'notes' | 'drums'`: in drums mode each touch-down hits drum voice `note`
  natively, with the same velocity as notes mode.
- Diagnostics `drumHits` and `drumPeak`.
- Beat events carry `position`, the fractional transport position when the event is sent.

### Fixed

- No beat events when the first listener subscribed before `setup()`: the beat timer now starts
  on setup when listeners exist (and may start before setup, idling until the engine exists).
- No beat events after all listeners were removed and one was added again: stopping the timer no
  longer drops the native beat callback.
- Backing track: a replace or unload published while the audio thread was inside its
  per-block buffer hand-over could let the replaced buffer be freed while its 5 ms fade-out
  still read it (a crash or noise on the audio thread right after a swap; present since
  0.6.0). The hand-over now keeps the old buffer protected on every path.

### Changed

- `synthPeak` covers the synth voices only (drums are reported in `drumPeak`).

### Compatibility

- Binaries older than 0.8.0: `hasDrums()` returns `false`, `drumHit` is a no-op, `setDrumVoice` /
  `setDrumKit` return `false`, `getDrumState()` returns the defaults; `JucePadView` ignores `mode`.

Earlier versions: see the git history (0.5.2 diagnostics and recovery, 0.6.0 effects, metronome,
backing track and analysis, 0.7.0 arpeggiator and spectrum).
