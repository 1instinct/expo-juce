#include "Voice.h"
#include <cmath>
#include <algorithm>
#include <cstring>

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
    fadeOutTailLength = FADE_OUT_SAMPLES;
    fadeOutTailPos = 0;
    std::memset(fadeOutTail, 0, sizeof(fadeOutTail));

    // Apply current filter params before rendering fade-out
    float mappedQ = 0.707f + filterResonance * 9.293f;
    filter.setCutoffFrequency(filterCutoff);
    filter.setResonance(mappedQ);

    for (int i = 0; i < FADE_OUT_SAMPLES; i++) {
        float envGain = adsr.getNextSample();

        float sample = 0.0f;
        switch (waveform) {
            case Waveform::Sine:    sample = sineOsc.processSample(0.0f); break;
            case Waveform::Square:  sample = squareOsc.processSample(0.0f) * 0.7f; break;
            case Waveform::Saw:     sample = sawOsc.processSample(0.0f) * 0.6f; break;
            case Waveform::Triangle: sample = triOsc.processSample(0.0f) * 0.8f; break;
        }

        float filtered = filter.processSample(0, sample);
        float fadeGain = 1.0f - ((float)i / (float)FADE_OUT_SAMPLES);
        fadeOutTail[i] = filtered * envGain * velocityGain * masterLevel * fadeGain;
    }

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

    float freqF = (float)frequency;
    if (freqF > 0.0f) {
        sineOsc.setFrequency(freqF, true);
        squareOsc.setFrequency(freqF, true);
        sawOsc.setFrequency(freqF, true);
        triOsc.setFrequency(freqF, true);
    }

    float mappedQ = 0.707f + filterResonance * 9.293f;
    filter.setCutoffFrequency(filterCutoff);
    filter.setResonance(mappedQ);

    updateADSR();

    for (int i = 0; i < numSamples; i++) {
        float envGain = adsr.getNextSample();
        if (envGain < 0.00001f && !adsr.isActive()) {
            noteNumber = -1;
            return;
        }

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

        float filtered = filter.processSample(0, sample);
        float out = filtered * envGain * velocityGain * masterLevel;

        currentLevel = envGain * velocityGain;
        buffer[i] += out;
    }
}
