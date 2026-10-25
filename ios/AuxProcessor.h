#pragma once

// Pure C++ (no Objective-C): compiled into the iOS module and by the offline harness.
//
// Aux source: metronome + backing track, its own graph node summed into the output after the
// effects chain (the effects never process it). Stereo out, no input.

#include "JuceConfig.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include "TransportEngine.h"
#include "BackingTrack.h"
#include <atomic>
#include <cstdint>
#include <vector>

enum MetronomeStyle : int {
    kMetronomeClassic = 0, kMetronomeWood, kMetronomeBeep, kMetronomeCowbell, kMetronomeHat,
    kMetronomeStyleCount
};
inline constexpr const char* kMetronomeStyleNames[kMetronomeStyleCount] = { "classic", "wood", "beep", "cowbell", "hat" };
int metronomeStyleIndex(const char* name); // -1 if unknown

// Plain copy of the aux settings (after clamping).
struct AuxSettings {
    bool metronomeEnabled;
    int metronomeStyle;
    float metronomeLevel;
    bool metronomeAccent;
    int beatsPerBar;
    float trackLevel;
    bool trackLoop;
    bool trackEnabled;
    static AuxSettings defaults();
    bool operator==(const AuxSettings& o) const;
};

// Metronome + backing-track options: atomics written by any thread, read by the audio thread
// once per block. One per process in the engine (like FxParamStore), so options set before
// setup or during a rebuild apply to the next AuxProcessor.
class AuxSettingsStore {
public:
    AuxSettingsStore() { apply(AuxSettings::defaults()); }

    void setMetronomeEnabled(bool on) { metronomeEnabled.store(on, std::memory_order_relaxed); }
    bool setMetronomeStyle(const char* name); // false if unknown
    void setMetronomeStyle(int style);
    void setMetronomeLevel(double level);       // 0–1
    void setMetronomeAccent(bool on) { metronomeAccent.store(on, std::memory_order_relaxed); }
    void setBeatsPerBar(double beats);          // 1–16, rounded
    void setTrackLevel(double level);           // 0–1
    void setTrackLoop(bool on) { trackLoop.store(on, std::memory_order_relaxed); }
    void setTrackEnabled(bool on) { trackEnabled.store(on, std::memory_order_relaxed); }

    AuxSettings snapshot() const;
    void apply(const AuxSettings& s);

private:
    friend class AuxProcessor;
    std::atomic<bool> metronomeEnabled{false};
    std::atomic<int> metronomeStyle{0};
    std::atomic<float> metronomeLevel{0.8f};
    std::atomic<bool> metronomeAccent{true};
    std::atomic<int> beatsPerBar{4};
    std::atomic<float> trackLevel{0.8f};
    std::atomic<bool> trackLoop{true};
    std::atomic<bool> trackEnabled{true};
};

struct AuxDiagnostics {
    std::atomic<uint64_t> processCount{0};
    std::atomic<uint64_t> clicks{0};          // metronome clicks started
    std::atomic<double> trackPositionSec{-1.0}; // backing-track read position (-1 = not playing)
};

// Metronome: clicks start on the first sample whose transport beat position is >= each integer
// beat, computed from the block-start beat / tempo TransportEngine::advanceBlock records (the
// synth node runs first in the same graph pass). Clicks are pre-rendered per style (normal +
// accent) in prepareToPlay; accent on beat 1 of each bar (beatsPerBar).
//
// Backing track: plays while the transport plays. Position = (transport beat − anchor) beats at
// the current tempo, where the anchor is the first beat of the bar the transport started on;
// seeks (setTransportPosition) therefore seek the track. Loops if `loop`, else silent past its
// end. A start away from the track's start, a seek or a stop is declicked with a 5 ms fade.
// A published buffer whose rate is not this node's rate is not played (it is being resampled).
class AuxProcessor : public juce::AudioProcessor {
public:
    static constexpr double kMaxClickSeconds = 0.08;
    static constexpr double kFadeSeconds = 0.005;
    static constexpr double kLevelRampSeconds = 0.030;

    AuxProcessor();
    ~AuxProcessor() override;

    // Process-lifetime settings / track holder (nullptr = own). Set before the node renders.
    void setSettings(AuxSettingsStore* s) { settings = s ? s : &ownSettings; }
    AuxSettingsStore& auxSettings() { return *settings; }
    void setBackingTrack(BackingTrack* t);
    void setTransport(TransportEngine* t) { transport = t; }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    const juce::String getName() const override { return "Aux"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}

    // Pre-rendered click (tests compare the output with it). Valid after prepareToPlay.
    const std::vector<float>& clickBuffer(int style, bool accent) const { return clicks[style][accent ? 1 : 0]; }

    AuxDiagnostics diag;

private:
    using Smoothed = juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>;

    void renderClicks();
    void renderMetronome(float* left, float* right, int n, double beatStart, double beatsPerSample, bool playing);
    void renderTrack(float* left, float* right, int n, double beatStart, double bpm, bool playing);

    AuxSettingsStore ownSettings;
    AuxSettingsStore* settings = &ownSettings;
    BackingTrack ownTrack;
    BackingTrack* track = &ownTrack;
    TransportEngine* transport = nullptr;

    double sr = 48000.0;
    bool prepared = false;

    // metronome
    std::vector<float> clicks[kMetronomeStyleCount][2];
    const float* clickData = nullptr;
    int clickLength = 0, clickPos = 0;
    int64_t lastClickedBeat = 0; // last integer beat clicked (or skipped)
    Smoothed metronomeGain;

    // backing track
    bool wasPlaying = false;
    double anchorBeat = 0.0;
    int64_t nextTrackPos = 0;   // where playback continues if nothing jumps
    bool trackActive = false;   // main voice playing
    int64_t mainPos = 0;
    int mainFadeLeft = 0;       // fade-in samples left
    bool tailActive = false;    // old position fading out after a seek / stop
    int64_t tailPos = 0;
    int tailLeft = 0;
    Smoothed trackGain;
    BackingTrackBuffer* lastTrack = nullptr;

    JUCE_DECLARE_NON_COPYABLE(AuxProcessor)
};
