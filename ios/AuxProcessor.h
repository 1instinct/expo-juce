#pragma once

// Pure C++ (no Objective-C): compiled into the iOS module and by the offline harness.
//
// Aux source: metronome + backing track, its own graph node summed into the output after the
// effects chain (the effects never process it). Stereo out, no input.

#include "JuceConfig.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include "TransportEngine.h"
#include "BackingTrack.h"
#include "AnalysisRing.h"
#include <atomic>
#include <cstdint>
#include <memory>
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
    std::atomic<double> trackTempoRatio{1.0};   // transport tempo / track tempo; 1 = the file untouched
};

// Metronome: clicks start on the first sample whose transport beat position is >= each integer
// beat, computed from the block-start beat / tempo TransportEngine::advanceBlock records (the
// synth node runs first in the same graph pass). Clicks are pre-rendered per style (normal +
// accent) in prepareToPlay; accent on beat 1 of each bar (beatsPerBar).
//
// Backing track: plays while the transport plays; its sample position advances with the
// samples rendered, so a tempo change does not move it. When the transport starts or is moved
// (setTransportPosition, detected from the transport's seek flag or a block-start beat that is
// not where the previous block ended) the position is set from the beat: track sample 0 is beat
// 0, position = beat × 60 / bpm × rate (modulo the length when looping), where bpm is the track's
// own tempo if it has one and the transport's otherwise. Pausing and resuming
// therefore continues where it stopped, and seeks land on the beat grid. Loops if `loop`, else
// silent past its end. A start away from sample 0, a seek, a stop, and a swap to another buffer
// (load, unload, resample) are declicked with a 5 ms fade (the old buffer fades out while the
// new one fades in; the holder keeps the old one alive for that). A published buffer whose rate
// is not this node's rate is not played (it is being reloaded).
//
// Following the tempo: a buffer with a sourceBpm keeps its beats on the transport's at any tempo,
// at its own pitch (WSOLA over the resident buffer, nothing is buffered or allocated). Two read
// heads, each at normal speed, crossfade every kGrainHopSeconds: the outgoing head is the previous
// grain, the incoming one starts where the beat says the track should be by the middle of its
// grain, moved up to kGrainSearchSeconds to the place that best continues what the outgoing head is
// playing. The target comes from the transport's beat each time, so errors never add up and a tempo
// change or a seek holds from the next grain. A click sits at most the search distance plus
// |ratio - 1| × hop from its beat. At the track's own tempo there are no grains: after one last
// crossfade onto the exact grid position it is the plain path above, the file sample for sample.
//
// Prepare: JUCE never runs prepareToPlay concurrently with processBlock (the graph renders
// nothing while its nodes are prepared, see AudioProcessorGraph NodeStates::applySettings; the
// iOS device calls audioDeviceAboutToStart under its callback lock or before the callback is
// installed). Defensively, the clicks live in fixed storage sized for 192 kHz from the
// constructor, so a prepare can only rewrite their contents, never move them.
class AuxProcessor : public juce::AudioProcessor {
public:
    static constexpr double kMaxClickSeconds = 0.08;
    static constexpr double kFadeSeconds = 0.005;
    static constexpr double kLevelRampSeconds = 0.030;
    static constexpr int kMaxClickSamples = (int)(kMaxClickSeconds * 192000.0) + 1;
    static constexpr double kGrainHopSeconds = 0.020;
    static constexpr double kGrainSearchSeconds = 0.010;
    static constexpr double kGrainCompareSeconds = 0.010;

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

    // Copy of a pre-rendered click (tests compare the output with it). Valid after prepareToPlay.
    std::vector<float> clickBuffer(int style, bool accent) const;
    // Transport state as of the last rendered block, for getAnalysis (nullptr = none).
    void setAnalysis(AnalysisRing* a) { analysis = a; }

    AuxDiagnostics diag;

private:
    using Smoothed = juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>;

    void renderClicks();
    void renderMetronome(float* left, float* right, int n, double beatStart, double beatsPerSample, bool playing);
    void renderTrack(float* left, float* right, int n, double beatStart, double bpm, bool playing, bool seeked);
    void startTail(BackingTrackBuffer* buffer, int64_t position);
    void startGrain(const BackingTrackBuffer& buffer, double sourcePosition, bool search, bool loop);

    AuxSettingsStore ownSettings;
    AuxSettingsStore* settings = &ownSettings;
    BackingTrack ownTrack;
    BackingTrack* track = &ownTrack;
    TransportEngine* transport = nullptr;
    AnalysisRing* analysis = nullptr;

    double sr = 48000.0;
    bool prepared = false;

    // metronome
    std::unique_ptr<float[]> clickStore; // [style][accent][kMaxClickSamples], allocated once
    std::atomic<int> clickLengths[kMetronomeStyleCount][2] = {};
    float* clickSlot(int style, int accent) const { return clickStore.get() + ((size_t)style * 2 + (size_t)accent) * kMaxClickSamples; }
    const float* clickData = nullptr;
    int clickLength = 0, clickPos = 0;
    int64_t lastClickedBeat = 0; // last integer beat clicked (or skipped)
    Smoothed metronomeGain;

    // backing track
    bool wasPlaying = false;
    bool havePrevBlock = false;       // prevBeatStart / prevBeatsAdvanced valid
    double prevBeatStart = 0.0, prevBeatsAdvanced = 0.0;
    bool trackActive = false;         // main voice playing
    int64_t trackPos = 0;             // main voice position (samples at sr, not wrapped); with grains, the incoming head
    bool grainsActive = false;        // following a tempo that is not the track's
    bool grainsOnGrid = false;        // back at the track's tempo: the incoming head is the exact position
    int64_t grainOutPos = 0;          // the outgoing head
    int grainHop = 960, grainPhase = 0; // samples per crossfade, samples into this one
    int grainSearch = 480, grainCompare = 480, grainStride = 4;
    int mainFadeLeft = 0;             // fade-in samples left
    bool tailActive = false;          // a voice fading out (seek / stop / swap)
    BackingTrackBuffer* tailBuffer = nullptr;
    int64_t tailPos = 0;
    int tailLeft = 0;
    bool tailGrains = false;          // the tail was mid-crossfade: it finishes that one while it fades
    int64_t tailOutPos = 0;
    int tailPhase = 0;
    bool tailProtected = false;       // the holder keeps a previous buffer for us
    Smoothed trackGain;
    BackingTrackBuffer* lastTrack = nullptr;

    JUCE_DECLARE_NON_COPYABLE(AuxProcessor)
};
