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
    Drum = 1, // a drum hit (`note` = drum voice 0–7); one-shot, never released, never arpeggiated
};

// A note-on (or drum hit) waiting for the audio thread.
struct NoteEvent {
    int note;               // 0–127, or the drum voice for kind Drum
    int velocity;
    uint64_t seq;           // NoteEventQueue sequence number (1, 2, …), compared with the release marks; 0 for a drum hit
    juce::int64 stampTicks; // juce::Time::getHighResolutionTicks() at enqueue
    juce::int64 touchTicks; // originating UITouch.timestamp in high-res ticks (same clock); 0 if not a touch
    NoteEventKind kind = NoteEventKind::Note;
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
    bool pushNoteOn(int note, int velocity, juce::int64 touchTicks = 0) {
        note = std::clamp(note, 0, 127);
        SpinGuard guard(producerLock);
        const int wp = writePos.load(std::memory_order_relaxed);
        const int next = (wp + 1) % kSize;
        if (next == readPos.load(std::memory_order_acquire)) {
            notesDropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        const uint64_t seq = lastSeq.load(std::memory_order_relaxed) + 1;
        ring[wp] = { note, velocity, seq, juce::Time::getHighResolutionTicks(), touchTicks, NoteEventKind::Note };
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
    static constexpr int kMaxDrumVoice = 7;
    bool pushDrumHit(int voice, int velocity, juce::int64 touchTicks = 0) {
        if (voice < 0 || voice > kMaxDrumVoice) return false;
        velocity = std::clamp(velocity, 1, 127);
        SpinGuard guard(producerLock);
        const int wp = writePos.load(std::memory_order_relaxed);
        const int next = (wp + 1) % kSize;
        if (next == readPos.load(std::memory_order_acquire)) {
            drumHitsDropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        ring[wp] = { voice, velocity, 0, juce::Time::getHighResolutionTicks(), touchTicks, NoteEventKind::Drum };
        drumHitsQueued.fetch_add(1, std::memory_order_relaxed);
        writePos.store(next, std::memory_order_release);
        return true;
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
            if (ring[rp].kind == NoteEventKind::Drum) drumHitsConsumed.fetch_add(1, std::memory_order_relaxed);
            else notesConsumed.fetch_add(1, std::memory_order_relaxed);
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

    // The allNotesOff mark (sequence number every note-on at or below is released by). The
    // consumer compares it with the value it last saw to detect a pushAllNotesOff, which also
    // clears the arpeggiator's latched list (a per-note release alone keeps it).
    uint64_t allReleasedMark() const { return allReleasedUpTo.load(std::memory_order_acquire); }

    // Drops queued note-ons (counted in notesDiscarded). Consumer side: call only while no audio
    // thread drains the queue (engine setup, before the device callback is attached).
    void discardPending() {
        int rp = readPos.load(std::memory_order_relaxed);
        const int wp = writePos.load(std::memory_order_acquire);
        uint32_t notes = 0, drums = 0;
        for (; rp != wp; rp = (rp + 1) % kSize) {
            if (ring[rp].kind == NoteEventKind::Drum) ++drums;
            else ++notes;
        }
        notesDiscarded.fetch_add(notes, std::memory_order_relaxed);
        drumHitsDiscarded.fetch_add(drums, std::memory_order_relaxed);
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
};
