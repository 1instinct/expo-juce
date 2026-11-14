#include "PolySynthProcessor.h"
#include <cmath>
#include <algorithm>

PolySynthProcessor::PolySynthProcessor()
    : AudioProcessor(BusesProperties()
        .withOutput("Output", juce::AudioChannelSet::stereo(), true)) {
    lastAllReleasedSeen = queue->allReleasedMark();
}

PolySynthProcessor::~PolySynthProcessor() {
    if (SamplerBufferSource* source = samplerEngine.bufferSource()) source->releaseReader();
}

void PolySynthProcessor::prepareToPlay(double sampleRate, int samplesPerBlock) {
    for (int i = 0; i < MAX_VOICES; i++) {
        voices[i].prepare(sampleRate, samplesPerBlock);
    }
    drumMachine.prepare(sampleRate);
    samplerEngine.prepare(sampleRate, samplesPerBlock);
    if (SamplerBufferSource* source = samplerEngine.bufferSource()) source->requestRate(sampleRate);
}

void PolySynthProcessor::releaseResources() {}

void PolySynthProcessor::processBlock(juce::AudioBuffer<float>& buffer,
                                       juce::MidiBuffer& /*midiMessages*/) {
    buffer.clear();

    const int numSamples = buffer.getNumSamples();
    const double sr = getSampleRate();

    // Transport first, so the block-start beat / bpm / play state describe this block here as
    // they do for the effects and aux processors downstream (TransportEngine::getBlockStartBeat).
    double blockBeat = 0.0, bpm = 120.0;
    bool playing = false;
    if (transport) {
        if (transport->advanceBlock(numSamples, sr)) {
            beatState.crossed.store(true);
            beatState.beat.store(transport->getPosition());
            beatState.bpm.store(transport->getTempo());
        }
        blockBeat = transport->getBlockStartBeat();
        bpm = transport->getBlockBpm();
        playing = transport->isBlockPlaying();
    }

    // Note-ons waiting in the queue: to the arpeggiator when it is enabled, else straight to
    // the voices. Then releases: every held voice whose note-on is at or below its release mark
    // stops (see NoteEventQueue; arp voices have startSeq 0 and are not affected), and the arp
    // drops the held notes the player released. Applied every block, so a release sent while
    // the device was stopped or reopening takes effect on the first block after it. The arp's
    // options are snapshotted once here and hold for the whole block. Drum hits in the queue
    // start their drum voice whether or not the arpeggiator is enabled; so do sampler events, which
    // the sampler releases from its own marks (or allNotesOff) right after the drain.
    const ArpSettings& arpNow = arpeggiator.beginBlock();
    samplerEngine.beginBlock(); // this block's slot buffers and parameters
    if (arpNow.enabled) {
        queue->drainNoteOns([this](const NoteEvent& event) {
            if (event.kind == NoteEventKind::Drum) { startDrumHit(event); return; }
            if (event.kind != NoteEventKind::Note) { startSamplerEvent(event); return; }
            arpeggiator.noteOn(event.note, event.velocity, event.seq);
            recordNoteLatency(event);
        });
        const uint64_t allReleased = queue->allReleasedMark();
        if (allReleased != lastAllReleasedSeen) {
            lastAllReleasedSeen = allReleased;
            arpeggiator.allNotesOff();
        }
        arpeggiator.releaseHeld([this](int note, uint64_t seq) { return queue->isReleased(note, seq); });
    } else {
        queue->drainNoteOns([this](const NoteEvent& event) {
            if (event.kind == NoteEventKind::Drum) startDrumHit(event);
            else if (event.kind == NoteEventKind::Note) startQueuedNote(event);
            else startSamplerEvent(event);
        });
        lastAllReleasedSeen = queue->allReleasedMark();
    }
    samplerEngine.releaseHeld([this](bool fromKey, int id, uint64_t seq) { return queue->isSamplerReleased(fromKey, id, seq); });
    for (int v = 0; v < MAX_VOICES; v++) {
        if (voices[v].isHeld() && queue->isReleased(voices[v].getNoteNumber(), voices[v].startSeq))
            voices[v].stopNote();
    }

    // The arp's events for this block (always run: disabling stops its voices here).
    arpEventCount = 0;
    arpeggiator.process(blockBeat, bpm, playing, sr, numSamples, [this](const ArpEvent& event) {
        if (arpEventCount < kArpEventCapacity) arpEvents[arpEventCount++] = event;
    });

    const Waveform wf = static_cast<Waveform>(waveform.load());
    const float cutoff = filterCutoff.load();
    const float reso = filterResonance.load();
    const float detune = detuneCents.load();
    const float master = masterLevel.load();

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

    // Render in segments split at the arp's event offsets (ascending): the voices play up to
    // each event, the event starts or stops its voice(s), and so on. No events: one segment.
    int pos = 0;
    for (int i = 0; i < arpEventCount; ++i) {
        const int at = std::clamp(arpEvents[i].sampleOffset, pos, numSamples);
        if (at > pos) renderVoices(monoBuffer + pos, at - pos, wf, detune, cutoff, reso, master);
        pos = at;
        applyArpEvent(arpEvents[i]);
    }
    if (pos < numSamples) renderVoices(monoBuffer + pos, numSamples - pos, wf, detune, cutoff, reso, master);

    // Synth-only peak (diag.peak), then the drum part on top (with no drum sounding it does not
    // touch the buffer).
    const float synthBlockPeak = numSamples > 0 ? buffer.getMagnitude(0, 0, numSamples) : 0.0f;
    drumMachine.render(monoBuffer, numSamples);

    if (buffer.getNumChannels() > 1) {
        buffer.copyFrom(1, 0, buffer, 0, 0, numSamples);
        // The sampler is stereo: added after the mono parts are copied to both channels. With
        // nothing sounding it does not touch the buffer.
        samplerEngine.render(buffer.getWritePointer(0), buffer.getWritePointer(1), numSamples);
    }

    if (analysis != nullptr)
        analysis->publishArp(arpeggiator.currentStep(), arpeggiator.patternLength(), arpeggiator.lastNote(),
                             arpeggiator.isActive());

    // ── Diagnostics (relaxed atomics only; no locks/allocation) ──
    {
        const float held = diag.peak.load(std::memory_order_relaxed) * diagPeakDecay(numSamples, sr);
        diag.peak.store(std::max(synthBlockPeak, held), std::memory_order_relaxed);
        int active = 0;
        for (int v = 0; v < MAX_VOICES; v++)
            if (voices[v].isActive()) ++active;
        diag.activeVoices.store(active, std::memory_order_relaxed);
        diag.processCount.fetch_add(1, std::memory_order_relaxed);
    }
}

// Audio thread: every active voice over `numSamples` samples into `mono` (summed).
void PolySynthProcessor::renderVoices(float* mono, int numSamples, Waveform wf, float detune, float cutoff,
                                      float reso, float master) {
    for (int v = 0; v < MAX_VOICES; v++) {
        if (!voices[v].isActive()) continue;

        const int note = voices[v].getNoteNumber();
        double freq = 440.0 * std::pow(2.0, (note - 69) / 12.0);
        if (std::fabs(detune) > 0.01f) {
            freq *= std::pow(2.0, (double)detune / 1200.0);
        }

        voices[v].renderBlock(mono, numSamples, wf, freq, cutoff, reso, master);
    }
}

// Audio thread: an arp note-on starts a voice with startSeq 0 (its note-on never enters the
// queue's release marks); an arp note-off stops every arp-started voice of that note.
void PolySynthProcessor::applyArpEvent(const ArpEvent& event) {
    if (event.note < 0 || event.note > 127) return;
    if (event.on) {
        allocator.noteOn(event.note, (float)std::clamp(event.velocity, 0, 127), voices,
                         static_cast<Waveform>(waveform.load()),
                         (double)detuneCents.load(),
                         filterCutoff.load(), filterResonance.load(),
                         masterLevel.load(), 0);
        queue->lastNoteStarted.store(event.note, std::memory_order_relaxed);
        return;
    }
    for (int v = 0; v < MAX_VOICES; v++) {
        if (voices[v].isHeld() && voices[v].startSeq == 0 && voices[v].getNoteNumber() == event.note)
            voices[v].stopNote();
    }
}

// Audio thread (processBlock): start a queued drum hit and record its latency.
void PolySynthProcessor::startDrumHit(const NoteEvent& event) {
    drumMachine.trigger(event.note, event.velocity);
    recordNoteLatency(event);
}

// Audio thread (processBlock): start a queued sampler pad hit or key note-on and record its latency.
void PolySynthProcessor::startSamplerEvent(const NoteEvent& event) {
    if (event.kind == NoteEventKind::SamplerHit) samplerEngine.hit(event.note, event.velocity, event.seq);
    else samplerEngine.noteOn(event.note, event.velocity, event.seq);
    recordNoteLatency(event);
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
    recordNoteLatency(event);
}

// Audio thread: enqueue → consume latency of a note-on (direct or into the arpeggiator).
void PolySynthProcessor::recordNoteLatency(const NoteEvent& event) {
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
    // Per-leg windows for touch-originated events (this thread is their only writer). Raw
    // differences: a negative value would mean the clocks are not the same, so it is kept.
    if (event.touchTicks != 0 && touchLatency != nullptr) {
        const auto ms = [](juce::int64 ticks) { return juce::Time::highResolutionTicksToSeconds(ticks) * 1000.0; };
        if (event.entryTicks != 0 && event.stampTicks != 0)
            touchLatency->handler.push(ms(event.stampTicks - event.entryTicks));
        if (event.stampTicks != 0)
            touchLatency->queueToRender.push(ms(nowTicks - event.stampTicks));
        touchLatency->touchToRender.push(ms(nowTicks - event.touchTicks));
    }
    // MIDI input: arrival → this block (this thread is the window's only writer).
    if (event.inputTicks != 0 && inputLatency != nullptr)
        inputLatency->push(juce::Time::highResolutionTicksToSeconds(nowTicks - event.inputTicks) * 1000.0);
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
             detuneCents.load(), masterLevel.load(), arpeggiator.getSettings() };
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
    arpeggiator.setSettings(s.arp);
}
