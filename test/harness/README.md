# Offline audio-path harness

Reproduces the iOS native audio path on Linux/WSL without a device: the `doSetup` call order
from `ios/ExpoJuceAudioEngine.mm`, `AudioDeviceManager::addAudioCallback` (which calls
`GraphPlayer::audioDeviceAboutToStart` → second `prepareToPlay`), and a fake device thread
calling `audioDeviceIOCallbackWithContext` with iOS-like block sizes. Each case sends
`noteOn(60,127)`, holds it 0.5 s and reports PASS/FAIL (non-silent, finite output).

From this directory, in WSL or any Linux shell:

```sh
cmake -S . -B build && cmake --build build -j16 && ./build/harness
./build/harness arp arpv sp   # only the cases named (ids: ctl, a … fx, mp, rs, rl, fx, fxb, …, an, arp, arpv, sp)
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
  top sample-exact, loops, seeks with `setPosition` to beat × 60 / bpm, starts mid-bar at the
  beat's position, pause + resume continues where it stopped, stops, plays once without loop,
  is decoded again from its file after a device-rate change), `btt` (a tempo ramp 120 → 124 bpm
  while playing leaves the track sample-continuous), `bsf` (replacing and unloading the track
  while it plays are crossfaded, never a step above one fade increment; the old buffer survives
  `collect()` until its fade-out ends), `btr` (loop-region rule on all its cases; decodes
  `fixtures/loop-124bpm-44k.mp3` with JUCE's MP3 decoder, prints which length it yields and
  the rule chosen; the exact loop resampled 44.1 → 48 kHz is 743226 frames), `btl` (frame cap
  and available-memory refusals), `bts` (300
  loads / unloads while another thread renders: 0 allocations and 0 frees in the render, every
  replaced buffer freed by `collect()`, a superseded load does not publish), `aux0` (aux with
  nothing to play outputs exact zeros; a graph with it equals one without it, sample for
  sample), `rsa` (aux settings and the track survive a rebuild), `an` (the `AnalysisRing` written
  by `GraphPlayer` holds a known sine at the expected scope indices, peak / rms since the
  last read, mono RMS, the transport published by the aux node). The `allocs` / `frees` counters also count `free()` inside render calls.
- Arpeggiator (`ios/Arpeggiator.h`, pure C++, driven with a recording sink): `arp` (the six
  pattern vectors of the design spec plus upDown edge cases, random membership and chord; at
  120 bpm / `1/16` / 48 kHz every hit lands exactly on `k × 6000` over the odd `kFxBlocks` sizes,
  note-off at `+ round(gate × 6000)`, swing 0.25 moves odd steps by 1500, ratchet 3 gives hits at
  0 / 2000 / 4000 with 1200-sample gates, `1/8D` = 18000 and `1/8T` = 8000; playing transport
  from beat 0.3 plays step 0 at once and step 1 on the 0.5 line, from 0.4 step 0 waits for the
  0.5 line; latch plays on after release and the next press replaces the list; disabling emits an
  off for every sounding note in that block and clears; a tap released before the first grid
  line plays nothing; allNotesOff stops and clears a latched list; a tempo change 120 → 60 puts
  the next step 12000 after the previous; pattern bits beyond `length` are ignored; same-note
  legato (gate 1.5) drops the superseded off; clamping / defaults / name tables; 0 allocations
  in every `process`), `arpv` (through `PolySynthProcessor`, square wave, attack / release 1 ms,
  gate 0.5, 471-sample blocks: every step's onset is on sample `k × 6000` + the attack's rise to
  0.01, the output is exactly 0 for the 1000 samples before each later step, 16 held notes in
  `chord` mode sound on 16 voices, disabling mid-gate silences the output within that block and
  a later player note-off of the arp note is harmless, enabling leaves a directly played voice
  sounding until its release; 0 allocations). `rs` also round-trips the arp settings through
  `SynthSettings`.
- Spectrum (`ios/SpectrumAnalyser.h`, pure C++): `sp` (a full-scale 1 kHz sine reads ≥ 0.95 in
  its band, 800–1027 Hz at 48 kHz, and ≤ 0.6 two bands away; a full-scale sine on a bin centre in
  that band reads ≥ 0.99 with both neighbours ≤ 0.6 (a tone between bins spills its Hann main
  lobe into the adjacent band, about 0.8, so sidelobe leakage is checked on a bin-centred tone);
  −60 dBFS → ≤ 0.05; silence → exactly 0; 60 Hz reads ≥ 0.9 in band 0 (one bin wide) and 70 Hz
  ≥ 0.95 in band 1; the edge table is strictly increasing with every band at least one bin wide
  at 48000 and 44100 Hz; `AnalysisRing::readRaw` is oldest-first and `GraphPlayer` publishes the
  device rate into the ring; `analyse` does not allocate).
- `.gitattributes` keeps `*.sh` at LF, so `fetch_deps.sh` runs from a Windows checkout. A checkout made
  before that rule still has CRLF: `git checkout -- fetch_deps.sh` (or `sed -i 's/\r$//' fetch_deps.sh`), or copy
  an existing `build/sysroot` (with its `.done`). Inside a Windows `git worktree` WSL cannot follow the
  `.git` pointer, so `git show HEAD:...` fails: pass `-DHARNESS_GIT_REF=WORKTREE` there.
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
