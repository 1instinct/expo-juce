# Offline audio-path harness

Reproduces the iOS native audio path on Linux/WSL without a device: the `doSetup` call order
from `ios/BeeperAudioEngine.mm`, `AudioDeviceManager::addAudioCallback` (which calls
`GraphPlayer::audioDeviceAboutToStart` → second `prepareToPlay`), and a fake device thread
calling `audioDeviceIOCallbackWithContext` with iOS-like block sizes. Each case sends
`noteOn(60,127)`, holds it 0.5 s and reports PASS/FAIL (non-silent, finite output).

```sh
wsl -e bash -lc "cd /mnt/c/Users/aaron/Internal/Beeper/code/expo-juce/test/harness && cmake -S . -B build && cmake --build build -j16 && ./build/harness"
```

- Engine sources (`Voice`, `VoiceAllocator`, `TransportEngine`, `PolySynthProcessor`) are
  taken from `git show HEAD:ios/...`; pass `-DHARNESS_GIT_REF=WORKTREE` (or any ref) to change.
- `GraphPlayer` is a verbatim copy inside `harness.cpp` (source commit noted there).
- Needs no root: missing X11/freetype dev headers (only needed because
  `juce_audio_processors` includes the gui modules) are fetched into `build/sysroot` by
  `fetch_deps.sh` via `apt-get download` + `dpkg -x`.
- Exit code is non-zero on any unexpected result. Case `g2` (aboutToStart off the message
  thread with no message loop) is a negative control and is expected to FAIL: it shows the
  `AudioProcessorGraph` settings-mismatch silence path when the async rebuild never runs.
