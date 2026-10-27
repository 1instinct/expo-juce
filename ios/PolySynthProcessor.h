#pragma once

#include "JuceConfig.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include "Voice.h"
#include "VoiceAllocator.h"
#include "TransportEngine.h"
#include "NoteEventQueue.h"
#include "Arpeggiator.h"
#include "AnalysisRing.h"
#include <atomic>
#include <cmath>
#include <cstdint>

struct BeatState {
    std::atomic<bool> crossed{false};
    std::atomic<double> beat{0.0};
    std::atomic<double> bpm{120.0};
};

// Diagnostics counters. Written on the audio thread, read from any thread (relaxed loads
// only). The note counters live in the NoteEventQueue.
struct SynthDiagnostics {
    std::atomic<uint64_t> processCount{0};
    std::atomic<float> peak{0.0f};                // decaying hold of block max |x|
    std::atomic<int> activeVoices{0};
    std::atomic<double> lastNoteToRenderMs{-1.0}; // -1 = no NoteOn consumed yet
    std::atomic<double> lastTouchToRenderMs{-1.0}; // -1 = no touch-stamped NoteOn consumed yet
};

// Per-block decay factor for peak holds: halves roughly every 300 ms
// regardless of block size. Real-time safe (no allocation, no locks).
inline float diagPeakDecay(int numSamples, double sampleRate) {
    if (sampleRate <= 0.0) return 0.0f;
    return (float)std::exp(-(double)numSamples / (0.43 * sampleRate));
}

// Every user-settable synth parameter, as stored (after clamping). getSettings / applySettings
// carry the configuration across an engine rebuild, which replaces the processor.
struct SynthSettings {
    int waveform;
    float filterCutoff, filterResonance;
    float attackMs, decayMs, sustainLevel, releaseMs;
    float detuneCents, masterLevel;
    ArpSettings arp;
};

class PolySynthProcessor : public juce::AudioProcessor {
public:
    PolySynthProcessor();
    ~PolySynthProcessor() override = default;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    const juce::String getName() const override { return "PolySynth"; }
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

    // Thread-safe, any number of threads: forwarded to the note queue (see NoteEventQueue).
    // touchTicks: originating UITouch timestamp in juce::Time high-resolution ticks
    // (mach_absolute_time on Apple), or 0 when the note did not come from a touch.
    void noteOn(int note, int velocity, juce::int64 touchTicks = 0) { queue->pushNoteOn(note, velocity, touchTicks); }
    void noteOff(int note) { queue->pushNoteOff(note); }
    void allNotesOff() { queue->pushAllNotesOff(); }

    // The queue this synth reads its notes from: its own by default; the engine passes its
    // process-lifetime queue (before the synth is attached to a device). nullptr = own queue.
    void setNoteQueue(NoteEventQueue* q) { queue = q ? q : &ownQueue; }
    NoteEventQueue& noteQueue() { return *queue; }

    void setWaveform(Waveform wf);
    void setFilterCutoff(double hz);
    void setFilterResonance(double q);
    void setAttack(double ms);
    void setDecay(double ms);
    void setSustain(double level);
    void setRelease(double ms);
    void setDetune(double cents);
    void setMasterLevel(double level);

    // Any thread (atomic loads / stores, same as the setters). Includes the arpeggiator's options.
    SynthSettings getSettings() const;
    void applySettings(const SynthSettings& s);

    // The arpeggiator between the note queue and the voices (options any thread, see
    // Arpeggiator::setSettings). Enabled, drained note-ons feed it and it starts / stops voices
    // at sample offsets inside the block; disabled, note-ons start voices directly as before.
    Arpeggiator& arp() { return arpeggiator; }
    const Arpeggiator& arp() const { return arpeggiator; }

    void setTransport(TransportEngine* t) { transport = t; }
    // Receives the arpeggiator state every block (AnalysisRing::publishArp). nullptr = none.
    void setAnalysis(AnalysisRing* a) { analysis = a; }
    BeatState beatState;
    SynthDiagnostics diag;

private:
    static constexpr int kArpEventCapacity = Arpeggiator::kMaxEvents;

    void startQueuedNote(const NoteEvent& event);
    void recordNoteLatency(const NoteEvent& event);
    void renderVoices(float* mono, int numSamples, Waveform wf, float detune, float cutoff, float reso, float master);
    void applyArpEvent(const ArpEvent& event);

    Voice voices[MAX_VOICES];
    VoiceAllocator allocator;
    TransportEngine* transport = nullptr;
    AnalysisRing* analysis = nullptr;
    Arpeggiator arpeggiator;
    ArpEvent arpEvents[kArpEventCapacity]; // this block's arp events (audio thread)
    int arpEventCount = 0;
    uint64_t lastAllReleasedSeen = 0;      // NoteEventQueue::allReleasedMark last applied to the arp

    std::atomic<int> waveform{static_cast<int>(Waveform::Sine)};
    std::atomic<float> filterCutoff{20000.0f};
    std::atomic<float> filterResonance{0.0f};
    std::atomic<float> detuneCents{0.0f};
    std::atomic<float> masterLevel{0.5f};

    NoteEventQueue ownQueue;
    NoteEventQueue* queue = &ownQueue;
};
