#pragma once

#include "JuceConfig.h"
#include <juce_dsp/juce_dsp.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>

enum class Waveform { Sine, Square, Saw, Triangle };

// Who started a voice. Player: a note-on from the note queue (released by the queue's marks);
// Arp: the arpeggiator (stopped by its own note-offs); Sequence: the loop sequence player
// (SequencePlayer.h, stopped by its own releases). A note-on retriggers (cuts) a held voice of the
// same note unless exactly one of the two is a Sequence voice: the loop and the player never cut
// each other's notes.
enum class VoiceSource : uint8_t { Player = 0, Arp = 1, Sequence = 2 };

inline constexpr int FADE_OUT_SAMPLES = 96;

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

    // Held: between startNote and stopNote (a voice in its release stage is not held).
    bool isHeld() const { return held && noteNumber >= 0; }
    // NoteEventQueue sequence number of the note-on that started this voice. 0 = not from the
    // queue: a voice the arpeggiator started (stopped by its own note-offs, never by a release mark).
    uint64_t startSeq = 0;
    VoiceSource source = VoiceSource::Player;

    void captureAndRelease(Waveform waveform, double frequency,
                           float filterCutoff, float filterResonance,
                           float masterLevel);

    bool isActive() const;

    int getNoteNumber() const { return noteNumber; }
    float getCurrentLevel() const { return currentLevel; }

    float fadeOutTail[FADE_OUT_SAMPLES];
    int fadeOutTailLength = 0;
    int fadeOutTailPos = 0;
    bool hasFadeOutTail() const { return fadeOutTailPos < fadeOutTailLength; }

    std::atomic<float> attackMs{2.0f};
    std::atomic<float> decayMs{1.0f};
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
    bool held = false;
    float velocityGain = 0.0f;
    float currentLevel = 0.0f;
    double cachedSampleRate = 44100.0;
    bool prepared = false;
};
