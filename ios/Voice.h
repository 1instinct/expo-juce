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
