#pragma once

#include "JuceConfig.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include "Voice.h"
#include "VoiceAllocator.h"
#include "TransportEngine.h"
#include <atomic>
#include <cmath>
#include <cstdint>

struct BeatState {
    std::atomic<bool> crossed{false};
    std::atomic<double> beat{0.0};
    std::atomic<double> bpm{120.0};
};

// Diagnostics counters. Written on the audio thread (except notesQueued and
// notesDropped, which the noteOn producers write), read from any thread. Relaxed
// loads only; notesQueued is incremented before the release-store that publishes
// the event, so a reader never sees notesConsumed > notesQueued. notesQueued counts
// accepted NoteOns only: attempted NoteOns == notesQueued + notesDropped.
struct SynthDiagnostics {
    std::atomic<uint64_t> processCount{0};
    std::atomic<float> peak{0.0f};                // decaying hold of block max |x|
    std::atomic<uint32_t> notesQueued{0};
    std::atomic<uint32_t> notesConsumed{0};
    std::atomic<uint32_t> notesDropped{0};        // noteOn rejected: event queue full
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

// Note event queue: multi-producer (main thread + JS thread, serialised by a spinlock),
// single consumer (audio thread, lock-free).
struct NoteEvent {
    enum Type : uint8_t { NoteOn, NoteOff, AllNotesOff };
    Type type;
    int note;
    int velocity;
    juce::int64 stampTicks; // juce::Time::getHighResolutionTicks() at enqueue (0 if unused)
    juce::int64 touchTicks; // originating UITouch.timestamp in high-res ticks (same clock); 0 if not a touch
};

// Every user-settable synth parameter, as stored (after clamping). getSettings / applySettings
// carry the configuration across an engine rebuild, which replaces the processor.
struct SynthSettings {
    int waveform;
    float filterCutoff, filterResonance;
    float attackMs, decayMs, sustainLevel, releaseMs;
    float detuneCents, masterLevel;
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

    // Thread-safe: callable from any number of threads (producers are serialised by
    // producerLock), queued for the audio thread.
    // touchTicks: originating UITouch timestamp in juce::Time high-resolution ticks
    // (mach_absolute_time on Apple), or 0 when the note did not come from a touch.
    void noteOn(int note, int velocity, juce::int64 touchTicks = 0);
    void noteOff(int note);
    void allNotesOff();

    void setWaveform(Waveform wf);
    void setFilterCutoff(double hz);
    void setFilterResonance(double q);
    void setAttack(double ms);
    void setDecay(double ms);
    void setSustain(double level);
    void setRelease(double ms);
    void setDetune(double cents);
    void setMasterLevel(double level);

    // Any thread (atomic loads / stores, same as the setters).
    SynthSettings getSettings() const;
    void applySettings(const SynthSettings& s);

    void setTransport(TransportEngine* t) { transport = t; }
    BeatState beatState;
    SynthDiagnostics diag;

private:
    void processNoteEvent(const NoteEvent& event);

    Voice voices[MAX_VOICES];
    VoiceAllocator allocator;
    TransportEngine* transport = nullptr;

    std::atomic<int> waveform{static_cast<int>(Waveform::Sine)};
    std::atomic<float> filterCutoff{20000.0f};
    std::atomic<float> filterResonance{0.0f};
    std::atomic<float> detuneCents{0.0f};
    std::atomic<float> masterLevel{0.5f};

    // Ring buffer for note events (producers → audio thread). Producers (noteOn / noteOff /
    // allNotesOff, from the main thread and the JS thread) hold producerLock while they
    // write; the consumer (processBlock) never touches it.
    static constexpr int EVENT_QUEUE_SIZE = 256;
    NoteEvent eventQueue[EVENT_QUEUE_SIZE];
    std::atomic<int> eventWritePos{0};
    std::atomic<int> eventReadPos{0};
    std::atomic_flag producerLock = ATOMIC_FLAG_INIT;
};
