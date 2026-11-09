# Changelog

## Unreleased

### Changed

- **Licensing is stated in full.** Added `LICENSE` (MIT, for expo-juce's own code) and
  `THIRD_PARTY_NOTICES.md` (JUCE 8: the JUCE licence or AGPLv3; Oboe: Apache 2.0; libFLAC: BSD).
  `package.json` `license` is now `MIT AND (AGPL-3.0-only OR LicenseRef-JUCE-8-EULA)`, and both
  files ship in the package. No code change.

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
