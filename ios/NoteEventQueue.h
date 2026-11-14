#pragma once

// Pure C++ (no Objective-C): used by PolySynthProcessor, ExpoJuceAudioEngine.mm and the
// offline harness (test/harness).

#include "JuceConfig.h"
#include <juce_core/juce_core.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <thread>

// What a queued event starts.
enum class NoteEventKind : uint8_t {
    Note = 0, // a synth note-on (`note` 0–127)
    Drum = 1, // a drum hit (`note` = drum voice 0–8); one-shot, never released, never arpeggiated
    SamplerHit = 2,  // a sampler pad hit (`note` = slot 0–15); released by pushSamplerRelease marks
    SamplerNote = 3, // a sampler key note-on (`note` 0–127); released by pushSamplerNoteOff marks
};

// A note-on (or drum hit) waiting for the audio thread.
struct NoteEvent {
    int note;               // 0–127, the drum voice for kind Drum, the slot for kind SamplerHit
    int velocity;
    uint64_t seq;           // NoteEventQueue sequence number (1, 2, …), compared with the release marks; 0 for a drum hit
    juce::int64 stampTicks; // juce::Time::getHighResolutionTicks() at enqueue
    juce::int64 touchTicks; // originating UITouch.timestamp in high-res ticks (same clock); 0 if not a touch
    NoteEventKind kind = NoteEventKind::Note;
    juce::int64 entryTicks = 0; // the touch handler's entry (touchesBegan) in high-res ticks; 0 if unknown
    juce::int64 inputTicks = 0; // arrival of the MIDI input bytes that carried it (MidiInput.h), high-res ticks; 0 if none
};

// Note input for the synth: note-ons travel in a ring, releases are marks.
//
// Note-ons: multi-producer (main thread + JS thread, serialised by a spinlock), single consumer
// (the audio thread, lock-free). A full ring drops the note-on (counted in notesDropped).
//
// Releases are level-triggered marks, not queued events, so nothing can drop them: noteOff(n)
// raises releasedUpTo[n] to the sequence number of the latest note-on of n, allNotesOff raises
// allReleasedUpTo to the latest note-on of any note. Every block the synth stops each held voice
// whose note-on is at or below its mark (PolySynthProcessor::processBlock). A note-on pushed after
// a release has a higher sequence number, so a release never cuts a later note. Producers only
// store atomics: no lock to wait for, no slot to run out of.
//
// The engine keeps one queue for the life of the process, outside the objects a rebuild replaces,
// so note input never needs the engine lock, and a release sent while the device is stopped or
// being reopened is applied on the first block after it.
class NoteEventQueue {
public:
    static constexpr int kSize = 256;

    // ── Producers (any thread) ──

    // Returns false if the ring is full (counted in notesDropped).
    // touchTicks / entryTicks: the originating UITouch and the touch handler's entry, in
    // juce::Time high-resolution ticks (0 = not from a touch / unknown); see TouchLatencyStats.
    // inputTicks: arrival of the MIDI input bytes that carried it (0 = not MIDI input).
    bool pushNoteOn(int note, int velocity, juce::int64 touchTicks = 0, juce::int64 entryTicks = 0,
                    juce::int64 inputTicks = 0) {
        note = std::clamp(note, 0, 127);
        SpinGuard guard(producerLock);
        const int wp = writePos.load(std::memory_order_relaxed);
        const int next = (wp + 1) % kSize;
        if (next == readPos.load(std::memory_order_acquire)) {
            notesDropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        const uint64_t seq = lastSeq.load(std::memory_order_relaxed) + 1;
        ring[wp] = { note, velocity, seq, juce::Time::getHighResolutionTicks(), touchTicks, NoteEventKind::Note, entryTicks, inputTicks };
        lastOnSeq[note].store(seq, std::memory_order_release);
        lastSeq.store(seq, std::memory_order_release);
        // Count before publishing: the release-store below orders this increment ahead of the
        // consumer's notesConsumed increment for the same event (consumed never exceeds queued).
        notesQueued.fetch_add(1, std::memory_order_relaxed);
        writePos.store(next, std::memory_order_release);
        return true;
    }

    // A drum hit (voice 0..kMaxDrumVoice, velocity clamped to 1–127) through the same ring as the
    // note-ons, so hits and notes keep their order. Hits carry no sequence number and are not
    // touched by the release marks. Returns false for an out-of-range voice (ignored, not counted)
    // or a full ring (counted in drumHitsDropped).
    static constexpr int kMaxDrumVoice = 8;
    bool pushDrumHit(int voice, int velocity, juce::int64 touchTicks = 0, juce::int64 entryTicks = 0,
                     juce::int64 inputTicks = 0) {
        if (voice < 0 || voice > kMaxDrumVoice) return false;
        velocity = std::clamp(velocity, 1, 127);
        SpinGuard guard(producerLock);
        const int wp = writePos.load(std::memory_order_relaxed);
        const int next = (wp + 1) % kSize;
        if (next == readPos.load(std::memory_order_acquire)) {
            drumHitsDropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        ring[wp] = { voice, velocity, 0, juce::Time::getHighResolutionTicks(), touchTicks, NoteEventKind::Drum, entryTicks, inputTicks };
        drumHitsQueued.fetch_add(1, std::memory_order_relaxed);
        writePos.store(next, std::memory_order_release);
        return true;
    }

    // Sampler events (0.10.0+) through the same ring, so they keep their order with notes and
    // hits. They take sequence numbers from the same counter as note-ons, so allNotesOff releases
    // them too, but have marks of their own: a pad hit (slot 0..kMaxSamplerSlot) is released by
    // pushSamplerRelease(slot), a key note-on (0–127) by pushSamplerNoteOff(note); the synth's
    // noteOff never touches them. Velocity clamped to 1–127. Returns false for an out-of-range
    // slot / note (ignored, not counted) or a full ring (counted in samplerEventsDropped).
    static constexpr int kMaxSamplerSlot = 15;
    bool pushSamplerHit(int slot, int velocity, juce::int64 touchTicks = 0, juce::int64 entryTicks = 0) {
        if (slot < 0 || slot > kMaxSamplerSlot) return false;
        return pushSampler(NoteEventKind::SamplerHit, slot, velocity, samplerSlotLastSeq[slot], touchTicks, entryTicks);
    }
    // A key note-on with velocity 0 (or less) is a note-off, as in MIDI: nothing is queued.
    bool pushSamplerNoteOn(int note, int velocity, juce::int64 touchTicks = 0, juce::int64 entryTicks = 0) {
        if (note < 0 || note > 127) return false;
        if (velocity <= 0) {
            pushSamplerNoteOff(note);
            return true;
        }
        return pushSampler(NoteEventKind::SamplerNote, note, velocity, samplerLastOnSeq[note], touchTicks, entryTicks);
    }
    // Releases every sampler key note-on of `note` / pad hit of `slot` pushed so far.
    void pushSamplerNoteOff(int note) {
        if (note < 0 || note > 127) return;
        raise(samplerReleasedUpTo[note], samplerLastOnSeq[note].load(std::memory_order_acquire));
    }
    void pushSamplerRelease(int slot) {
        if (slot < 0 || slot > kMaxSamplerSlot) return;
        raise(samplerSlotReleasedUpTo[slot], samplerSlotLastSeq[slot].load(std::memory_order_acquire));
    }

    // Ends every sampler voice started by an event pushed so far, one-shots included (they ignore
    // releases and allNotesOff): the engine's interruption and background stop, so a long one-shot
    // neither resumes after a call nor keeps sounding after the app leaves the screen. A mark like
    // the releases: a sampler event pushed later is not affected.
    void pushSamplerSilence() {
        raise(samplerSilencedUpTo, lastSeq.load(std::memory_order_acquire));
    }

    // Releases every note-on of `note` pushed so far.
    void pushNoteOff(int note) {
        if (note < 0 || note > 127) return;
        raise(releasedUpTo[note], lastOnSeq[note].load(std::memory_order_acquire));
    }

    // Releases every note-on pushed so far.
    void pushAllNotesOff() {
        raise(allReleasedUpTo, lastSeq.load(std::memory_order_acquire));
    }

    // ── Consumer (the audio thread) ──

    template <typename Handler>
    void drainNoteOns(Handler&& onNoteOn) {
        int rp = readPos.load(std::memory_order_relaxed);
        const int wp = writePos.load(std::memory_order_acquire);
        while (rp != wp) {
            onNoteOn(ring[rp]);
            counterFor(ring[rp].kind, drumHitsConsumed, samplerEventsConsumed, notesConsumed).fetch_add(1, std::memory_order_relaxed);
            rp = (rp + 1) % kSize;
        }
        readPos.store(rp, std::memory_order_release);
    }

    // True if the note-on `seq` of `note` has been released (seq 0 = not from this queue).
    bool isReleased(int note, uint64_t seq) const {
        if (seq == 0) return false;
        uint64_t mark = allReleasedUpTo.load(std::memory_order_acquire);
        if (note >= 0 && note <= 127)
            mark = std::max(mark, releasedUpTo[note].load(std::memory_order_acquire));
        return seq <= mark;
    }

    // True if the sampler event `seq` has been released: a key note-on of `id` (fromKey) or a
    // pad hit of slot `id`, by its own mark or allNotesOff (seq 0 = not from this queue).
    bool isSamplerReleased(bool fromKey, int id, uint64_t seq) const {
        if (seq == 0) return false;
        uint64_t mark = allReleasedUpTo.load(std::memory_order_acquire);
        if (fromKey && id >= 0 && id <= 127)
            mark = std::max(mark, samplerReleasedUpTo[id].load(std::memory_order_acquire));
        else if (!fromKey && id >= 0 && id <= kMaxSamplerSlot)
            mark = std::max(mark, samplerSlotReleasedUpTo[id].load(std::memory_order_acquire));
        return seq <= mark;
    }

    // The allNotesOff mark (sequence number every note-on at or below is released by). The
    // consumer compares it with the value it last saw to detect a pushAllNotesOff, which also
    // clears the arpeggiator's latched list (a per-note release alone keeps it).
    uint64_t allReleasedMark() const { return allReleasedUpTo.load(std::memory_order_acquire); }

    // The pushSamplerSilence mark: sampler voices whose event is at or below it are ended.
    uint64_t samplerSilenceMark() const { return samplerSilencedUpTo.load(std::memory_order_acquire); }

    // Drops queued note-ons (counted in notesDiscarded). Consumer side: call only while no audio
    // thread drains the queue (engine setup, before the device callback is attached).
    void discardPending() {
        int rp = readPos.load(std::memory_order_relaxed);
        const int wp = writePos.load(std::memory_order_acquire);
        for (; rp != wp; rp = (rp + 1) % kSize)
            counterFor(ring[rp].kind, drumHitsDiscarded, samplerEventsDiscarded, notesDiscarded).fetch_add(1, std::memory_order_relaxed);
        readPos.store(wp, std::memory_order_release);
    }

    // Cumulative counters (relaxed). notesQueued == notesConsumed + notesDiscarded + still queued;
    // attempted note-ons == notesQueued + notesDropped.
    std::atomic<uint32_t> notesQueued{0};
    std::atomic<uint32_t> notesConsumed{0};
    std::atomic<uint32_t> notesDropped{0};   // note-on rejected: ring full
    std::atomic<uint32_t> notesDiscarded{0}; // dropped by discardPending (engine setup)
    // Drum hits, counted apart from the notes (same invariants).
    std::atomic<uint32_t> drumHitsQueued{0};
    std::atomic<uint32_t> drumHitsConsumed{0};
    std::atomic<uint32_t> drumHitsDropped{0};
    std::atomic<uint32_t> drumHitsDiscarded{0};
    // Sampler events (pad hits and key note-ons together), same invariants.
    std::atomic<uint32_t> samplerEventsQueued{0};
    std::atomic<uint32_t> samplerEventsConsumed{0};
    std::atomic<uint32_t> samplerEventsDropped{0};
    std::atomic<uint32_t> samplerEventsDiscarded{0};
    // Last note the synth started (0–127), -1 = none yet. Written by the consumer (relaxed).
    std::atomic<int> lastNoteStarted{-1};

private:
    // Spinlock guard for the producer side (test_and_set acquire / clear release). Critical
    // sections are a few stores; yield in case the holder was preempted.
    struct SpinGuard {
        std::atomic_flag& flag;
        explicit SpinGuard(std::atomic_flag& f) : flag(f) {
            while (flag.test_and_set(std::memory_order_acquire)) std::this_thread::yield();
        }
        ~SpinGuard() { flag.clear(std::memory_order_release); }
        SpinGuard(const SpinGuard&) = delete;
        SpinGuard& operator=(const SpinGuard&) = delete;
    };

    static std::atomic<uint32_t>& counterFor(NoteEventKind kind, std::atomic<uint32_t>& drum,
                                             std::atomic<uint32_t>& sampler, std::atomic<uint32_t>& note) {
        if (kind == NoteEventKind::Drum) return drum;
        if (kind == NoteEventKind::SamplerHit || kind == NoteEventKind::SamplerNote) return sampler;
        return note;
    }

    bool pushSampler(NoteEventKind kind, int id, int velocity, std::atomic<uint64_t>& lastSeqOf, juce::int64 touchTicks,
                     juce::int64 entryTicks) {
        velocity = std::clamp(velocity, 1, 127);
        SpinGuard guard(producerLock);
        const int wp = writePos.load(std::memory_order_relaxed);
        const int next = (wp + 1) % kSize;
        if (next == readPos.load(std::memory_order_acquire)) {
            samplerEventsDropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        const uint64_t seq = lastSeq.load(std::memory_order_relaxed) + 1;
        ring[wp] = { id, velocity, seq, juce::Time::getHighResolutionTicks(), touchTicks, kind, entryTicks, 0 };
        lastSeqOf.store(seq, std::memory_order_release);
        lastSeq.store(seq, std::memory_order_release);
        samplerEventsQueued.fetch_add(1, std::memory_order_relaxed);
        writePos.store(next, std::memory_order_release);
        return true;
    }

    static void raise(std::atomic<uint64_t>& mark, uint64_t value) {
        uint64_t current = mark.load(std::memory_order_relaxed);
        while (current < value
               && !mark.compare_exchange_weak(current, value, std::memory_order_release,
                                              std::memory_order_relaxed)) {}
    }

    NoteEvent ring[kSize];
    std::atomic<int> writePos{0};
    std::atomic<int> readPos{0};
    std::atomic_flag producerLock = ATOMIC_FLAG_INIT;

    std::atomic<uint64_t> lastSeq{0};          // latest note-on sequence number (any note)
    std::atomic<uint64_t> lastOnSeq[128] = {}; // latest note-on sequence number per note
    std::atomic<uint64_t> releasedUpTo[128] = {};
    std::atomic<uint64_t> allReleasedUpTo{0};
    // Sampler marks: latest key note-on / pad hit sequence number and the release marks.
    std::atomic<uint64_t> samplerLastOnSeq[128] = {};
    std::atomic<uint64_t> samplerReleasedUpTo[128] = {};
    std::atomic<uint64_t> samplerSlotLastSeq[kMaxSamplerSlot + 1] = {};
    std::atomic<uint64_t> samplerSlotReleasedUpTo[kMaxSamplerSlot + 1] = {};
    std::atomic<uint64_t> samplerSilencedUpTo{0};
};
