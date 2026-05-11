# Polyphonic Synth Engine Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the mono SynthEngine + AVAudioEngine with a JUCE-owned AudioProcessorGraph containing an 8-voice polyphonic synthesizer.

**Architecture:** JUCE AudioDeviceManager owns CoreAudio. An AudioProcessorGraph routes audio — its first node is a PolySynthProcessor with 8 voices, each having its own oscillator, filter, and ADSR. The ObjC bridge forwards MIDI-style noteOn/noteOff calls from TypeScript.

**Tech Stack:** C++17, JUCE 8.0.1 (11 modules), Objective-C++, Swift (Expo Modules), TypeScript

**Spec:** `docs/superpowers/specs/2026-05-11-polyphonic-synth-engine-design.md`

---

## File Map

### New Files
| File | Responsibility |
|------|---------------|
| `ios/Voice.h` | Single synth voice — oscillator, filter, ADSR, fade-out tail |
| `ios/Voice.mm` | Voice rendering and lifecycle |
| `ios/VoiceAllocator.h` | Note-to-voice mapping, stealing by quietest |
| `ios/VoiceAllocator.mm` | Allocation logic |
| `ios/PolySynthProcessor.h` | AudioProcessor subclass — 8 voices, global params |
| `ios/PolySynthProcessor.mm` | processBlock: render voices, sum, advance transport |
| `ios/TransportEngine.h` | Extracted transport (same logic as current) |
| `ios/TransportEngine.mm` | Transport implementation |
| `ios/BeeperAudioEngine.h` | Top-level coordinator — device, graph, player |
| `ios/BeeperAudioEngine.mm` | JUCE stack setup/teardown, method forwarding |

### Modified Files
| File | Change |
|------|--------|
| `src/scripts/vendor-juce.cjs:22-29` | Add 6 new modules to `MODULES_TO_VENDOR` |
| `ios/JuceConfig.h` | Enable all 11 modules, add MessageManager config |
| `ios/JuceModules.mm` | Add 6 new `#include` lines for new modules |
| `ios/ExpoJuce.podspec:29-31` | Add new iOS system frameworks |
| `ios/ExpoJuceBridge.h` | Replace mono API with poly API signatures |
| `ios/ExpoJuceBridge.m` | Forward to BeeperAudioEngine instead of JuceToneGenerator |
| `ios/ExpoJuceModule.swift` | New AsyncFunction definitions for poly API |
| `src/index.ts` | New TypeScript API exports |
| `src/ExpoJuce.types.ts` | No change needed (BeatEventPayload/ChangeEventPayload stay) |
| `src/ExpoJuceModule.ts` | No change needed (dynamic module loading via requireNativeModule) |

### Deleted Files
| File | Reason |
|------|--------|
| `ios/JuceToneGenerator.h` | Replaced by BeeperAudioEngine |
| `ios/JuceToneGenerator.mm` | Replaced by BeeperAudioEngine |

---

## Task 1: Vendor New JUCE Modules

**Files:**
- Modify: `src/scripts/vendor-juce.cjs:22-29`

- [ ] **Step 1: Add 6 new modules to the vendor script**

In `src/scripts/vendor-juce.cjs`, replace the `MODULES_TO_VENDOR` array (lines 22-29) with:

```javascript
const MODULES_TO_VENDOR = [
  'juce_core',
  'juce_events',
  'juce_data_structures',
  'juce_audio_basics',
  'juce_audio_formats',
  'juce_dsp',
  // New modules for AudioProcessorGraph + AudioDeviceManager
  'juce_graphics',
  'juce_gui_basics',
  'juce_gui_extra',
  'juce_audio_processors',
  'juce_audio_devices',
];
```

Note: `juce_events` and `juce_data_structures` are already in the list (they were vendored but not compiled). The 5 truly new modules are `juce_graphics`, `juce_gui_basics`, `juce_gui_extra`, `juce_audio_processors`, `juce_audio_devices`.

- [ ] **Step 2: Run the vendor script**

```bash
cd /Users/smokey/Internal/Beeper/code/expo-juce
node src/scripts/vendor-juce.cjs
```

Expected: 11 modules vendored successfully with sizes printed. Verify `ios/juce_modules/` contains all 11 directories (juce_core already existed).

- [ ] **Step 3: Verify all module directories exist**

```bash
ls ios/juce_modules/ | sort
```

Expected output should include: `juce_audio_basics`, `juce_audio_devices`, `juce_audio_formats`, `juce_audio_processors`, `juce_core`, `juce_data_structures`, `juce_dsp`, `juce_events`, `juce_graphics`, `juce_gui_basics`, `juce_gui_extra`

- [ ] **Step 4: Commit**

```bash
git add src/scripts/vendor-juce.cjs
git commit -m "build: add 5 new JUCE modules to vendor script"
```

Note: `ios/juce_modules/` is gitignored — only the script change is committed.

---

## Task 2: Update JuceConfig.h

**Files:**
- Modify: `ios/JuceConfig.h`

- [ ] **Step 1: Rewrite JuceConfig.h to enable all 11 modules**

Replace the entire contents of `ios/JuceConfig.h` with:

```cpp
#pragma once

// Xcode 16+ clang no longer implicitly includes these —
// JUCE uses FLT_MAX, FLT_MIN, INT_MAX, etc. throughout.
#include <cfloat>
#include <climits>
#include <cstdint>
#include <cmath>

// Tell JUCE that global module settings are configured
#define JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED 1

// Not a standalone JUCE app — embedded in React Native
#define JUCE_STANDALONE_APPLICATION 0
#define JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP 0

// App identity (required by some JUCE modules)
#define JUCE_APPLICATION_NAME "Beeper"
#define JUCE_APPLICATION_VERSION "0.4.0"

// Disable JUCE splash screen and analytics
#define JUCE_DISPLAY_SPLASH_SCREEN 0
#define JUCE_REPORT_APP_USAGE 0
#define JUCE_USE_DARK_SPLASH_SCREEN 0
#define JUCE_STRICT_REFCOUNTEDPOINTER 1

// ── Module availability flags (all 11 JUCE modules enabled) ──
#define JUCE_MODULE_AVAILABLE_juce_core 1
#define JUCE_MODULE_AVAILABLE_juce_events 1
#define JUCE_MODULE_AVAILABLE_juce_data_structures 1
#define JUCE_MODULE_AVAILABLE_juce_audio_basics 1
#define JUCE_MODULE_AVAILABLE_juce_audio_formats 1
#define JUCE_MODULE_AVAILABLE_juce_audio_devices 1
#define JUCE_MODULE_AVAILABLE_juce_audio_processors 1
#define JUCE_MODULE_AVAILABLE_juce_dsp 1
#define JUCE_MODULE_AVAILABLE_juce_graphics 1
#define JUCE_MODULE_AVAILABLE_juce_gui_basics 1
#define JUCE_MODULE_AVAILABLE_juce_gui_extra 1

// Modules we still don't use
#define JUCE_MODULE_AVAILABLE_juce_audio_plugin_client 0
#define JUCE_MODULE_AVAILABLE_juce_audio_utils 0
#define JUCE_MODULE_AVAILABLE_juce_cryptography 0
#define JUCE_MODULE_AVAILABLE_juce_opengl 0
#define JUCE_MODULE_AVAILABLE_juce_video 0
#define JUCE_MODULE_AVAILABLE_juce_product_unlocking 0

// Disable unit tests
#define JUCE_UNIT_TESTS 0

// Disable features we don't need
#define JUCE_USE_CURL 0
#define JUCE_WEB_BROWSER 0
#define JUCE_USE_OGGVORBIS 0
#define JUCE_USE_MP3AUDIOFORMAT 0
#define JUCE_USE_LAME_AUDIO_FORMAT 0
#define JUCE_USE_WINDOWS_MEDIA_FORMAT 0
#define JUCE_USE_FLAC 0

// Audio device config
#define JUCE_ASIO 0
#define JUCE_WASAPI 0
#define JUCE_DIRECTSOUND 0
#define JUCE_ALSA 0
#define JUCE_JACK 0
#define JUCE_BELA 0
#define JUCE_USE_ANDROID_OBOE 0
#define JUCE_USE_ANDROID_OPENSLES 0
#define JUCE_USE_WINRT_MIDI 0
#define JUCE_DISABLE_AUDIO_MIXING_WITH_OTHER_APPS 0

// Plugin format config (we don't host plugins)
#define JUCE_PLUGINHOST_VST 0
#define JUCE_PLUGINHOST_VST3 0
#define JUCE_PLUGINHOST_AU 0
#define JUCE_PLUGINHOST_LADSPA 0
#define JUCE_PLUGINHOST_LV2 0
#define JUCE_PLUGINHOST_ARA 0
```

- [ ] **Step 2: Commit**

```bash
git add ios/JuceConfig.h
git commit -m "build: enable all 12 JUCE modules in JuceConfig.h"
```

---

## Task 3: Update JuceModules.mm and Podspec

**Files:**
- Modify: `ios/JuceModules.mm`
- Modify: `ios/ExpoJuce.podspec:24-31`

- [ ] **Step 1: Rewrite JuceModules.mm to compile all modules**

Replace the entire contents of `ios/JuceModules.mm` with:

```objcpp
// JuceModules.mm — Compiles vendored JUCE modules as a single ObjC++ translation unit.
// The .mm extension ensures Objective-C++ mode so JUCE's Apple platform code compiles correctly.
//
// All 12 JUCE modules are compiled here. juce_events is now enabled —
// MessageManager is initialized explicitly in BeeperAudioEngine::setup()
// and attaches to React Native's existing CFRunLoop (no conflict).

#include "JuceConfig.h"

// Foundation
#include "juce_modules/juce_core/juce_core.cpp"
#include "juce_modules/juce_events/juce_events.cpp"
#include "juce_modules/juce_data_structures/juce_data_structures.cpp"

// Graphics (compile-time dependency of juce_audio_processors, not used for rendering)
#include "juce_modules/juce_graphics/juce_graphics.cpp"
#include "juce_modules/juce_gui_basics/juce_gui_basics.cpp"
#include "juce_modules/juce_gui_extra/juce_gui_extra.cpp"

// Audio
#include "juce_modules/juce_audio_basics/juce_audio_basics.cpp"
#include "juce_modules/juce_audio_formats/juce_audio_formats.cpp"
#include "juce_modules/juce_audio_devices/juce_audio_devices.cpp"
#include "juce_modules/juce_audio_processors/juce_audio_processors.cpp"
#include "juce_modules/juce_dsp/juce_dsp.cpp"
```

- [ ] **Step 2: Update the podspec to add required system frameworks**

In `ios/ExpoJuce.podspec`, replace lines 29-31:

```ruby
  s.frameworks = 'AVFoundation', 'AudioToolbox', 'CoreAudio',
                 'Foundation', 'Accelerate', 'CoreMIDI'
```

with:

```ruby
  s.frameworks = 'AVFoundation', 'AudioToolbox', 'CoreAudio',
                 'Foundation', 'Accelerate', 'CoreMIDI',
                 'CoreGraphics', 'CoreImage', 'CoreText',
                 'QuartzCore', 'UIKit', 'WebKit', 'Metal', 'MetalKit'
  s.weak_frameworks = 'UserNotifications'
```

Also update the comment on line 24 to reflect the new module count:

```ruby
  # JUCE modules compiled: all 12 (core, events, data_structures, graphics, gui_basics,
  # gui_extra, audio_basics, audio_formats, audio_devices, audio_processors, dsp)
```

- [ ] **Step 3: Commit**

```bash
git add ios/JuceModules.mm ios/ExpoJuce.podspec
git commit -m "build: compile all 12 JUCE modules, add system frameworks"
```

---

## Task 4: Verify Build Compiles

**Files:** None (verification only)

- [ ] **Step 1: Run vendor script to populate juce_modules**

```bash
cd /Users/smokey/Internal/Beeper/code/expo-juce
node src/scripts/vendor-juce.cjs
```

- [ ] **Step 2: Build the TypeScript layer**

```bash
npm run build
```

Expected: TypeScript compiles without errors.

- [ ] **Step 3: Install pods in example app and verify Xcode build**

```bash
cd example
npx pod-install
cd ios
xcodebuild -workspace expojuceexample.xcworkspace \
  -scheme expojuceexample \
  -configuration Debug \
  -sdk iphonesimulator \
  -destination 'platform=iOS Simulator,name=iPhone 15' \
  build 2>&1 | tail -20
```

Expected: Build succeeds. If there are JUCE compilation errors (likely around missing defines or framework conflicts), fix them in `JuceConfig.h` before proceeding.

**Troubleshooting:**
- If `juce_gui_basics` fails with Metal/MetalKit errors, ensure those frameworks are in the podspec
- If `MessageManager` static initializer causes issues, verify `JUCE_STANDALONE_APPLICATION 0` is set
- If header search paths fail, verify `HEADER_SEARCH_PATHS` in podspec includes `$(PODS_TARGET_SRCROOT)/juce_modules`

- [ ] **Step 4: Commit any build fixes**

```bash
git add -A
git commit -m "fix: resolve JUCE module build issues"
```

(Skip if no fixes needed.)

---

## Tasks 5-8: C++ Engine Classes (batch)

> **Note:** Tasks 5-8 create interdependent C++ files. The podspec compiles all `*.mm` files, so intermediate builds may fail due to unresolved symbols. Treat Tasks 5-8 as a batch — commit after each task but only do a full build verification after Task 8 is complete. The build check in Task 6 Step 3 is optional and may fail until all classes exist.

## Task 5: Extract TransportEngine

**Files:**
- Create: `ios/TransportEngine.h`
- Create: `ios/TransportEngine.mm`

- [ ] **Step 1: Create TransportEngine.h**

```cpp
#pragma once

#include <atomic>

class TransportEngine {
public:
    TransportEngine();

    void setTempo(double newBpm);
    double getTempo();

    void setPosition(double beat);
    double getPosition();

    void start();
    void stop();
    bool isPlaying();

    // Advance by one block of samples. Returns true if a beat boundary was crossed.
    // Called from audio thread only.
    bool advanceBlock(int numSamples, double sampleRate);

private:
    // Written by main thread, read by audio thread
    std::atomic<double> bpm;
    std::atomic<bool> playing;
    std::atomic<double> pendingPosition; // -1.0 means no pending seek
    std::atomic<bool> hasPendingPosition;

    // Audio-thread only (not atomic — only touched in advanceBlock)
    double beatPosition = 0.0;
    int lastBeatInt = -1;

    // Snapshot for main thread reading (written by audio thread)
    std::atomic<double> positionSnapshot;
};
```

- [ ] **Step 2: Create TransportEngine.mm**

```cpp
#include "TransportEngine.h"
#include <algorithm>

TransportEngine::TransportEngine()
    : bpm(120.0), playing(false), pendingPosition(-1.0),
      hasPendingPosition(false), positionSnapshot(0.0) {}

void TransportEngine::setTempo(double newBpm) {
    bpm.store(std::clamp(newBpm, 20.0, 400.0));
}

double TransportEngine::getTempo() { return bpm.load(); }

void TransportEngine::setPosition(double beat) {
    pendingPosition.store(beat);
    hasPendingPosition.store(true);
}

double TransportEngine::getPosition() { return positionSnapshot.load(); }

void TransportEngine::start() { playing.store(true); }
void TransportEngine::stop() { playing.store(false); }
bool TransportEngine::isPlaying() { return playing.load(); }

bool TransportEngine::advanceBlock(int numSamples, double sampleRate) {
    // Apply pending seek from main thread
    if (hasPendingPosition.exchange(false)) {
        beatPosition = pendingPosition.load();
        lastBeatInt = (int)beatPosition;
    }

    if (!playing.load()) {
        positionSnapshot.store(beatPosition);
        return false;
    }

    double currentBpm = bpm.load();
    double beatsPerSample = currentBpm / (60.0 * sampleRate);
    double beatsThisBlock = beatsPerSample * numSamples;

    double newPos = beatPosition + beatsThisBlock;
    int oldBeatInt = lastBeatInt;
    int newBeatInt = (int)newPos;

    beatPosition = newPos;
    lastBeatInt = newBeatInt;
    positionSnapshot.store(newPos);

    return (newBeatInt != oldBeatInt);
}
```

- [ ] **Step 3: Commit**

```bash
git add ios/TransportEngine.h ios/TransportEngine.mm
git commit -m "refactor: extract TransportEngine into own files"
```

---

## Task 6: Create Voice

**Files:**
- Create: `ios/Voice.h`
- Create: `ios/Voice.mm`

- [ ] **Step 1: Create Voice.h**

```cpp
#pragma once

#include "JuceConfig.h"
#include <juce_dsp/juce_dsp.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>

enum class Waveform { Sine, Square, Saw, Triangle };

static const int FADE_OUT_SAMPLES = 96;

class Voice {
public:
    Voice();

    void prepare(double sampleRate, int blockSize);
    void renderBlock(float* buffer, int numSamples,
                     Waveform waveform, double frequency,
                     float filterCutoff, float filterResonance,
                     float masterLevel);

    void startNote(int note, float velocity);
    void stopNote();

    // Capture current state into fadeOutTail, then free the voice slot.
    // Called before reassigning this voice to a new note.
    void captureAndRelease(Waveform waveform, double frequency,
                           float filterCutoff, float filterResonance,
                           float masterLevel);

    bool isActive() const;

    int getNoteNumber() const { return noteNumber; }
    float getCurrentLevel() const { return currentLevel; }

    // Fade-out tail buffer — plays independently after voice is stolen.
    // Mixed into output by PolySynthProcessor::processBlock.
    float fadeOutTail[FADE_OUT_SAMPLES];
    int fadeOutTailLength = 0;
    int fadeOutTailPos = 0;
    bool hasFadeOutTail() const { return fadeOutTailPos < fadeOutTailLength; }

    // ADSR params (set from main thread, read on audio thread)
    std::atomic<float> attackMs{10.0f};
    std::atomic<float> decayMs{0.0f};
    std::atomic<float> sustainLevel{1.0f};
    std::atomic<float> releaseMs{200.0f};

private:
    void updateADSR();

    juce::dsp::Oscillator<float> sineOsc;
    juce::dsp::Oscillator<float> squareOsc;
    juce::dsp::Oscillator<float> sawOsc;
    juce::dsp::Oscillator<float> triOsc;
    juce::dsp::StateVariableTPTFilter<float> filter;
    juce::ADSR adsr;

    int noteNumber = -1;
    float velocityGain = 0.0f;
    float currentLevel = 0.0f;
    double cachedSampleRate = 44100.0;
    bool prepared = false;
};
```

- [ ] **Step 2: Create Voice.mm**

```cpp
#include "Voice.h"
#include <cmath>
#include <algorithm>

Voice::Voice() {
    std::memset(fadeOutTail, 0, sizeof(fadeOutTail));
}

void Voice::prepare(double sampleRate, int blockSize) {
    juce::dsp::ProcessSpec spec;
    spec.sampleRate = sampleRate;
    spec.maximumBlockSize = (juce::uint32)blockSize;
    spec.numChannels = 1;

    sineOsc.initialise([](float x) { return std::sin(x); }, 2048);
    sineOsc.prepare(spec);

    squareOsc.initialise([](float x) -> float { return x < 0.0f ? -1.0f : 1.0f; }, 2048);
    squareOsc.prepare(spec);

    sawOsc.initialise([](float x) -> float { return x / juce::MathConstants<float>::pi; }, 2048);
    sawOsc.prepare(spec);

    triOsc.initialise([](float x) -> float {
        return 2.0f * std::abs(x) / juce::MathConstants<float>::pi - 1.0f;
    }, 2048);
    triOsc.prepare(spec);

    filter.prepare(spec);
    filter.setType(juce::dsp::StateVariableTPTFilter<float>::Type::lowpass);

    adsr.setSampleRate(sampleRate);
    cachedSampleRate = sampleRate;
    prepared = true;
}

void Voice::startNote(int note, float velocity) {
    noteNumber = note;
    velocityGain = velocity / 127.0f;
    fadingOut = false;
    fadeOutCounter = 0;

    // Convert MIDI note to frequency: A4 = 440Hz = note 69
    float freq = 440.0f * std::pow(2.0f, (note - 69) / 12.0f);
    sineOsc.setFrequency(freq, true);
    squareOsc.setFrequency(freq, true);
    sawOsc.setFrequency(freq, true);
    triOsc.setFrequency(freq, true);

    sineOsc.reset();
    squareOsc.reset();
    sawOsc.reset();
    triOsc.reset();
    filter.reset();

    updateADSR();
    adsr.noteOn();
}

void Voice::stopNote() {
    adsr.noteOff();
}

void Voice::captureAndRelease(Waveform waveform, double frequency,
                               float filterCutoff, float filterResonance,
                               float masterLevel) {
    // Render remaining fade-out into the tail buffer
    fadeOutTailLength = FADE_OUT_SAMPLES;
    fadeOutTailPos = 0;
    std::memset(fadeOutTail, 0, sizeof(fadeOutTail));

    for (int i = 0; i < FADE_OUT_SAMPLES; i++) {
        float envGain = adsr.getNextSample();

        // Generate sample from active waveform
        float sample = 0.0f;
        switch (waveform) {
            case Waveform::Sine:    sample = sineOsc.processSample(0.0f); break;
            case Waveform::Square:  sample = squareOsc.processSample(0.0f) * 0.7f; break;
            case Waveform::Saw:     sample = sawOsc.processSample(0.0f) * 0.6f; break;
            case Waveform::Triangle: sample = triOsc.processSample(0.0f) * 0.8f; break;
        }

        float filtered = filter.processSample(0, sample);
        float fadeGain = 1.0f - ((float)i / (float)FADE_OUT_SAMPLES); // linear ramp down
        fadeOutTail[i] = filtered * envGain * velocityGain * masterLevel * fadeGain;
    }

    // Free the voice slot
    noteNumber = -1;
    currentLevel = 0.0f;
}

bool Voice::isActive() const {
    return noteNumber >= 0 && adsr.isActive();
}

void Voice::updateADSR() {
    juce::ADSR::Parameters p {
        attackMs.load() * 0.001f,
        decayMs.load() * 0.001f,
        sustainLevel.load(),
        releaseMs.load() * 0.001f
    };
    adsr.setParameters(p);
}

void Voice::renderBlock(float* buffer, int numSamples,
                        Waveform waveform, double frequency,
                        float filterCutoff, float filterResonance,
                        float masterLevel) {
    if (!prepared || !isActive()) return;

    // Apply detune (frequency already includes detune from caller)
    float freqF = (float)frequency;
    if (freqF > 0.0f) {
        sineOsc.setFrequency(freqF, true);
        squareOsc.setFrequency(freqF, true);
        sawOsc.setFrequency(freqF, true);
        triOsc.setFrequency(freqF, true);
    }

    // Update filter
    float mappedQ = 0.707f + filterResonance * 9.293f;
    filter.setCutoffFrequency(filterCutoff);
    filter.setResonance(mappedQ);

    // Update ADSR if params changed
    updateADSR();

    for (int i = 0; i < numSamples; i++) {
        float envGain = adsr.getNextSample();
        if (envGain < 0.00001f && !adsr.isActive()) {
            noteNumber = -1;
            return;
        }

        // Generate sample from active waveform
        float sample = 0.0f;
        switch (waveform) {
            case Waveform::Sine:
                sample = sineOsc.processSample(0.0f);
                break;
            case Waveform::Square:
                sample = squareOsc.processSample(0.0f) * 0.7f;
                break;
            case Waveform::Saw:
                sample = sawOsc.processSample(0.0f) * 0.6f;
                break;
            case Waveform::Triangle:
                sample = triOsc.processSample(0.0f) * 0.8f;
                break;
        }

        // Advance inactive oscillators for phase coherence
        if (waveform != Waveform::Sine)     sineOsc.processSample(0.0f);
        if (waveform != Waveform::Square)   squareOsc.processSample(0.0f);
        if (waveform != Waveform::Saw)      sawOsc.processSample(0.0f);
        if (waveform != Waveform::Triangle) triOsc.processSample(0.0f);

        // Apply filter
        float filtered = filter.processSample(0, sample);

        // Apply envelope, velocity, master level
        float out = filtered * envGain * velocityGain * masterLevel;

        currentLevel = envGain * velocityGain;
        buffer[i] += out;
    }
}
```

- [ ] **Step 3: Verify build compiles with Voice files**

```bash
cd /Users/smokey/Internal/Beeper/code/expo-juce/example
xcodebuild -workspace ios/expojuceexample.xcworkspace \
  -scheme expojuceexample \
  -configuration Debug \
  -sdk iphonesimulator \
  -destination 'platform=iOS Simulator,name=iPhone 15' \
  build 2>&1 | tail -5
```

- [ ] **Step 4: Commit**

```bash
git add ios/Voice.h ios/Voice.mm
git commit -m "feat: add Voice class — per-voice osc + filter + ADSR"
```

---

## Task 7: Create VoiceAllocator

**Files:**
- Create: `ios/VoiceAllocator.h`
- Create: `ios/VoiceAllocator.mm`

- [ ] **Step 1: Create VoiceAllocator.h**

```cpp
#pragma once

#include "Voice.h"

static const int MAX_VOICES = 8;

class VoiceAllocator {
public:
    VoiceAllocator();

    // Returns the voice index assigned (or stolen). -1 on failure.
    // Needs current global params for rendering the stolen voice's fade-out tail.
    int noteOn(int note, float velocity, Voice* voices,
               Waveform waveform, double detuneCents,
               float filterCutoff, float filterResonance,
               float masterLevel);

    // Triggers release on the voice playing this note.
    void noteOff(int note, Voice* voices);

    // Release all voices immediately.
    void allNotesOff(Voice* voices);

private:
    // Find a free voice, or steal the quietest.
    int findVoice(Voice* voices);

    // Track note-to-voice for O(1) noteOff.
    // noteMap[midiNote] = voiceIndex, or -1 if not playing.
    int noteMap[128];
};
```

- [ ] **Step 2: Create VoiceAllocator.mm**

```cpp
#include "VoiceAllocator.h"
#include <cstring>

VoiceAllocator::VoiceAllocator() {
    std::memset(noteMap, -1, sizeof(noteMap));
}

int VoiceAllocator::findVoice(Voice* voices) {
    // First: find a free voice
    for (int i = 0; i < MAX_VOICES; i++) {
        if (!voices[i].isActive()) {
            return i;
        }
    }

    // All voices active — steal the quietest
    int quietest = 0;
    float quietestLevel = voices[0].getCurrentLevel();
    for (int i = 1; i < MAX_VOICES; i++) {
        float level = voices[i].getCurrentLevel();
        if (level < quietestLevel) {
            quietestLevel = level;
            quietest = i;
        }
    }
    return quietest;
}

int VoiceAllocator::noteOn(int note, float velocity, Voice* voices,
                           Waveform waveform, double detuneCents,
                           float filterCutoff, float filterResonance,
                           float masterLevel) {
    // If this note is already playing, stop it first
    if (note >= 0 && note < 128 && noteMap[note] >= 0) {
        int existing = noteMap[note];
        if (existing < MAX_VOICES && voices[existing].getNoteNumber() == note) {
            voices[existing].stopNote();
        }
        noteMap[note] = -1;
    }

    int voiceIdx = findVoice(voices);
    if (voiceIdx < 0) return -1;

    Voice& voice = voices[voiceIdx];

    // If stealing an active voice, capture its fade-out into the tail buffer
    if (voice.isActive()) {
        // Calculate the stolen voice's current frequency for tail rendering
        int oldNote = voice.getNoteNumber();
        double oldFreq = 440.0 * std::pow(2.0, (oldNote - 69) / 12.0);
        if (std::fabs(detuneCents) > 0.01) {
            oldFreq *= std::pow(2.0, detuneCents / 1200.0);
        }

        voice.captureAndRelease(waveform, oldFreq,
                                filterCutoff, filterResonance, masterLevel);

        if (oldNote >= 0 && oldNote < 128) {
            noteMap[oldNote] = -1;
        }
    }

    voice.startNote(note, velocity);

    if (note >= 0 && note < 128) {
        noteMap[note] = voiceIdx;
    }

    return voiceIdx;
}

void VoiceAllocator::noteOff(int note, Voice* voices) {
    if (note < 0 || note >= 128) return;

    int voiceIdx = noteMap[note];
    if (voiceIdx >= 0 && voiceIdx < MAX_VOICES) {
        if (voices[voiceIdx].getNoteNumber() == note) {
            voices[voiceIdx].stopNote();
        }
        noteMap[note] = -1;
    }
}

void VoiceAllocator::allNotesOff(Voice* voices) {
    for (int i = 0; i < MAX_VOICES; i++) {
        if (voices[i].isActive()) {
            voices[i].stopNote();
        }
    }
    std::memset(noteMap, -1, sizeof(noteMap));
}
```

- [ ] **Step 3: Commit**

```bash
git add ios/VoiceAllocator.h ios/VoiceAllocator.mm
git commit -m "feat: add VoiceAllocator — note mapping + quietest stealing"
```

---

## Task 8: Create PolySynthProcessor

**Files:**
- Create: `ios/PolySynthProcessor.h`
- Create: `ios/PolySynthProcessor.mm`

- [ ] **Step 1: Create PolySynthProcessor.h**

```cpp
#pragma once

#include "JuceConfig.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include "Voice.h"
#include "VoiceAllocator.h"
#include "TransportEngine.h"

struct BeatState {
    std::atomic<bool> crossed{false};
    std::atomic<double> beat{0.0};
    std::atomic<double> bpm{120.0};
};

class PolySynthProcessor : public juce::AudioProcessor {
public:
    PolySynthProcessor();
    ~PolySynthProcessor() override = default;

    // AudioProcessor overrides
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    const juce::String getName() const override { return "PolySynth"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    // Editor stubs (required by AudioProcessor but unused)
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}

    // ── MIDI-style note control ──
    void noteOn(int note, int velocity);
    void noteOff(int note);
    void allNotesOff();

    // ── Global params (main thread setters) ──
    void setWaveform(Waveform wf);
    void setFilterCutoff(double hz);
    void setFilterResonance(double q);
    void setAttack(double ms);
    void setDecay(double ms);
    void setSustain(double level);
    void setRelease(double ms);
    void setDetune(double cents);
    void setMasterLevel(double level);

    // Transport (owned externally, pointer set by BeeperAudioEngine)
    void setTransport(TransportEngine* t) { transport = t; }
    BeatState beatState;

private:
    Voice voices[MAX_VOICES];
    VoiceAllocator allocator;
    TransportEngine* transport = nullptr;

    // Global params (atomics for thread safety)
    // Use int for waveform to guarantee lock-free on all platforms
    std::atomic<int> waveform{static_cast<int>(Waveform::Sine)};
    std::atomic<float> filterCutoff{20000.0f};
    std::atomic<float> filterResonance{0.0f};
    std::atomic<float> detuneCents{0.0f};
    std::atomic<float> masterLevel{0.5f};
};
```

- [ ] **Step 2: Create PolySynthProcessor.mm**

```cpp
#include "PolySynthProcessor.h"
#include <cmath>
#include <algorithm>

PolySynthProcessor::PolySynthProcessor()
    : AudioProcessor(BusesProperties()
        .withOutput("Output", juce::AudioChannelSet::stereo(), true)) {
}

void PolySynthProcessor::prepareToPlay(double sampleRate, int samplesPerBlock) {
    for (int i = 0; i < MAX_VOICES; i++) {
        voices[i].prepare(sampleRate, samplesPerBlock);
    }
}

void PolySynthProcessor::releaseResources() {}

void PolySynthProcessor::processBlock(juce::AudioBuffer<float>& buffer,
                                       juce::MidiBuffer& /*midiMessages*/) {
    buffer.clear();

    int numSamples = buffer.getNumSamples();
    double sr = getSampleRate();

    // Read global params once per block
    Waveform wf = static_cast<Waveform>(waveform.load());
    float cutoff = filterCutoff.load();
    float reso = filterResonance.load();
    float detune = detuneCents.load();
    float master = masterLevel.load();

    // Render mono into channel 0, then duplicate to stereo
    float* monoBuffer = buffer.getWritePointer(0);

    // Mix in any active fade-out tails from stolen voices
    for (int v = 0; v < MAX_VOICES; v++) {
        if (voices[v].hasFadeOutTail()) {
            int tailRemaining = voices[v].fadeOutTailLength - voices[v].fadeOutTailPos;
            int samplesToMix = std::min(tailRemaining, numSamples);
            for (int i = 0; i < samplesToMix; i++) {
                monoBuffer[i] += voices[v].fadeOutTail[voices[v].fadeOutTailPos + i];
            }
            voices[v].fadeOutTailPos += samplesToMix;
        }
    }

    // Render active voices
    for (int v = 0; v < MAX_VOICES; v++) {
        if (!voices[v].isActive()) continue;

        // Calculate detuned frequency for this voice
        int note = voices[v].getNoteNumber();
        double freq = 440.0 * std::pow(2.0, (note - 69) / 12.0);
        if (std::fabs(detune) > 0.01f) {
            freq *= std::pow(2.0, (double)detune / 1200.0);
        }

        voices[v].renderBlock(monoBuffer, numSamples,
                              wf, freq, cutoff, reso, master);
    }

    // Duplicate mono to stereo (channel 0 → channel 1)
    if (buffer.getNumChannels() > 1) {
        buffer.copyFrom(1, 0, buffer, 0, 0, numSamples);
    }

    // Advance transport (per-block, not per-sample)
    if (transport) {
        if (transport->advanceBlock(numSamples, sr)) {
            beatState.crossed.store(true);
            beatState.beat.store(transport->getPosition());
            beatState.bpm.store(transport->getTempo());
        }
    }
}

// ── Note control ──

void PolySynthProcessor::noteOn(int note, int velocity) {
    int clampedNote = std::clamp(note, 0, 127);
    int clampedVel = std::clamp(velocity, 0, 127);
    allocator.noteOn(clampedNote, (float)clampedVel, voices,
                     static_cast<Waveform>(waveform.load()),
                     (double)detuneCents.load(),
                     filterCutoff.load(), filterResonance.load(),
                     masterLevel.load());
}

void PolySynthProcessor::noteOff(int note) {
    int clampedNote = std::clamp(note, 0, 127);
    allocator.noteOff(clampedNote, voices);
}

void PolySynthProcessor::allNotesOff() {
    allocator.allNotesOff(voices);
}

// ── Global param setters ──

void PolySynthProcessor::setWaveform(Waveform wf) { waveform.store(static_cast<int>(wf)); }

void PolySynthProcessor::setFilterCutoff(double hz) {
    filterCutoff.store((float)std::clamp(hz, 20.0, 20000.0));
}

void PolySynthProcessor::setFilterResonance(double q) {
    filterResonance.store((float)std::clamp(q, 0.0, 1.0));
}

void PolySynthProcessor::setAttack(double ms) {
    float val = (float)std::clamp(ms, 1.0, 10000.0);
    for (int i = 0; i < MAX_VOICES; i++) voices[i].attackMs.store(val);
}

void PolySynthProcessor::setDecay(double ms) {
    float val = (float)std::clamp(ms, 1.0, 10000.0);
    for (int i = 0; i < MAX_VOICES; i++) voices[i].decayMs.store(val);
}

void PolySynthProcessor::setSustain(double level) {
    float val = (float)std::clamp(level, 0.0, 1.0);
    for (int i = 0; i < MAX_VOICES; i++) voices[i].sustainLevel.store(val);
}

void PolySynthProcessor::setRelease(double ms) {
    float val = (float)std::clamp(ms, 1.0, 10000.0);
    for (int i = 0; i < MAX_VOICES; i++) voices[i].releaseMs.store(val);
}

void PolySynthProcessor::setDetune(double cents) {
    detuneCents.store((float)std::clamp(cents, -1200.0, 1200.0));
}

void PolySynthProcessor::setMasterLevel(double level) {
    masterLevel.store((float)std::clamp(level, 0.0, 1.0));
}
```

- [ ] **Step 3: Commit**

```bash
git add ios/PolySynthProcessor.h ios/PolySynthProcessor.mm
git commit -m "feat: add PolySynthProcessor — 8-voice AudioProcessor with graph support"
```

---

## Task 9: Create BeeperAudioEngine

**Files:**
- Create: `ios/BeeperAudioEngine.h`
- Create: `ios/BeeperAudioEngine.mm`

- [ ] **Step 1: Create BeeperAudioEngine.h**

```objcpp
#pragma once

#import <Foundation/Foundation.h>

typedef void(^BeatCallback)(double beat, double bpm);

@interface BeeperAudioEngine : NSObject

+ (instancetype)sharedInstance;

// Lifecycle
- (void)setup;
- (void)shutdown;

// Synth — MIDI-style
- (void)noteOn:(int)note velocity:(int)velocity;
- (void)noteOff:(int)note;
- (void)allNotesOff;

// Global synth params
- (void)setWaveform:(NSString *)waveform;
- (void)setFilterCutoff:(double)hz;
- (void)setFilterResonance:(double)q;
- (void)setAttack:(double)ms;
- (void)setDecay:(double)ms;
- (void)setSustain:(double)level;
- (void)setRelease:(double)ms;
- (void)setDetune:(double)cents;
- (void)setMasterLevel:(double)level;

// Transport
- (void)startTransport;
- (void)stopTransport;
- (void)setTempo:(double)bpm;
- (void)setTransportPosition:(double)beat;
- (double)getTransportPosition;
- (double)getTempo;
- (BOOL)isTransportPlaying;

// Beat events
- (void)setBeatCallback:(BeatCallback)callback;
- (void)startBeatTimer;
- (void)stopBeatTimer;

@end
```

- [ ] **Step 2: Create BeeperAudioEngine.mm**

```objcpp
#import "BeeperAudioEngine.h"
#import <AVFoundation/AVFoundation.h>
#include "JuceConfig.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "PolySynthProcessor.h"
#include "TransportEngine.h"

@interface BeeperAudioEngine ()
@property (nonatomic, assign) bool isReady;
@property (nonatomic, copy) BeatCallback beatCallback;
@property (nonatomic, strong) dispatch_source_t beatTimer;
@end

// C++ members stored as raw pointers (ObjC class can't have C++ members inline)
static juce::AudioDeviceManager* sDeviceManager = nullptr;
static juce::AudioProcessorPlayer* sPlayer = nullptr;
static std::unique_ptr<juce::AudioProcessorGraph> sGraph;
static PolySynthProcessor* sSynth = nullptr; // owned by graph
static TransportEngine* sTransport = nullptr;
static juce::AudioProcessorGraph::Node::Ptr sSynthNode;
static juce::AudioProcessorGraph::Node::Ptr sOutputNode;

@implementation BeeperAudioEngine

+ (instancetype)sharedInstance {
    static BeeperAudioEngine *instance = nil;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        instance = [[self alloc] init];
    });
    return instance;
}

- (instancetype)init {
    self = [super init];
    if (self) {
        _isReady = false;
    }
    return self;
}

- (void)setup {
    NSAssert([NSThread isMainThread], @"setup() must be called on main thread");

    if (self.isReady) {
        NSLog(@"[BeeperAudioEngine] Already initialized, skipping");
        return;
    }

    NSLog(@"[BeeperAudioEngine] Initializing JUCE audio stack...");

    // 0. Configure AVAudioSession before JUCE takes over
    NSError *sessionError = nil;
    AVAudioSession *session = [AVAudioSession sharedInstance];
    [session setCategory:AVAudioSessionCategoryPlayAndRecord
             withOptions:(AVAudioSessionCategoryOptionMixWithOthers |
                          AVAudioSessionCategoryOptionDefaultToSpeaker)
                   error:&sessionError];
    if (sessionError) {
        NSLog(@"[BeeperAudioEngine] AVAudioSession category error: %@", sessionError);
    }
    [session setActive:YES error:&sessionError];
    if (sessionError) {
        NSLog(@"[BeeperAudioEngine] AVAudioSession activation error: %@", sessionError);
    }

    // 1. Initialize MessageManager (attaches to existing CFRunLoop)
    juce::MessageManager::getInstance();

    // 2. Create transport
    sTransport = new TransportEngine();

    // 3. Create AudioDeviceManager
    sDeviceManager = new juce::AudioDeviceManager();

    // 4. Initialize device: 0 inputs, 2 outputs (stereo)
    juce::String error = sDeviceManager->initialise(0, 2, nullptr, true);
    if (error.isNotEmpty()) {
        NSLog(@"[BeeperAudioEngine] AudioDeviceManager init failed: %s", error.toRawUTF8());
        delete sDeviceManager;
        sDeviceManager = nullptr;
        delete sTransport;
        sTransport = nullptr;
        return;
    }

    // 5. Create AudioProcessorGraph
    sGraph = std::make_unique<juce::AudioProcessorGraph>();
    sGraph->setPlayConfigDetails(0, 2, sDeviceManager->getAudioDeviceSetup().sampleRate,
                                  sDeviceManager->getAudioDeviceSetup().bufferSize);
    sGraph->prepareToPlay(sDeviceManager->getAudioDeviceSetup().sampleRate,
                           sDeviceManager->getAudioDeviceSetup().bufferSize);

    // 6. Add PolySynthProcessor node
    auto synthProcessor = std::make_unique<PolySynthProcessor>();
    sSynth = synthProcessor.get();
    sSynth->setTransport(sTransport);
    sSynthNode = sGraph->addNode(std::move(synthProcessor));

    // 7. Add output node
    sOutputNode = sGraph->addNode(
        std::make_unique<juce::AudioProcessorGraph::AudioGraphIOProcessor>(
            juce::AudioProcessorGraph::AudioGraphIOProcessor::audioOutputNode));

    // 8. Connect synth → output (stereo: channels 0 and 1)
    sGraph->addConnection({
        {sSynthNode->nodeID, 0},
        {sOutputNode->nodeID, 0}
    });
    sGraph->addConnection({
        {sSynthNode->nodeID, 1},
        {sOutputNode->nodeID, 1}
    });

    // 9. Create player and attach to device
    sPlayer = new juce::AudioProcessorPlayer();
    sPlayer->setProcessor(sGraph.get());
    sDeviceManager->addAudioCallback(sPlayer);

    self.isReady = true;
    NSLog(@"[BeeperAudioEngine] JUCE audio stack initialized (sr=%.0f, buf=%d)",
          sDeviceManager->getAudioDeviceSetup().sampleRate,
          sDeviceManager->getAudioDeviceSetup().bufferSize);
}

- (void)shutdown {
    NSAssert([NSThread isMainThread], @"shutdown() must be called on main thread");

    if (!self.isReady) return;

    NSLog(@"[BeeperAudioEngine] Shutting down...");

    [self stopBeatTimer];

    if (sSynth) {
        sSynth->allNotesOff();
    }

    if (sDeviceManager && sPlayer) {
        sDeviceManager->removeAudioCallback(sPlayer);
    }

    if (sPlayer) {
        sPlayer->setProcessor(nullptr);
        delete sPlayer;
        sPlayer = nullptr;
    }

    sSynthNode = nullptr;
    sOutputNode = nullptr;
    sSynth = nullptr;
    sGraph.reset();

    if (sDeviceManager) {
        sDeviceManager->closeAudioDevice();
        delete sDeviceManager;
        sDeviceManager = nullptr;
    }

    if (sTransport) {
        delete sTransport;
        sTransport = nullptr;
    }

    self.isReady = false;
    NSLog(@"[BeeperAudioEngine] Shutdown complete");
}

// ── Synth forwarding ──

- (void)noteOn:(int)note velocity:(int)velocity {
    if (!self.isReady || !sSynth) {
        NSLog(@"[BeeperAudioEngine] noteOn ignored — engine not initialized. Call setup() first.");
        return;
    }
    sSynth->noteOn(note, velocity);
}

- (void)noteOff:(int)note {
    if (!self.isReady || !sSynth) {
        NSLog(@"[BeeperAudioEngine] noteOff ignored — engine not initialized.");
        return;
    }
    sSynth->noteOff(note);
}

- (void)allNotesOff {
    if (!self.isReady || !sSynth) return;
    sSynth->allNotesOff();
}

- (void)setWaveform:(NSString *)waveform {
    if (!self.isReady || !sSynth) return;
    if ([waveform isEqualToString:@"square"]) {
        sSynth->setWaveform(Waveform::Square);
    } else if ([waveform isEqualToString:@"saw"]) {
        sSynth->setWaveform(Waveform::Saw);
    } else if ([waveform isEqualToString:@"triangle"]) {
        sSynth->setWaveform(Waveform::Triangle);
    } else {
        sSynth->setWaveform(Waveform::Sine);
    }
}

- (void)setFilterCutoff:(double)hz {
    if (self.isReady && sSynth) sSynth->setFilterCutoff(hz);
}

- (void)setFilterResonance:(double)q {
    if (self.isReady && sSynth) sSynth->setFilterResonance(q);
}

- (void)setAttack:(double)ms {
    if (self.isReady && sSynth) sSynth->setAttack(ms);
}

- (void)setDecay:(double)ms {
    if (self.isReady && sSynth) sSynth->setDecay(ms);
}

- (void)setSustain:(double)level {
    if (self.isReady && sSynth) sSynth->setSustain(level);
}

- (void)setRelease:(double)ms {
    if (self.isReady && sSynth) sSynth->setRelease(ms);
}

- (void)setDetune:(double)cents {
    if (self.isReady && sSynth) sSynth->setDetune(cents);
}

- (void)setMasterLevel:(double)level {
    if (self.isReady && sSynth) sSynth->setMasterLevel(level);
}

// ── Transport ──

- (void)startTransport {
    if (self.isReady && sTransport) sTransport->start();
}

- (void)stopTransport {
    if (self.isReady && sTransport) sTransport->stop();
}

- (void)setTempo:(double)bpm {
    if (self.isReady && sTransport) sTransport->setTempo(bpm);
}

- (void)setTransportPosition:(double)beat {
    if (self.isReady && sTransport) sTransport->setPosition(beat);
}

- (double)getTransportPosition {
    return (self.isReady && sTransport) ? sTransport->getPosition() : 0.0;
}

- (double)getTempo {
    return (self.isReady && sTransport) ? sTransport->getTempo() : 120.0;
}

- (BOOL)isTransportPlaying {
    return (self.isReady && sTransport) ? sTransport->isPlaying() : NO;
}

// ── Beat events ──

- (void)setBeatCallback:(BeatCallback)callback {
    self.beatCallback = callback;
}

- (void)startBeatTimer {
    if (self.beatTimer) return;
    if (!sSynth) return;

    self.beatTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
    dispatch_source_set_timer(self.beatTimer, DISPATCH_TIME_NOW, 16 * NSEC_PER_MSEC, 1 * NSEC_PER_MSEC);
    __weak BeeperAudioEngine *weakSelf = self;
    dispatch_source_set_event_handler(self.beatTimer, ^{
        BeeperAudioEngine *strongSelf = weakSelf;
        if (!strongSelf || !sSynth) return;
        BeatCallback cb = strongSelf.beatCallback;
        if (sSynth->beatState.crossed.exchange(false) && cb) {
            cb(sSynth->beatState.beat.load(), sSynth->beatState.bpm.load());
        }
    });
    dispatch_resume(self.beatTimer);
}

- (void)stopBeatTimer {
    if (self.beatTimer) {
        dispatch_source_cancel(self.beatTimer);
        self.beatTimer = nil;
    }
    self.beatCallback = nil;
}

- (void)dealloc {
    [self shutdown];
}

@end
```

- [ ] **Step 3: Commit**

```bash
git add ios/BeeperAudioEngine.h ios/BeeperAudioEngine.mm
git commit -m "feat: add BeeperAudioEngine — JUCE AudioDeviceManager + Graph coordinator"
```

---

## Task 10: Update Bridge Layer

**Files:**
- Modify: `ios/ExpoJuceBridge.h`
- Modify: `ios/ExpoJuceBridge.m`

- [ ] **Step 1: Rewrite ExpoJuceBridge.h**

Replace the entire contents of `ios/ExpoJuceBridge.h`:

```objc
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

typedef void(^ExpoJuceBeatBlock)(double beat, double bpm);

@interface ExpoJuceBridge : NSObject

+ (void)setup;
+ (void)shutdown;
+ (void)startBeatTimer;
+ (void)stopBeatTimer;

// ── Synth (MIDI-style) ───────────────────────────────────────────
+ (void)noteOn:(int)note velocity:(int)velocity;
+ (void)noteOff:(int)note;
+ (void)allNotesOff;

// ── Global Synth Params ──────────────────────────────────────────
+ (void)setWaveform:(NSString *)waveform;
+ (void)setFilterCutoff:(double)hz;
+ (void)setFilterResonance:(double)q;
+ (void)setAttack:(double)ms;
+ (void)setDecay:(double)ms;
+ (void)setSustain:(double)level;
+ (void)setRelease:(double)ms;
+ (void)setDetune:(double)cents;
+ (void)setMasterLevel:(double)level;

// ── Transport ────────────────────────────────────────────────────
+ (void)startTransport;
+ (void)stopTransport;
+ (void)setTempo:(double)bpm;
+ (void)setTransportPosition:(double)beat;
+ (double)getTransportPosition;
+ (double)getTempo;
+ (BOOL)isTransportPlaying;
+ (void)setBeatCallback:(ExpoJuceBeatBlock)callback;

@end

NS_ASSUME_NONNULL_END
```

- [ ] **Step 2: Rewrite ExpoJuceBridge.m**

Replace the entire contents of `ios/ExpoJuceBridge.m`:

```objc
#import "ExpoJuceBridge.h"
#import "BeeperAudioEngine.h"

@implementation ExpoJuceBridge

+ (void)setup { [[BeeperAudioEngine sharedInstance] setup]; }
+ (void)shutdown { [[BeeperAudioEngine sharedInstance] shutdown]; }
+ (void)startBeatTimer { [[BeeperAudioEngine sharedInstance] startBeatTimer]; }
+ (void)stopBeatTimer { [[BeeperAudioEngine sharedInstance] stopBeatTimer]; }

// ── Synth ──
+ (void)noteOn:(int)note velocity:(int)velocity {
    [[BeeperAudioEngine sharedInstance] noteOn:note velocity:velocity];
}
+ (void)noteOff:(int)note {
    [[BeeperAudioEngine sharedInstance] noteOff:note];
}
+ (void)allNotesOff { [[BeeperAudioEngine sharedInstance] allNotesOff]; }

// ── Global Params ──
+ (void)setWaveform:(NSString *)waveform { [[BeeperAudioEngine sharedInstance] setWaveform:waveform]; }
+ (void)setFilterCutoff:(double)hz { [[BeeperAudioEngine sharedInstance] setFilterCutoff:hz]; }
+ (void)setFilterResonance:(double)q { [[BeeperAudioEngine sharedInstance] setFilterResonance:q]; }
+ (void)setAttack:(double)ms { [[BeeperAudioEngine sharedInstance] setAttack:ms]; }
+ (void)setDecay:(double)ms { [[BeeperAudioEngine sharedInstance] setDecay:ms]; }
+ (void)setSustain:(double)level { [[BeeperAudioEngine sharedInstance] setSustain:level]; }
+ (void)setRelease:(double)ms { [[BeeperAudioEngine sharedInstance] setRelease:ms]; }
+ (void)setDetune:(double)cents { [[BeeperAudioEngine sharedInstance] setDetune:cents]; }
+ (void)setMasterLevel:(double)level { [[BeeperAudioEngine sharedInstance] setMasterLevel:level]; }

// ── Transport ──
+ (void)startTransport { [[BeeperAudioEngine sharedInstance] startTransport]; }
+ (void)stopTransport { [[BeeperAudioEngine sharedInstance] stopTransport]; }
+ (void)setTempo:(double)bpm { [[BeeperAudioEngine sharedInstance] setTempo:bpm]; }
+ (void)setTransportPosition:(double)beat { [[BeeperAudioEngine sharedInstance] setTransportPosition:beat]; }
+ (double)getTransportPosition { return [[BeeperAudioEngine sharedInstance] getTransportPosition]; }
+ (double)getTempo { return [[BeeperAudioEngine sharedInstance] getTempo]; }
+ (BOOL)isTransportPlaying { return [[BeeperAudioEngine sharedInstance] isTransportPlaying]; }
+ (void)setBeatCallback:(ExpoJuceBeatBlock)callback {
    [[BeeperAudioEngine sharedInstance] setBeatCallback:callback];
}

@end
```

- [ ] **Step 3: Commit**

```bash
git add ios/ExpoJuceBridge.h ios/ExpoJuceBridge.m
git commit -m "refactor: update bridge to forward to BeeperAudioEngine"
```

---

## Task 11: Update ExpoJuceModule.swift

**Files:**
- Modify: `ios/ExpoJuceModule.swift`

- [ ] **Step 1: Rewrite ExpoJuceModule.swift with new API**

Replace the entire contents of `ios/ExpoJuceModule.swift`:

```swift
import ExpoModulesCore

public class ExpoJuceModule: Module {
  private var hasListeners = false

  public func definition() -> ModuleDefinition {
    Name("ExpoJuce")

    OnDestroy {
      ExpoJuceBridge.stopBeatTimer()
      ExpoJuceBridge.shutdown()
    }

    Constants([
      "PI": Double.pi
    ])

    Events("onChange", "onBeat")

    Function("hello") {
      return "Hello world!"
    }

    OnStartObserving {
      self.hasListeners = true
      ExpoJuceBridge.startBeatTimer()
    }

    OnStopObserving {
      self.hasListeners = false
      ExpoJuceBridge.stopBeatTimer()
    }

    // ── Lifecycle ──────────────────────────────────────────────────

    AsyncFunction("setup") { [weak self] in
      ExpoJuceBridge.setup()
      ExpoJuceBridge.setBeatCallback { [weak self] beat, bpm in
        guard let self = self, self.hasListeners else { return }
        self.sendEvent("onBeat", [
          "beat": beat,
          "bpm": bpm,
        ])
      }
    }.runOnQueue(.main)

    AsyncFunction("shutdown") {
      ExpoJuceBridge.shutdown()
    }.runOnQueue(.main)

    // ── Synth (MIDI-style) ─────────────────────────────────────────

    AsyncFunction("noteOn") { (note: Int, velocity: Int) in
      ExpoJuceBridge.noteOn(Int32(note), velocity: Int32(velocity))
    }.runOnQueue(.main)

    AsyncFunction("noteOff") { (note: Int) in
      ExpoJuceBridge.noteOff(Int32(note))
    }.runOnQueue(.main)

    AsyncFunction("allNotesOff") {
      ExpoJuceBridge.allNotesOff()
    }.runOnQueue(.main)

    // ── Global Synth Params ────────────────────────────────────────

    AsyncFunction("setWaveform") { (waveform: String) in
      ExpoJuceBridge.setWaveform(waveform)
    }.runOnQueue(.main)

    AsyncFunction("setFilterCutoff") { (hz: Double) in
      ExpoJuceBridge.setFilterCutoff(hz)
    }.runOnQueue(.main)

    AsyncFunction("setFilterResonance") { (q: Double) in
      ExpoJuceBridge.setFilterResonance(q)
    }.runOnQueue(.main)

    AsyncFunction("setAttack") { (ms: Double) in
      ExpoJuceBridge.setAttack(ms)
    }.runOnQueue(.main)

    AsyncFunction("setDecay") { (ms: Double) in
      ExpoJuceBridge.setDecay(ms)
    }.runOnQueue(.main)

    AsyncFunction("setSustain") { (level: Double) in
      ExpoJuceBridge.setSustain(level)
    }.runOnQueue(.main)

    AsyncFunction("setRelease") { (ms: Double) in
      ExpoJuceBridge.setRelease(ms)
    }.runOnQueue(.main)

    AsyncFunction("setDetune") { (cents: Double) in
      ExpoJuceBridge.setDetune(cents)
    }.runOnQueue(.main)

    AsyncFunction("setMasterLevel") { (level: Double) in
      ExpoJuceBridge.setMasterLevel(level)
    }.runOnQueue(.main)

    // ── Transport ──────────────────────────────────────────────────

    AsyncFunction("startTransport") {
      ExpoJuceBridge.startTransport()
    }.runOnQueue(.main)

    AsyncFunction("stopTransport") {
      ExpoJuceBridge.stopTransport()
    }.runOnQueue(.main)

    AsyncFunction("setTempo") { (bpm: Double) in
      ExpoJuceBridge.setTempo(bpm)
    }.runOnQueue(.main)

    AsyncFunction("setTransportPosition") { (beat: Double) in
      ExpoJuceBridge.setTransportPosition(beat)
    }.runOnQueue(.main)

    AsyncFunction("getTransportPosition") { () -> Double in
      return ExpoJuceBridge.getTransportPosition()
    }.runOnQueue(.main)

    AsyncFunction("getTempo") { () -> Double in
      return ExpoJuceBridge.getTempo()
    }.runOnQueue(.main)

    AsyncFunction("isTransportPlaying") { () -> Bool in
      return ExpoJuceBridge.isTransportPlaying()
    }.runOnQueue(.main)

    // ── Async / View ───────────────────────────────────────────────

    AsyncFunction("setValueAsync") { (value: String) in
      self.sendEvent("onChange", [
        "value": value
      ])
    }

    View(ExpoJuceView.self) {
      Prop("name") { (view: ExpoJuceView, prop: String) in
        print(prop)
      }
    }
  }
}
```

- [ ] **Step 2: Commit**

```bash
git add ios/ExpoJuceModule.swift
git commit -m "feat: update Expo module with MIDI-style poly API"
```

---

## Task 12: Update TypeScript API

**Files:**
- Modify: `src/index.ts`

- [ ] **Step 1: Rewrite src/index.ts with new API**

Replace the entire contents of `src/index.ts`:

```typescript
import { NativeModulesProxy, EventEmitter, Subscription } from 'expo-modules-core';

import ExpoJuceModule from './ExpoJuceModule';
import ExpoJuceView from './ExpoJuceView';
import { ChangeEventPayload, BeatEventPayload, ExpoJuceViewProps } from './ExpoJuce.types';

// ── Lifecycle ────────────────────────────────────────────────────

export async function setup(): Promise<void> {
  return ExpoJuceModule.setup();
}

export async function shutdown(): Promise<void> {
  return ExpoJuceModule.shutdown();
}

// ── Polyphonic Synth (MIDI-style) ────────────────────────────────

export async function noteOn(note: number, velocity: number): Promise<void> {
  return ExpoJuceModule.noteOn(note, velocity);
}

export async function noteOff(note: number): Promise<void> {
  return ExpoJuceModule.noteOff(note);
}

export async function allNotesOff(): Promise<void> {
  return ExpoJuceModule.allNotesOff();
}

// ── Global Synth Params ──────────────────────────────────────────

export async function setWaveform(waveform: 'sine' | 'square' | 'saw' | 'triangle'): Promise<void> {
  return ExpoJuceModule.setWaveform(waveform);
}

export async function setFilterCutoff(hz: number): Promise<void> {
  return ExpoJuceModule.setFilterCutoff(hz);
}

export async function setFilterResonance(q: number): Promise<void> {
  return ExpoJuceModule.setFilterResonance(q);
}

export async function setAttack(ms: number): Promise<void> {
  return ExpoJuceModule.setAttack(ms);
}

export async function setDecay(ms: number): Promise<void> {
  return ExpoJuceModule.setDecay(ms);
}

export async function setSustain(level: number): Promise<void> {
  return ExpoJuceModule.setSustain(level);
}

export async function setRelease(ms: number): Promise<void> {
  return ExpoJuceModule.setRelease(ms);
}

export async function setDetune(cents: number): Promise<void> {
  return ExpoJuceModule.setDetune(cents);
}

export async function setMasterLevel(level: number): Promise<void> {
  return ExpoJuceModule.setMasterLevel(level);
}

// ── Transport ────────────────────────────────────────────────────

export async function startTransport(): Promise<void> {
  return ExpoJuceModule.startTransport();
}

export async function stopTransport(): Promise<void> {
  return ExpoJuceModule.stopTransport();
}

export async function setTempo(bpm: number): Promise<void> {
  return ExpoJuceModule.setTempo(bpm);
}

export async function setTransportPosition(beat: number): Promise<void> {
  return ExpoJuceModule.setTransportPosition(beat);
}

export async function getTransportPosition(): Promise<number> {
  return ExpoJuceModule.getTransportPosition();
}

export async function getTempo(): Promise<number> {
  return ExpoJuceModule.getTempo();
}

export async function isTransportPlaying(): Promise<boolean> {
  return ExpoJuceModule.isTransportPlaying();
}

// ── Events ───────────────────────────────────────────────────────

export async function setValueAsync(value: string) {
  return await ExpoJuceModule.setValueAsync(value);
}

const emitter = new EventEmitter(ExpoJuceModule ?? NativeModulesProxy.ExpoJuce);

export function addChangeListener(listener: (event: ChangeEventPayload) => void): Subscription {
  return emitter.addListener<ChangeEventPayload>('onChange', listener);
}

export function addBeatListener(listener: (event: BeatEventPayload) => void): Subscription {
  return emitter.addListener<BeatEventPayload>('onBeat', listener);
}

export { ExpoJuceView, ExpoJuceViewProps, ChangeEventPayload, BeatEventPayload };
```

- [ ] **Step 2: Build TypeScript**

```bash
cd /Users/smokey/Internal/Beeper/code/expo-juce
npm run build
```

Expected: TypeScript compiles without errors.

- [ ] **Step 3: Commit**

```bash
git add src/index.ts
git commit -m "feat: update TypeScript API — MIDI-style poly synth"
```

---

## Task 13: Delete Old Files

**Files:**
- Delete: `ios/JuceToneGenerator.h`
- Delete: `ios/JuceToneGenerator.mm`

- [ ] **Step 1: Delete old mono synth files**

```bash
cd /Users/smokey/Internal/Beeper/code/expo-juce
git rm ios/JuceToneGenerator.h ios/JuceToneGenerator.mm
```

- [ ] **Step 2: Verify build still compiles**

```bash
cd example
npx pod-install
cd ios
xcodebuild -workspace expojuceexample.xcworkspace \
  -scheme expojuceexample \
  -configuration Debug \
  -sdk iphonesimulator \
  -destination 'platform=iOS Simulator,name=iPhone 15' \
  build 2>&1 | tail -5
```

- [ ] **Step 3: Commit**

```bash
git add -A
git commit -m "refactor: remove JuceToneGenerator (replaced by BeeperAudioEngine)"
```

---

## Task 14: Full Build Verification

**Files:** None (verification only)

- [ ] **Step 1: Clean build from scratch**

```bash
cd /Users/smokey/Internal/Beeper/code/expo-juce
node src/scripts/vendor-juce.cjs
npm run build
cd example
rm -rf ios/Pods ios/build
npx pod-install
```

- [ ] **Step 2: Build for simulator**

```bash
cd ios
xcodebuild -workspace expojuceexample.xcworkspace \
  -scheme expojuceexample \
  -configuration Debug \
  -sdk iphonesimulator \
  -destination 'platform=iOS Simulator,name=iPhone 15' \
  build 2>&1 | tail -10
```

Expected: **BUILD SUCCEEDED**

- [ ] **Step 3: Fix any remaining build issues and commit**

If there are linker errors, framework issues, or header search path problems, fix them in the relevant files (most likely `ExpoJuce.podspec` or `JuceConfig.h`).

```bash
git add -A
git commit -m "fix: resolve final build issues"
```

---

## Task 15: On-Device Audio Verification

**Files:** None (manual testing)

- [ ] **Step 1: Build and run on physical iOS device**

Use either Xcode or EAS:

```bash
cd /Users/smokey/Internal/Beeper/code/expo-juce/example
npx expo run:ios --device
```

Or via EAS:
```bash
eas build -p ios --profile dev
```

- [ ] **Step 2: Test basic audio output**

In the example app (or via JS console), run:

```javascript
import * as ExpoJuce from 'expo-juce';
await ExpoJuce.setup();
await ExpoJuce.noteOn(60, 100);  // Middle C
// Should hear a tone
setTimeout(() => ExpoJuce.noteOff(60), 1000);
```

- [ ] **Step 3: Test polyphony — play a chord**

```javascript
await ExpoJuce.noteOn(60, 100);  // C
await ExpoJuce.noteOn(64, 100);  // E
await ExpoJuce.noteOn(67, 100);  // G
// Should hear a C major chord
setTimeout(() => ExpoJuce.allNotesOff(), 2000);
```

- [ ] **Step 4: Test voice stealing**

```javascript
// Play 9 notes rapidly — 9th should steal the quietest of the first 8
for (let i = 60; i <= 68; i++) {
  await ExpoJuce.noteOn(i, 100);
}
// Should hear 8 notes, no clicks or pops
```

- [ ] **Step 5: Test parameter changes**

```javascript
await ExpoJuce.setWaveform('saw');
await ExpoJuce.setFilterCutoff(2000);
await ExpoJuce.setFilterResonance(0.5);
await ExpoJuce.noteOn(48, 127);
// Should hear a filtered saw wave
```

- [ ] **Step 6: Commit verification notes**

```bash
git commit --allow-empty -m "verify: on-device audio testing complete — polyphony working"
```
