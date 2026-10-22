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

- Engine sources (`Voice`, `VoiceAllocator`, `TransportEngine`, `PolySynthProcessor`) are
  taken from `git show HEAD:ios/...`; pass `-DHARNESS_GIT_REF=WORKTREE` (or any ref) to change.
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
