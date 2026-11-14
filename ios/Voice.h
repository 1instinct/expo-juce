#pragma once

#include "JuceConfig.h"
#include <juce_dsp/juce_dsp.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <cstdint>

enum class Waveform { Sine, Square, Saw, Triangle };

// Who started a voice. Player: a note-on from the note queue (released by the queue's marks);
// Arp: the arpeggiator (stopped by its own note-offs); Sequence: the loop sequence player
// (SequencePlayer.h, stopped by its own releases). A note-on retriggers (cuts) a held voice of the
// same note unless exactly one of the two is a Sequence voice: the loop and the player never cut
// each other's notes.
enum class VoiceSource : uint8_t { Player = 0, Arp = 1, Sequence = 2 };

inline constexpr int FADE_OUT_SAMPLES = 96;

// Envelope settings as the setters store them (times in ms).
struct EnvelopeSettings {
    float attackMs = 2.0f;
    float decayMs = 1.0f;
    float sustain = 1.0f;
    float releaseMs = 200.0f;
    bool operator==(const EnvelopeSettings& o) const {
        return attackMs == o.attackMs && decayMs == o.decayMs && sustain == o.sustain && releaseMs == o.releaseMs;
    }
    bool operator!=(const EnvelopeSettings& o) const { return !(*this == o); }
};

// One synth voice. Audio thread only, except prepare() (before rendering starts).
class Voice {
public:
    Voice();

    void prepare(double sampleRate, int blockSize);

    // Adds n samples of this voice to out. The parameter arrays hold one value per sample (from
    // SynthParamSmoother): filter cutoff in Hz, resonance 0…1, pitch ratio (detune), output
    // level, envelope sustain level. A value is applied only on the sample it changes.
    void render(float* out, int n, Waveform waveform,
                const float* cutoffHz, const float* resonance, const float* pitchRatio,
                const float* level, const float* sustain);

    void startNote(int note, float velocity);
    void stopNote();

    // Envelope for this voice: applied now unless the voice is in its release stage (whose
    // rate is fixed when the note is released, see Voice.mm), otherwise on its next note.
    void setEnvelope(const EnvelopeSettings& e);

    // Held: between startNote and stopNote (a voice in its release stage is not held).
    bool isHeld() const { return held && noteNumber >= 0; }
    // NoteEventQueue sequence number of the note-on that started this voice. 0 = not from the
    // queue: a voice the arpeggiator started (stopped by its own note-offs, never by a release mark).
    uint64_t startSeq = 0;
    VoiceSource source = VoiceSource::Player;

    // Renders a short faded tail of the sounding note (with the given current parameters) into
    // fadeOutTail and frees the voice, so it can be stolen without a click.
    void captureAndRelease(Waveform waveform, float filterCutoff, float filterResonance,
                           float masterLevel);

    bool isActive() const;

    int getNoteNumber() const { return noteNumber; }
    float getCurrentLevel() const { return currentLevel; }

    float fadeOutTail[FADE_OUT_SAMPLES];
    int fadeOutTailLength = 0;
    int fadeOutTailPos = 0;
    bool hasFadeOutTail() const { return fadeOutTailPos < fadeOutTailLength; }

private:
    void applyEnvelope(bool force = false);
    void setFrequency(float hz);
    void setFilter(float cutoffHz, float resonance);
    float nextOscSample(Waveform waveform);

    juce::dsp::Oscillator<float> sineOsc;
    juce::dsp::Oscillator<float> squareOsc;
    juce::dsp::Oscillator<float> sawOsc;
    juce::dsp::Oscillator<float> triOsc;
    juce::dsp::StateVariableTPTFilter<float> filter;
    juce::ADSR adsr;

    EnvelopeSettings envelope;          // wanted
    EnvelopeSettings appliedEnvelope;   // what adsr holds (after prepare)
    // Last values handed to the oscillators / filter (-1 = none yet: set on the next sample).
    float appliedFreq = -1.0f;
    float appliedCutoff = -1.0f;
    float appliedResonance = -1.0f;

    int noteNumber = -1;
    bool held = false;
    float baseFreq = 440.0f;
    float lastPitchRatio = 1.0f;        // detune ratio of the last pitch rendered
    float velocityGain = 0.0f;
    float currentLevel = 0.0f;
    double cachedSampleRate = 44100.0;
    bool prepared = false;
};
