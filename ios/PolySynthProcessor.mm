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

    // Note-ons waiting in the queue, then releases: every held voice whose note-on is at or
    // below its release mark stops (see NoteEventQueue). Applied every block, so a release sent
    // while the device was stopped or reopening takes effect on the first block after it.
    queue->drainNoteOns([this](const NoteEvent& event) { startQueuedNote(event); });
    for (int v = 0; v < MAX_VOICES; v++) {
        if (voices[v].isHeld() && queue->isReleased(voices[v].getNoteNumber(), voices[v].startSeq))
            voices[v].stopNote();
    }

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

    // ── Diagnostics (relaxed atomics only; no locks/allocation) ──
    {
        const float blockPeak = numSamples > 0 ? buffer.getMagnitude(0, 0, numSamples) : 0.0f;
        const float held = diag.peak.load(std::memory_order_relaxed) * diagPeakDecay(numSamples, sr);
        diag.peak.store(std::max(blockPeak, held), std::memory_order_relaxed);
        int active = 0;
        for (int v = 0; v < MAX_VOICES; v++)
            if (voices[v].isActive()) ++active;
        diag.activeVoices.store(active, std::memory_order_relaxed);
        diag.processCount.fetch_add(1, std::memory_order_relaxed);
    }

    if (transport) {
        if (transport->advanceBlock(numSamples, sr)) {
            beatState.crossed.store(true);
            beatState.beat.store(transport->getPosition());
            beatState.bpm.store(transport->getTempo());
        }
    }
}

// Audio thread (processBlock): start a queued note-on and record its latency.
void PolySynthProcessor::startQueuedNote(const NoteEvent& event) {
    const int clampedNote = std::clamp(event.note, 0, 127);
    const int clampedVel = std::clamp(event.velocity, 0, 127);
    allocator.noteOn(clampedNote, (float)clampedVel, voices,
                     static_cast<Waveform>(waveform.load()),
                     (double)detuneCents.load(),
                     filterCutoff.load(), filterResonance.load(),
                     masterLevel.load(), event.seq);
    queue->lastNoteStarted.store(clampedNote, std::memory_order_relaxed);

    const juce::int64 nowTicks = juce::Time::getHighResolutionTicks();
    if (event.stampTicks != 0) {
        const juce::int64 elapsed = nowTicks - event.stampTicks;
        diag.lastNoteToRenderMs.store(juce::Time::highResolutionTicksToSeconds(elapsed) * 1000.0,
                                      std::memory_order_relaxed);
    }
    if (event.touchTicks != 0 && nowTicks >= event.touchTicks) {
        const juce::int64 elapsed = nowTicks - event.touchTicks;
        diag.lastTouchToRenderMs.store(juce::Time::highResolutionTicksToSeconds(elapsed) * 1000.0,
                                       std::memory_order_relaxed);
    }
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

SynthSettings PolySynthProcessor::getSettings() const {
    // The envelope is stored per voice but always set on all of them; voice 0 holds it.
    return { waveform.load(), filterCutoff.load(), filterResonance.load(),
             voices[0].attackMs.load(), voices[0].decayMs.load(),
             voices[0].sustainLevel.load(), voices[0].releaseMs.load(),
             detuneCents.load(), masterLevel.load() };
}

void PolySynthProcessor::applySettings(const SynthSettings& s) {
    setWaveform(static_cast<Waveform>(s.waveform));
    setFilterCutoff(s.filterCutoff);
    setFilterResonance(s.filterResonance);
    setAttack(s.attackMs);
    setDecay(s.decayMs);
    setSustain(s.sustainLevel);
    setRelease(s.releaseMs);
    setDetune(s.detuneCents);
    setMasterLevel(s.masterLevel);
}
