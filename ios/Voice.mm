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

    // The ADSR's rates depend on the sample rate. A voice in its release keeps the rate its
    // noteOff set (re-applying it now would cut a note released at sustain 0, see setEnvelope);
    // it gets the new rates on its next note.
    adsr.setSampleRate(sampleRate);
    if (held || noteNumber < 0) applyEnvelope(true);
    else appliedEnvelope.attackMs = -1.0f; // matches no envelope: applied on the next note
    cachedSampleRate = sampleRate;
    appliedFreq = appliedCutoff = appliedResonance = -1.0f;
    prepared = true;
}

void Voice::startNote(int note, float velocity) {
    noteNumber = note;
    velocityGain = velocity / 127.0f;
    baseFreq = 440.0f * std::pow(2.0f, (note - 69) / 12.0f);

    sineOsc.reset();
    squareOsc.reset();
    sawOsc.reset();
    triOsc.reset();
    filter.reset();
    // The pitch starts with this voice's last pitch ratio, so a voice stolen again before it
    // renders leaves a tail at its own pitch; the first rendered sample sets the smoothed ratio
    // (and the filter) of that sample.
    setFrequency(baseFreq * lastPitchRatio);
    appliedCutoff = appliedResonance = -1.0f;

    applyEnvelope();
    held = true;
    adsr.noteOn();
}

void Voice::stopNote() {
    held = false;
    adsr.noteOff();
}

// Envelope times change the slope of the running stage, never the envelope's value, so a new
// attack / decay / release time applies at once without a click (and without smoothing: a
// smoothed time would only arrive later). The sustain level is a value: the processor ramps it
// (SynthParamSmoother) and render() applies it per sample.
//
// Not applied during the release stage: juce::ADSR::noteOff sets the release rate from the
// level the note was released at (so it reaches 0 after releaseMs), and setParameters would
// replace it with sustain / releaseMs, which is 0 at sustain 0 (the note was cut on the next
// block) and gives the wrong release length for a note released during its attack or decay. A
// voice in its release picks the settings up on its next note.
void Voice::setEnvelope(const EnvelopeSettings& e) {
    if (e == envelope) return;
    envelope = e;
    if (held || noteNumber < 0) applyEnvelope();
}

void Voice::applyEnvelope(bool force) {
    if (!force && envelope == appliedEnvelope) return;
    juce::ADSR::Parameters p {
        envelope.attackMs * 0.001f,
        envelope.decayMs * 0.001f,
        envelope.sustain,
        envelope.releaseMs * 0.001f
    };
    adsr.setParameters(p);
    appliedEnvelope = envelope;
}

void Voice::setFrequency(float hz) {
    // force = true: the oscillators' own 50 ms frequency smoothing is bypassed (pitch is
    // smoothed by the caller), so a new note starts exactly at its pitch.
    sineOsc.setFrequency(hz, true);
    squareOsc.setFrequency(hz, true);
    sawOsc.setFrequency(hz, true);
    triOsc.setFrequency(hz, true);
    appliedFreq = hz;
}

void Voice::setFilter(float cutoffHz, float resonance) {
    if (cutoffHz != appliedCutoff) {
        filter.setCutoffFrequency(cutoffHz);
        appliedCutoff = cutoffHz;
    }
    if (resonance != appliedResonance) {
        filter.setResonance(0.707f + resonance * 9.293f);
        appliedResonance = resonance;
    }
}

float Voice::nextOscSample(Waveform waveform) {
    switch (waveform) {
        case Waveform::Sine:     return sineOsc.processSample(0.0f);
        case Waveform::Square:   return squareOsc.processSample(0.0f) * 0.7f;
        case Waveform::Saw:      return sawOsc.processSample(0.0f) * 0.6f;
        case Waveform::Triangle: return triOsc.processSample(0.0f) * 0.8f;
    }
    return 0.0f;
}

void Voice::captureAndRelease(Waveform waveform, float filterCutoff, float filterResonance,
                              float masterLevel) {
    fadeOutTailLength = FADE_OUT_SAMPLES;
    fadeOutTailPos = 0;
    std::memset(fadeOutTail, 0, sizeof(fadeOutTail));

    setFilter(filterCutoff, filterResonance);

    for (int i = 0; i < FADE_OUT_SAMPLES; i++) {
        float envGain = adsr.getNextSample();
        float filtered = filter.processSample(0, nextOscSample(waveform));
        float fadeGain = 1.0f - ((float)i / (float)FADE_OUT_SAMPLES);
        fadeOutTail[i] = filtered * envGain * velocityGain * masterLevel * fadeGain;
    }

    noteNumber = -1;
    held = false;
    currentLevel = 0.0f;
}

bool Voice::isActive() const {
    return noteNumber >= 0 && adsr.isActive();
}

void Voice::render(float* out, int n, Waveform waveform,
                   const float* cutoffHz, const float* resonance, const float* pitchRatio,
                   const float* level, const float* sustain) {
    if (!prepared || !isActive()) return;

    for (int i = 0; i < n; i++) {
        if (sustain[i] != envelope.sustain) {
            envelope.sustain = sustain[i];
            if (held) applyEnvelope();
        }

        float envGain = adsr.getNextSample();
        if (envGain < 0.00001f && !adsr.isActive()) {
            noteNumber = -1;
            return;
        }

        const float freq = baseFreq * pitchRatio[i];
        if (freq != appliedFreq) {
            setFrequency(freq);
            lastPitchRatio = pitchRatio[i];
        }
        setFilter(cutoffHz[i], resonance[i]);

        float filtered = filter.processSample(0, nextOscSample(waveform));
        out[i] += filtered * envGain * velocityGain * level[i];
        currentLevel = envGain * velocityGain;
    }
}
