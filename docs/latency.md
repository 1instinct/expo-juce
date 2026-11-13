# Latency guide

How long it takes from a finger touching an on-screen pad in an app using expo-juce to
sound leaving the phone, where that time goes, what expo-juce does about it, how to read
the numbers it reports, and how to measure the real figure with a slow-motion camera.
Written for the latency-margin work of 2026-10-22; behaviour that arrives with that work is
marked **(0.5.2+)** and should be re-checked against the code and a device once it ships.

Contents
1. [Budget and targets](#1-budget-and-targets)
2. [The signal path, finger to speaker](#2-the-signal-path-finger-to-speaker)
3. [Baseline (2026-10-22)](#3-baseline-2026-10-22)
4. [Changes and why](#4-changes-and-why)
5. [A diagnostics panel](#5-a-diagnostics-panel)
6. [Slow-motion measurement procedure](#6-slow-motion-measurement-procedure)
7. [Results](#7-results)
8. [Troubleshooting](#8-troubleshooting)

---

## 1. Budget and targets

| | Touch → sound | Meaning |
|---|---|---|
| **Budget** | < 40 ms | Above this, players feel the pad "drag"; the panel's latency stage fails above 40 ms (`LATENCY_BUDGET_MS`). |
| **Target** | ≤ 25 ms | What we design for, so jitter and slower routes still stay inside the budget. |
| **Stretch** | ≤ 15 ms | Comparable to a hardware instrument; only reachable on wired output. |

The number that matters is **acoustic**: finger contact to sound in the air. The app can only
see part of the chain (from the moment iOS hands it a touch), so the panel gives an
*estimate* and the slow-motion procedure in section 6 gives the *measured* value.

## 2. The signal path, finger to speaker

Each segment with typical values on a current iPhone. "Seen by" names the diagnostics field
that covers it, if any.

| # | Segment | Typical | Seen by |
|---|---|---|---|
| 1 | **Digitizer scan and touch delivery.** The touch controller scans the glass (120 Hz class on recent iPhones), the system builds a `UITouch` and delivers it to the app's main thread. | ≈ 8–16 ms | nothing in-app; slow-motion only. `UITouch.timestamp` marks the start of in-app time **(0.5.2+)**. |
| 2a | **React Native path.** `Pressable` → the touch is dispatched as a Fabric event to the JS thread → Pressability → `onPressIn` → the app's handler → `ExpoJuce.noteOn` (synchronous JSI call). Waits for the JS thread, which also runs React renders, animation work and timers. | ≈ 2–16 ms, plus jitter when the JS thread is busy (tens of ms during heavy renders) | the dispatch wait is invisible; an app-side timestamp in `onPressIn` covers only handler → `noteOn` (≈ 0.04 ms) |
| 2b | **Native path — `JucePadView` (0.5.2+).** `touchesBegan` on the main thread calls the engine's `noteOn` directly; JS is notified afterwards (`onNoteOn`) for visuals and haptics only. | < 0.1 ms after delivery | `lastTouchToRenderMs` (0.5.2+) |
| 3 | **SPSC queue → next audio block.** `noteOn` pushes a `NoteEvent` into the 256-slot ring of the engine's `NoteEventQueue` (no engine lock; releases are separate marks, see `ios/NoteEventQueue.h`); `PolySynthProcessor::processBlock` drains it at the *start* of the next block. Wait is 0 … 1 buffer, half a buffer on average. | 0–5.4 ms @ 257 samples; 0–2.7 ms @ 128 | `lastNoteToRenderMs` |
| 4 | **I/O buffer.** The rendered block is handed to Core Audio and played out over one buffer period. | 5.35 ms @ 257 / 48 kHz; 2.67 ms @ 128 | `sessionIOBufferMs` |
| 5 | **Output latency of the route.** Converter, system output processing and the transducer path, as reported by `AVAudioSession.outputLatency`. Depends entirely on the route: | | `sessionOutputLatencyMs`, `routeOutputs` |
| | — built-in speaker (`Speaker`) | ≈ 15 ms (includes the system's speaker processing) | |
| | — wired headphones / USB-C or Lightning adapter (`Headphones`, `USBAudio`) | a few ms | |
| | — Bluetooth A2DP (`BluetoothA2DPOutput`) | **100–200 ms — unsupported for play** | |
| | — AirPlay (`AirPlay`) | hundreds of ms or more (buffered) — **unsupported for play** | |
| 6 | **Envelope attack.** The voice's ADSR ramps from silence to full level; the onset is audible from the first samples but reaches full loudness only at the end of the attack. | 10 ms default; 2 ms (0.5.2+) | `Voice::attackMs` (settable with `setAttack`) |

Bluetooth and AirPlay latency is added by the receiver and the radio link; nothing the app
does can remove it. The app plays through them, but they are not supported for playing
pads in time. Out of scope: Android. Note-ons from pads and keys are still applied at the start
of the block that drains them (segment 3); the arpeggiator's steps (0.7.0+) are the exception:
the arp schedules its note-ons and note-offs at sample offsets inside the block and the synth
renders the block in segments around them, so step timing does not depend on the block size.

## 3. Baseline (2026-10-22)

iPhone17,1 (iPhone 16 Pro), iOS 26.6.2, built-in speaker, dev build of the test app,
expo-juce `0.5.2-diag.1`, read from its diagnostics panel (section 5;
`lastPressToNoteOnMs` is that app's own `onPressIn` → `noteOn` timer, not an expo-juce field):

| Field | Value |
|---|---|
| `sessionSampleRate` / `sampleRate` | 48 000 Hz |
| `bufferSize` | 257 samples |
| `sessionIOBufferMs` | 5.35 ms |
| `sessionOutputLatencyMs` | 15.35 ms |
| `lastPressToNoteOnMs` | 0.04 ms |
| `lastNoteToRenderMs` | 3.48 ms |
| **Panel estimate** (`js press→call + note→render + session io + session out`) | **24.2 ms** |

The estimate excludes segments 1 and the dispatch part of 2a, so the acoustic latency is
expected to be roughly 35–40 ms on the speaker with the current `Pressable` path — which is
why the work below targets both the audio buffer and the touch path. Confirm with section 6.

## 4. Changes and why

From the latency-margin spec. Savings are expected values on the baseline device; replace
them with measured ones in section 7.

| # | Change | Why | Expected saving |
|---|---|---|---|
| 1 | **128-sample buffer at 48 kHz (0.5.2+).** Before `AudioDeviceManager::initialise`, set `AVAudioSession` preferred sample rate 48 000 and preferred IO buffer duration 128/48 000; afterwards `setAudioDeviceSetup` with `sampleRate = 48000`, `bufferSize = 128`. Falls back to what the device grants and reports the actual values. `setup({ bufferSize?, sampleRate? })` (defaults 128 / 48 000) lets the app tune without a native rebuild. | iOS defaulted to 257 samples. Halving the buffer halves segment 4 and the average wait in segment 3. | ≈ 2.7 ms (I/O) + ≈ 1.3 ms (queue, average) ≈ **4 ms** |
| 2 | **No allocation on the audio thread (0.5.2+).** `GraphPlayer` sizes its scratch buffer in `audioDeviceAboutToStart` (≥ 2× device buffer, ≥ 1024 samples) and renders oversize blocks in chunks instead of growing it. | Before, `buffer.setSize` ran inside the device callback and could allocate when iOS delivered a larger block — a dropout risk that gets worse at small buffers. | none on average; removes glitches |
| 3 | **`JucePadView` native touch path (0.5.2+).** Expo native view; `touchesBegan/Ended/Cancelled` on the main thread call `noteOn/noteOff` directly. Props `note`, `velocity`, `enabled`; events `onNoteOn`, `onNoteOff` (visuals/haptics only). Multi-touch safe: note off only when the last touch on that view lifts. Stamps `UITouch.timestamp` into the note event. | Takes the JS thread (and its jitter) out of the trigger path. | ≈ 2–16 ms typical, much more under JS load; also removes jitter |
| 4 | **Diagnostics (0.5.2+):** `lastTouchToRenderMs`, `preferredBufferSize`, `preferredSampleRate`, `sessionReassertCount`. Existing fields unchanged. | Measure the new path from the hardware touch time; show requested vs granted buffer. | — |
| 5 | **Session safeguard and recovery (0.5.2+).** On `AVAudioSessionRouteChangeNotification` with reason `CategoryChange`, if the category is not Playback/PlayAndRecord, re-assert Playback + MixWithOthers (re-entrancy guarded, counted). On interruption end, reactivate the session and restart the device if stopped. On media-services reset (or a device whose callback has stalled), tear down and set up again with the last options, keeping synth settings and transport state. Fields `interruptionCount`, `mediaResetCount`, `lastRecoveryError`, `engineState`. | expo-av once switched the shared session to Ambient (muted by the silent switch); calls and Siri left the engine silent. | none; keeps sound coming |
| 6 | **Attack default 10 → 2 ms (0.5.2+).** App can override with `setAttack`. | Percussive onset; 2 ms is still click-free. | up to ≈ 8 ms to full level (perceived) |
| 7 | App: `setup({ bufferSize: 128, sampleRate: 48000 })` (0.5.2+). | Uses change 1. | — |
| 8 | App: pads and keyboard keys trigger through `JucePadView` when `isJucePadViewAvailable()` is true, falling back to `Pressable` on older binaries (0.5.2+). | Uses change 3. | — |
| 9 | App: panel shows `lastTouchToRenderMs` and actual vs preferred buffer; the latency stage uses touch→render + I/O + output when available (0.5.2+). | Honest estimate for the native path. | — |

Expected panel estimate after the work on the same device and speaker:
≈ 1.5–2.5 (touch→render) + 2.7 (I/O) + 15.4 (output) ≈ **20 ms**, with the acoustic figure
≈ 8–16 ms higher because of segment 1. On wired headphones both drop by roughly 10 ms.

### Known trade-offs

Of the `JucePadView` touch path (change 3):

- **Drag inside a ScrollView cuts the note.** A drag of about 10 pt makes the enclosing
  `UIScrollView` take the gesture and cancel its content touches; the pad gets
  `touchesCancelled` and sends note off.
- **No glissando.** A touch stays on the pad it started on; sliding onto a neighbouring pad
  neither ends the first note nor starts the next.
- **Pads tapped before `setup()` completes are silent** on the native path (the engine drops
  notes until it is ready; nothing is queued for later).

## 5. A diagnostics panel

expo-juce supplies the data (`getDiagnostics()`, `playTestTone()`); the panel itself lives in
the app. The layout below is the one the measurements in this guide were taken with, and a
good template: a debug screen, reachable in release builds (e.g. a hidden long-press), that
polls `getDiagnostics()` four times a second and shows a nine-stage ladder; each stage is
`pass`, `FAIL` or `?` (not enough data yet). Buttons: **Test tone** (`playTestTone(500)`:
440 Hz mixed straight into the device output, bypassing graph and synth), **Play C4**,
**Re-run setup**, **Copy JSON** (share sheet with the full snapshot — attach it to bug
reports).

| # | Stage | Fields | Read it as |
|---|---|---|---|
| 1 | JS module loaded | `require`, `diag api` | FAIL = `require('expo-juce')` threw; the app is on silent stubs (usually a stale Metro bundle or a binary without the module). |
| 2 | Engine setup | JS state/error, `isReady`, `setupStep`, `lastError` | `setupStep` 0–11 is the last native step reached (11 = graph player attached). A non-empty `lastError` with `isReady` true is a non-fatal note. |
| 3 | Audio session | `sessionCategory`, `sessionSampleRate`, `sessionIOBufferMs`, `sessionOutputLatencyMs`, `routeOutputs`, `otherAudioPlaying` | Category must be Playback (or PlayAndRecord); Ambient fails because the silent switch mutes it. Route tells you which output-latency row of section 2 applies. |
| 4 | Device callbacks | `callbackCount` rate, device name/type, `sampleRate`, `bufferSize` | Rate ≈ sampleRate / bufferSize per second (≈ 187/s at 257, 375/s at 128). Not moving = device/session layer is stopped. |
| 5 | Graph processing | `graphProcessCount` | Should move with stage 4. |
| 6 | Synth processing | `synthProcessCount`, notes queued/consumed/dropped, `activeVoices` | Dropped > 0 means note-ons overflowed the 256-slot ring (releases are never dropped). Counters are cumulative since launch. |
| 7 | Synth signal | `synthPeak` now / max | Passes once a note produced signal inside the synth. |
| 8 | Output signal | `outputPeak`, `nanCount`, `toneSamplesMixed` | Graph output before the test tone; any NaN block fails. |
| 9 | Latency | see below | Pass when the estimate ≤ 40 ms. |

Latency rows:

- **js press→call** — app-side: a timestamp taken in `onPressIn` to the native `noteOn`
  call. "(deferred)" means the note waited for setup; ignore that sample.
- **note→render** — `lastNoteToRenderMs`: `noteOn` enqueue to the synth consuming it at
  the start of a block (segment 3). Bounded by one buffer.
- **touch→render (0.5.2+)** — `lastTouchToRenderMs`: `UITouch.timestamp` to the first
  rendered block, for notes from `JucePadView` (segments 2b + 3). −1 or absent = no native
  touch yet / old binary.
- **device out** — JUCE's `outputLatencySamples / sampleRate`; shown for reference.
- **session io / session out** — segments 4 and 5 as iOS reports them.
- **buffer actual / preferred (0.5.2+)** — `bufferSize` vs `preferredBufferSize`. iOS may
  grant a different size (e.g. on some routes or when another app owns the hardware).
- **est. total** — `js + note→render + session io + session out` today; with 0.5.2+
  `touch→render + session io + session out` when a native touch exists. Never includes
  segment 1, so it is always lower than the slow-motion figure.

Recovery fields (0.5.2+): `sessionReassertCount` (Playback re-asserted after someone changed
the category — a rising count means another library keeps changing it), `interruptionCount`,
`mediaResetCount`, `lastRecoveryError` (empty when all recoveries succeeded).

### Touch latency legs and device format (0.9.0-rc.1)

A device capture on 0.8 (iPhone 16 Pro, wired headphones, a camera scene) showed touch → render
134 ms but queue → render 5.5 ms: almost all of the time passed before the note was queued. One
"last" value could not say where, so the path is now split, each leg with `last` / `p50` / `p95`
/ `max` over the last 64 pad notes and hits (`touchLatency`):

| Leg | From → to | Written by |
|---|---|---|
| `touchDeliveryMs` | `UITouch.timestamp` → `touchesBegan` entry (`CACurrentMediaTime()`, same mach clock) | main thread |
| `touchHandlerMs` | `touchesBegan` entry → note queue push | audio thread (from the event's stamps) |
| `queueToRenderMs` | push → the render block that consumed it | audio thread |
| `touchToRenderMs` | `UITouch.timestamp` → render block (`lastTouchToRenderMs` is its `last`) | audio thread |

`bufferMs` and `outputLatencyMs` complete the path to the speaker. The windows are lock-free
single-writer rings (`ios/LatencyStats.h`, harness case `lat`).

**Reading it.** A large delivery leg means iOS held the touch back: either a recognizer or
scroll view delayed it, or the main thread was busy.

- `touchPath`: every gesture recognizer from the pad up to its window (window included) with
  `delaysTouchesBegan` / `delaysTouchesEnded` / `cancelsTouchesInView`, and every scroll view
  with `delaysContentTouches` / `canCancelContentTouches`. Reported when a pad attaches, a second
  later, and after its first touch (some recognizers are attached after mount).
- `lastTouch`: the recognizers the last touch was actually delivered to (`UITouch.gestureRecognizers`,
  which also lists ones that sit on no ancestor, e.g. the system's screen-edge gate), its
  `deliveryMs` and `edgeDistance` (points to the nearest window edge).
- `mainThreadMaxGapMs` (longest gap between display-link ticks in the last 10 s) and
  `mainThreadStalls50` (gaps over 50 ms since launch).

**Device format.** The engine now runs at the session's current rate (`hardwareSampleRate`, read
at setup before JUCE's device exists: the route's own rate on a fresh session, else the rate last
preferred in the process) instead of forcing 48 kHz; `preferredSampleRate` is a hint for when the
session reports no music rate. The IO buffer duration is computed at that rate. After a route
change the rate check runs once JUCE's device has restarted itself and asked for its old rate
again, so it only sees a new rate when the new route cannot run the old one (a voice rate is
ignored); it does not find a rate the route would merely prefer. JUCE only
accepts a buffer size from the device's list (`availableBufferSizes`: the sizes granted for 64,
128, … frames), so the engine asks for the nearest listed size (`requestedBufferSize`) and reports
what it got (`grantedBufferSize`). Recovery used JUCE's `restartLastAudioDevice`, which reopens a
new device with the last *granted* size; that size is normally not in the list, so JUCE fell back
to its default 256 (+1 frame, rounded up by the route: 288 at 44.1 kHz on 32-frame steps). The
dead-device restart, the stopped-device restart, the interruption recovery and route changes now
reapply the session preferences and reopen with the nearest listed size (`ios/DeviceFormat.h`,
harness case `bsel`, which runs JUCE's real `AudioDeviceManager` against an iOS-like fake device).
On a route change JUCE's device restarts itself with the size it was opened with; the engine asks
again only when the nearest listed size on the new route is a different one (e.g. back on the
speaker after a Bluetooth route opened at 1024), and a request equal to the running setup reopens
nothing (harness case `broute`). `deviceOpens` keeps the last 8 opens (context, requested and
granted size and rate, `opened`: whether the device was actually reopened) and `xrunCount` the
underruns since the last `setup()`, each device's counted once.

## 6. Slow-motion measurement procedure

Measures finger contact → sound, the only figure that includes the digitizer and the speaker.

### Equipment

- **The test phone** running a **preview** (or prod) build — not a dev build: dev builds run
  the JS bundle in development mode from Metro and are slower and noisier on the JS thread.
- **A second device that records 240 fps slow motion** (iPhone: Camera → Slo-mo, set 1080p
  at 240 fps in Settings → Camera → Record Slo-mo). Do not film with the test phone itself.
- A tripod or something to prop the camera 20–40 cm from the test phone.
- A computer with **Audacity** (free) and a video editor that steps frame by frame
  (Final Cut Pro, DaVinci Resolve, or QuickTime/VLC with frame stepping).
- Optional: a capacitive stylus (sharper, louder contact click than a fingertip) and a pen
  for the sync check.

### Setup

1. Quiet room; no fan or music. Phone flat on a table, screen up.
2. Note the **ring/silent switch** (Action button mode) position — it must not matter with a
   Playback session, but write it down.
3. Volume about 75 %. Built-in speaker unless you are testing another route; **disconnect
   Bluetooth** (Control Centre) so the route cannot change mid-run.
4. Open the screen with the pads and choose a high-contrast pad (the lit pad colour against the
   dark background is easy to see). Set waveform to square or saw for a sharp onset; leave
   attack at the default and note its value.
5. Open the diagnostics panel once, check stage 3 shows `Speaker` and the buffer you expect,
   note `est. total`, close it.
6. Frame the shot so the fingertip, the pad and the phone's bottom speaker grille are all in
   view, lit brightly (slow motion needs light). Camera microphone as close to the speaker as
   the framing allows.
7. Optional **sync check**: before the taps, tap the pen firmly on the table in frame three
   times. The frame where the pen touches and the click in the audio give the recording's own
   audio/video offset.

### Steps

1. Start the slow-motion recording.
2. Do the sync-check taps (optional).
3. Tap the pad **firmly and quickly**, one finger, about once a second, **10–20 times**. Lift
   fully between taps. Keep the rest of the hand out of the pad area.
4. Stop recording. Note any taps that felt wrong (double touch, missed).
5. Transfer the **original** file to the computer (AirDrop → Options → All Photos Data, or
   Photos → File → Export → Export Unmodified Original). Photos' slow-motion ramp is metadata;
   the original is a 240 fps file playing at real-time speed with real-time audio.

### Counting frames (video method)

At 240 fps one frame = 1000 / 240 = **4.17 ms**.

For each tap:

1. Step to the **first frame where the finger visibly touches the glass** (skin flattens or the
   pad lights is too late — use contact). Note the frame number or timecode, *F₀*.
2. In the editor's audio waveform, find the **first sample where the synth tone rises out of the
   noise floor** and convert to a frame, *F₁* (or read its time directly).
3. Latency = (*F₁* − *F₀*) × 4.17 ms. If you did the sync check, subtract the pen offset
   (pen click time − pen contact frame time).

Resolution is ± 1 frame (± 4 ms) per reading; the median of 10–20 taps is good to about 2 ms.

### Audio-only method (higher precision)

The finger (or stylus) hitting the glass makes a small click; the synth starts after it. Both
come from the same place, so the recording's audio/video offset does not matter.

1. Open the recording (or a separate audio recording made with a mic or the second phone's
   Voice Memos placed next to the test phone) in Audacity. Set the project rate to the file's
   rate (usually 48 000 Hz).
2. Zoom in on one tap. Mark the **onset of the contact click** (sharp transient, broadband) and
   the **onset of the synth** (periodic waveform at the pad's pitch).
3. Select from click onset to synth onset; Audacity shows the selection length — switch the
   selection format to samples. Latency = samples / 48 000 × 1000 ms (48 samples = 1 ms).
4. If the click is hard to see, use the stylus, tap harder, or high-pass filter a copy at
   2 kHz to make the transient stand out (the synth's fundamental is low).

Resolution is well under 1 ms. The click marks the moment of contact, which is the same
starting point as the video method.

### Report

For each run: median, minimum, maximum (and the number of taps). Discard a tap only for a
recorded reason (double touch, missed note). Add a row to section 7, together with the panel's
`est. total`, `bufferSize` and route from the same session; the difference between measured and
estimated is the part the app cannot see (segment 1 plus, on the `Pressable` path, JS dispatch).

## 7. Results

### TODO: device checks pending for 0.5.2

`0.5.2-rc.2` compiles and installs (EAS preview, app build 7, 2026-10-23); these checks have
not been run on a device yet:

- [ ] **Latency:** slow-motion measurement (section 6) on the built-in speaker, recorded below.
- [ ] **Stuck notes:** mash pads quickly and lift; nothing keeps sounding, `notesDropped` stays 0.
- [ ] **Interruption:** hold a pad, then trigger an interruption (incoming call, Siri, alarm).
      After it ends: silence (no stuck note), `interruptionCount` moved, pads play again,
      `lastRecoveryError` empty.
- [ ] **Route change:** plug and unplug wired headphones while playing; sound follows the route,
      no stuck note.
- [ ] **State across recovery:** change waveform / envelope and tempo, start the transport, then
      trigger an interruption. Settings, tempo and play state are unchanged afterwards. (The full
      rebuild path, a media-services reset, is hard to trigger on demand; the offline harness
      covers it with case `rs`.)

Copy the template row; newest first.

| Date | Build (app / expo-juce) | Device / iOS | Route | Buffer (actual / pref.) | Panel est. (ms) | Measured median (ms) | Min | Max | Notes |
|---|---|---|---|---|---|---|---|---|---|
| YYYY-MM-DD | preview 0.2.0 (n) / 0.5.2-… | iPhone17,1 / 26.6.2 | Speaker | 128 / 128 | | | | | method (video/audio), taps, path (JucePadView/Pressable), attack |
| 2026-10-22 | dev / 0.5.2-diag.1 | iPhone17,1 / 26.6.2 | Speaker | 257 / — | 24.2 | not measured | | | baseline from panel only |

## 8. Troubleshooting

**Panel estimate fine, measured latency much higher.** Expected difference is segment 1
(≈ 8–16 ms). More than that on the `Pressable` path points at the JS thread: close visualizers,
measure in a preview build, or check that pads use `JucePadView` (0.5.2+; `touch→render`
populated).

**Buffer stays at 257 (or another size) after 0.5.2+.** iOS treats the preferred IO buffer as a
request. Check `routeOutputs` (Bluetooth forces larger buffers), whether another app is playing
(`otherAudioPlaying`), and `lastError` for a `setAudioDeviceSetup` note. Re-run setup after
changing the route.

**Clicks, crackles or dropouts at 128.** Pass a larger buffer from the app,
`setup({ bufferSize: 256 })` (0.5.2+), and compare. Rising `nanCount` is a DSP bug, not a buffer
problem.

**`touch→render` shows — or −1.** No note has come through `JucePadView` yet, the installed
binary predates it (the app then falls back to `Pressable`), or the pad is `enabled={false}`.

**Latency stage says "(deferred)".** The note was pressed before setup finished and waited for
it; play another note.

**Route is `BluetoothA2DPOutput` or `AirPlay`.** 100–200 ms or more of added latency is expected
and cannot be removed; disconnect for playing. Measurements on these routes are not comparable.

**Sound stops after a phone call, Siri or an alarm.** Before 0.5.2+ the engine had no
interruption recovery — re-run setup from the panel or restart the app. With 0.5.2+ check
`interruptionCount` / `mediaResetCount` moved and `lastRecoveryError` is empty.

**Category shows Ambient / silent switch mutes the synth.** Another audio library changed the
shared session. If the app also uses expo-av, call
`Audio.setAudioModeAsync({ playsInSilentModeIOS: true })` at startup (expo-av's default sets
Ambient as soon as it plays anything, even a muted `<Video>`); with 0.5.2+ expo-juce also
re-asserts Playback and counts it in `sessionReassertCount`.

**No click visible in the audio-only method.** Fingertip taps on glass are quiet; use a
capacitive stylus, move the mic closer, or use the video method.
