#include "PolySynthProcessor.h"
#include <cmath>
#include <algorithm>

PolySynthProcessor::PolySynthProcessor()
    : AudioProcessor(BusesProperties()
        .withOutput("Output", juce::AudioChannelSet::stereo(), true)) {
}

void PolySynthProcessor::prepareToPlay(double sampleRate, int samplesPerBlock) {
    for (int i = 0; i < MAX_VOICES; i++) {
        voices[i].prepare(sampleRate, samplesPerBlock);
    }
}

void PolySynthProcessor::releaseResources() {}

void PolySynthProcessor::processBlock(juce::AudioBuffer<float>& buffer,
                                       juce::MidiBuffer& /*midiMessages*/) {
    buffer.clear();

    int numSamples = buffer.getNumSamples();
    double sr = getSampleRate();

    Waveform wf = static_cast<Waveform>(waveform.load());
    float cutoff = filterCutoff.load();
    float reso = filterResonance.load();
    float detune = detuneCents.load();
    float master = masterLevel.load();

    float* monoBuffer = buffer.getWritePointer(0);

    for (int v = 0; v < MAX_VOICES; v++) {
        if (voices[v].hasFadeOutTail()) {
            int tailRemaining = voices[v].fadeOutTailLength - voices[v].fadeOutTailPos;
            int samplesToMix = std::min(tailRemaining, numSamples);
            for (int i = 0; i < samplesToMix; i++) {
                monoBuffer[i] += voices[v].fadeOutTail[voices[v].fadeOutTailPos + i];
            }
            voices[v].fadeOutTailPos += samplesToMix;
        }
    }

    for (int v = 0; v < MAX_VOICES; v++) {
        if (!voices[v].isActive()) continue;

        int note = voices[v].getNoteNumber();
        double freq = 440.0 * std::pow(2.0, (note - 69) / 12.0);
        if (std::fabs(detune) > 0.01f) {
            freq *= std::pow(2.0, (double)detune / 1200.0);
        }

        voices[v].renderBlock(monoBuffer, numSamples,
                              wf, freq, cutoff, reso, master);
    }

    if (buffer.getNumChannels() > 1) {
        buffer.copyFrom(1, 0, buffer, 0, 0, numSamples);
    }

    if (transport) {
        if (transport->advanceBlock(numSamples, sr)) {
            beatState.crossed.store(true);
            beatState.beat.store(transport->getPosition());
            beatState.bpm.store(transport->getTempo());
        }
    }
}

void PolySynthProcessor::noteOn(int note, int velocity) {
    int clampedNote = std::clamp(note, 0, 127);
    int clampedVel = std::clamp(velocity, 0, 127);
    allocator.noteOn(clampedNote, (float)clampedVel, voices,
                     static_cast<Waveform>(waveform.load()),
                     (double)detuneCents.load(),
                     filterCutoff.load(), filterResonance.load(),
                     masterLevel.load());
}

void PolySynthProcessor::noteOff(int note) {
    int clampedNote = std::clamp(note, 0, 127);
    allocator.noteOff(clampedNote, voices);
}

void PolySynthProcessor::allNotesOff() {
    allocator.allNotesOff(voices);
}

void PolySynthProcessor::setWaveform(Waveform wf) { waveform.store(static_cast<int>(wf)); }

void PolySynthProcessor::setFilterCutoff(double hz) {
    filterCutoff.store((float)std::clamp(hz, 20.0, 20000.0));
}

void PolySynthProcessor::setFilterResonance(double q) {
    filterResonance.store((float)std::clamp(q, 0.0, 1.0));
}

void PolySynthProcessor::setAttack(double ms) {
    float val = (float)std::clamp(ms, 1.0, 10000.0);
    for (int i = 0; i < MAX_VOICES; i++) voices[i].attackMs.store(val);
}

void PolySynthProcessor::setDecay(double ms) {
    float val = (float)std::clamp(ms, 1.0, 10000.0);
    for (int i = 0; i < MAX_VOICES; i++) voices[i].decayMs.store(val);
}

void PolySynthProcessor::setSustain(double level) {
    float val = (float)std::clamp(level, 0.0, 1.0);
    for (int i = 0; i < MAX_VOICES; i++) voices[i].sustainLevel.store(val);
}

void PolySynthProcessor::setRelease(double ms) {
    float val = (float)std::clamp(ms, 1.0, 10000.0);
    for (int i = 0; i < MAX_VOICES; i++) voices[i].releaseMs.store(val);
}

void PolySynthProcessor::setDetune(double cents) {
    detuneCents.store((float)std::clamp(cents, -1200.0, 1200.0));
}

void PolySynthProcessor::setMasterLevel(double level) {
    masterLevel.store((float)std::clamp(level, 0.0, 1.0));
}
