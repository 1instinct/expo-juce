# Offline audio-path harness

Reproduces the iOS native audio path on Linux/WSL without a device: the `doSetup` call order
from `ios/ExpoJuceAudioEngine.mm`, `AudioDeviceManager::addAudioCallback` (which calls
`GraphPlayer::audioDeviceAboutToStart` → second `prepareToPlay`), and a fake device thread
calling `audioDeviceIOCallbackWithContext` with iOS-like block sizes. Each case sends
`noteOn(60,127)`, holds it 0.5 s and reports PASS/FAIL (non-silent, finite output).

From this directory, in WSL or any Linux shell:

```sh
cmake -S . -B build && cmake --build build -j16 && ./build/harness
```

- Engine sources (`Voice`, `VoiceAllocator`, `TransportEngine`, `PolySynthProcessor`, `BackingTrack`, `AuxProcessor`,
  `FxChainProcessor`) are taken from `git show HEAD:ios/...`; pass `-DHARNESS_GIT_REF=WORKTREE`
  (or any ref) to change.
- The graph cases build `synth → FxChainProcessor → output` like `doSetup` (effects off, so the
  output is the dry synth); case `fx` runs the same with every effect on. Effects cases:
  `fx <id>` / `fx all` (each effect, then all, with non-default params over odd block sizes:
  finite, non-silent, differs from dry, under the limiter ceiling, 0 allocations), `fxb` (chain
  off == dry bit-exact; after echo + reverb are turned off their tail spills over, then the
  output is bit-exact again once both tails have ended), `fxt` (an echo tail at feedback 0.95 is
  capped and faded at 5 s, then exact silence; re-enabled on silence it stays exactly 0, i.e. the
  uncleared line is never read back; a NaN input is silenced and resets the effects at most
  once), `fxd` (`src/effectDefaults.ts` lists the same effects / params / defaults as
  `kFxSpecs`), `fxe` (synced echo returns exactly
  division × 60 / bpm later), `fxp` (pump dips right after each transport beat; limiter holds a
  2.0 sine under -0.3 dBFS); `rs` also round-trips the effect settings.
- The graph cases also add the `AuxProcessor` node (metronome + backing track) into the output,
  at its defaults (silent). Aux cases: `met` (every metronome style: each click starts exactly on
  the sample `ceil(k × 60 × sr / bpm)`, equals the pre-rendered click, accent on bar beat 1,
  silence between, over odd block sizes and mid-beat starts), `bt` (backing track plays from the
  top sample-exact, loops, seeks with `setPosition`, starts in the bar the transport started in,
  stops, plays once without loop, is resampled after a device-rate change), `btr` (loop-region
  rule on its lengths; decodes `fixtures/loop-124bpm-44k.mp3` with JUCE's MP3 decoder and
  prints which length it yields and the region chosen; resample 44.1 → 48 kHz), `bts` (300
  loads / unloads while another thread renders: 0 allocations and 0 frees in the render, every
  replaced buffer freed by `collect()`, a superseded load does not publish), `aux0` (aux with
  nothing to play outputs exact zeros; a graph with it equals one without it, sample for
  sample), `rsa` (aux settings and the track survive a rebuild), `an` (the `AnalysisRing` written
  by `GraphPlayer` holds a known sine at the expected scope indices, peak / rms of the last
  block). The `allocs` / `frees` counters also count `free()` inside render calls.
- From a Windows checkout (`core.autocrlf=true`) `fetch_deps.sh` has CRLF line endings and bash
  rejects it: run from a copy with LF endings, or copy an existing `build/sysroot` (with its
  `.done`) and convert the script (`sed -i 's/\r$//' fetch_deps.sh`).
- `GraphPlayer` is the shipped class from `ios/GraphPlayer.h` (same git ref). Cases `k`/`k2` deliver
  blocks larger than its preallocated capacity and check the chunked render (graph calls = ceil(n/cap)
  per block, no silent block tails, no buffer growth).
- Every IO callback runs with an allocation counter on (the glibc malloc family is interposed,
  which covers `operator new` and JUCE's `HeapBlock`); the `allocs` column must be 0 in every
  case. The vendored JUCE needs the `graph-output-buffer-no-realloc` patch
  (`src/scripts/juce-patches.cjs`) for that: unpatched, cases `d` and `k2` allocate on almost every
  block and all cases on their first.
- Needs no root: missing X11/freetype dev headers (only needed because
  `juce_audio_processors` includes the gui modules) are fetched into `build/sysroot` by
  `fetch_deps.sh` via `apt-get download` + `dpkg -x`.
- Exit code is non-zero on any unexpected result. Case `g2` (aboutToStart off the message
  thread with no message loop) is a negative control and is expected to FAIL: it shows the
  `AudioProcessorGraph` settings-mismatch silence path when the async rebuild never runs.
