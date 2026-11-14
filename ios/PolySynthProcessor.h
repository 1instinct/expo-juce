#pragma once

#include "JuceConfig.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include "Voice.h"
#include "VoiceAllocator.h"
#include "TransportEngine.h"
#include "NoteEventQueue.h"
#include "Arpeggiator.h"
#include "DrumMachine.h"
#include "SamplerEngine.h"
#include "AnalysisRing.h"
#include "LatencyStats.h"
#include "SequencePlayer.h"
#include <atomic>
#include <cmath>
#include <cstdint>

static_assert(NoteEventQueue::kMaxDrumVoice == kDrumVoices - 1, "drum voice range of the note queue");
static_assert(NoteEventQueue::kMaxSamplerSlot == kSamplerSlots - 1, "sampler slot range of the note queue");

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
    ~PolySynthProcessor() override; // releases the sampler source's reader slot

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
    // entryTicks: the touch handler's entry (touchesBegan), same clock; 0 = unknown.
    void noteOn(int note, int velocity, juce::int64 touchTicks = 0, juce::int64 entryTicks = 0) {
        queue->pushNoteOn(note, velocity, touchTicks, entryTicks);
    }
    void noteOff(int note) { queue->pushNoteOff(note); }
    void allNotesOff() { queue->pushAllNotesOff(); }
    // A drum hit (voice 0–8, velocity 1–127) through the same queue; any thread. Out of range: ignored.
    void drumHit(int voice, int velocity, juce::int64 touchTicks = 0, juce::int64 entryTicks = 0) {
        queue->pushDrumHit(voice, velocity, touchTicks, entryTicks);
    }
    // Sampler (0.10.0+) through the same queue; any thread. Out of range: ignored.
    void samplerHit(int slot, int velocity, juce::int64 touchTicks = 0, juce::int64 entryTicks = 0) {
        queue->pushSamplerHit(slot, velocity, touchTicks, entryTicks);
    }
    void samplerRelease(int slot) { queue->pushSamplerRelease(slot); }
    void samplerNoteOn(int note, int velocity) { queue->pushSamplerNoteOn(note, velocity); }
    void samplerNoteOff(int note) { queue->pushSamplerNoteOff(note); }

    // The queue this synth reads its notes from: its own by default; the engine passes its
    // process-lifetime queue (before the synth is attached to a device). nullptr = own queue. The
    // queue's current all-notes-off mark counts as already applied (it may predate this synth).
    void setNoteQueue(NoteEventQueue* q) {
        queue = q ? q : &ownQueue;
        lastAllReleasedSeen = queue->allReleasedMark();
        lastAllReleasedSeenBySequence = lastAllReleasedSeen;
        lastSamplerSilenceSeen = queue->samplerSilenceMark();
    }
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
    // Arpeggiator::setSettings / setSettingsStore; applySettings writes its store too). Enabled, drained note-ons feed it and it starts / stops voices
    // at sample offsets inside the block; disabled, note-ons start voices directly as before.
    Arpeggiator& arp() { return arpeggiator; }
    const Arpeggiator& arp() const { return arpeggiator; }

    // The drum part (0.8.0+): drained drum hits trigger it (never the arpeggiator), and it is mixed
    // into this processor's output after the synth voices, so it goes through the same effects
    // chain. Its parameters: drums().setParamStore / params() (atomics, any thread). Its level is
    // its own (the synth's master level does not apply), and it is left out of diag.peak.
    DrumMachine& drums() { return drumMachine; }
    const DrumMachine& drums() const { return drumMachine; }

    // The sampler (0.10.0+): drained sampler events start its voices (never the arpeggiator); it
    // takes each slot's buffer from its source at the start of every block and adds its stereo
    // output after the synth and drums (copied to both channels), so it goes through the effects
    // chain too. Parameters: sampler().setParamStore / params() (any thread). Left out of
    // diag.peak. setSamplerSource: before the processor renders (nullptr = none); prepareToPlay
    // passes the device rate to the source and the destructor releases its reader slot.
    SamplerEngine& sampler() { return samplerEngine; }
    const SamplerEngine& sampler() const { return samplerEngine; }
    void setSamplerSource(SamplerBufferSource* source) { samplerEngine.setSource(source); }

    // The loop sequence player (SequencePlayer.h, 0.9.0-rc.5): plays the sequence in the attached
    // store while the transport plays, at sample offsets inside the block. Its synth notes start
    // voices of their own (VoiceSource::Sequence: a player release never stops them, their release
    // never stops a player's note); with the arpeggiator enabled they feed the arpeggiator instead,
    // as player notes do. Its drum hits trigger the drum part at their offsets. The engine attaches
    // its process-lifetime store and note log (no store: the player plays nothing). Its events never
    // reach the sampler (a sequence has note and drum events only; pads in sampler mode hit the
    // sampler directly), and stopping it leaves sampler voices alone.
    void setSequenceStore(seqplay::SequenceStore* s) { sequencer.setStore(s); }
    void setSequenceNoteLog(seqplay::NoteLog* log) { sequenceLog = log; }
    const seqplay::SequencePlayer& sequence() const { return sequencer; }

    void setTransport(TransportEngine* t) { transport = t; }
    // Receives the arpeggiator state every block (AnalysisRing::publishArp). nullptr = none.
    void setAnalysis(AnalysisRing* a) { analysis = a; }
    // Touch-originated notes and hits record their legs here (audio thread: handler, queueToRender,
    // touchToRender). The engine passes its process-lifetime stats. nullptr = none.
    void setTouchLatency(TouchLatencyStats* s) { touchLatency = s; }
    // MIDI input (MidiInput.h): note-ons and hits that carry an input arrival record arrival → the
    // render block that consumed them here (audio thread). nullptr = none.
    void setInputLatency(LatencyWindow* w) { inputLatency = w; }
    BeatState beatState;
    SynthDiagnostics diag;

private:
    static constexpr int kArpEventCapacity = Arpeggiator::kMaxEvents;

    void startQueuedNote(const NoteEvent& event);
    void startDrumHit(const NoteEvent& event);
    void startSamplerEvent(const NoteEvent& event);
    void recordNoteLatency(const NoteEvent& event);
    void renderVoices(float* mono, int numSamples, Waveform wf, float detune, float cutoff, float reso, float master);
    void applyArpEvent(const ArpEvent& event);
    void applySequenceOutput(const seqplay::Output& out);

    Voice voices[MAX_VOICES];
    VoiceAllocator allocator;
    TransportEngine* transport = nullptr;
    AnalysisRing* analysis = nullptr;
    TouchLatencyStats* touchLatency = nullptr;
    LatencyWindow* inputLatency = nullptr;
    Arpeggiator arpeggiator;
    DrumMachine drumMachine;
    SamplerEngine samplerEngine;
    ArpEvent arpEvents[kArpEventCapacity]; // this block's arp events (audio thread)
    int arpEventCount = 0;
    uint64_t lastAllReleasedSeen = 0;      // NoteEventQueue::allReleasedMark last applied to the arp
    seqplay::SequencePlayer sequencer;
    seqplay::NoteLog* sequenceLog = nullptr;
    seqplay::Output seqOutputs[seqplay::SequencePlayer::kMaxOutputs]; // this block's (audio thread)
    int seqOutputCount = 0;
    uint64_t lastAllReleasedSeenBySequence = 0; // NoteEventQueue::allReleasedMark last applied to the sequence
    uint64_t lastSamplerSilenceSeen = 0;        // NoteEventQueue::samplerSilenceMark last applied to the sampler

    std::atomic<int> waveform{static_cast<int>(Waveform::Sine)};
    std::atomic<float> filterCutoff{20000.0f};
    std::atomic<float> filterResonance{0.0f};
    std::atomic<float> detuneCents{0.0f};
    std::atomic<float> masterLevel{0.5f};

    NoteEventQueue ownQueue;
    NoteEventQueue* queue = &ownQueue;
};
