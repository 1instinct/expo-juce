#pragma once

// Pure C++ (no Objective-C): the loop sequence player. Used by PolySynthProcessor (audio thread),
// ExpoJuceAudioEngine.mm (setSequence and the note log for JS) and the offline harness
// (test/harness, cases "seq", "seqv", "seqs").
//
// A sequence is a loop of `loopBeats` beats holding up to kMaxEvents events: a synth note (note
// 0–127, velocity 1–127, start and length in beats), a drum hit (voice 0–8; one-shot, no
// length), a sampler pad hit (slot 0–15, released after its length) or a sampler key note (note
// 0–127, on and then off after its length; 0.10.0-rc.2). While the transport plays, every event fires once per loop pass at the transport beat
// pass × loopBeats + start, on the exact sample of the block that beat falls in (the first sample
// at or after it), and a note's release follows at start + length. Beat positions come from the
// transport, block by block, so tempo changes need nothing special.
//
// Publishing (SequenceStore): the writer (any thread but the audio thread; writers serialised)
// builds the sorted event array into the slot the audio thread is not reading and publishes it
// with one atomic store. The audio thread marks the slot it reads for the length of a block
// (acquire / release, a hazard slot), so a writer never overwrites the array under it: a writer
// that picks the slot a block is still reading waits for that block to end (bounded, a block is a
// few ms; it gives up after kWriterWaitMs and returns false). No lock, no allocation and no wait
// on the audio thread.
//
// Release rules (SequencePlayer, audio thread): the player keeps a list of the notes it started
// (kMaxActive). A transport stop, a seek, a loop-length change, mute, a cleared sequence and the
// engine's allNotesOff release every one of them at the start of the block. When a new sequence
// is published while notes sound, a sounding note whose event (same start, note and kind) is still
// in the new sequence keeps sounding with the new event's length; the others are released. Notes
// of the same pitch are counted: the release of a note goes out only when its last sounding
// instance ends. Sampler events are not counted: each start carries a tag of its own (Output::tag)
// and its release names that tag, so it ends exactly the voices that start played. Whenever
// everything is released (stop, seek, loop length, mute, clear, allNotesOff) after the player has
// started a sampler event, a SamplerStop output goes out at offset 0 (after that block's releases,
// before any start): the synth then ends
// every sampler voice the sequence started, one-shots included (see PolySynthProcessor). A new
// sequence only releases (a one-shot whose event is gone plays on, as a drum hit does). A firing
// window carries from block to block ((previous end, this end]), so an
// event fires exactly once per pass even across tempo changes and chunked blocks.
//
// Bounded work per block: at most kMaxBlockStarts starts and kMaxPassesPerBlock loop passes (a
// loop is at least kMinLoopBeats long). Releases are never dropped: the output list has room for
// every release a block can make (kMaxOutputs), so a full block can lose starts, never note-offs.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>
#include <thread>

namespace seqplay {

enum class EventKind : uint8_t {
    Note = 0,       // a synth note: on at start, off at start + length
    Drum = 1,       // a drum hit (note = voice 0–8): one-shot
    SamplerHit = 2, // a sampler pad hit (note = slot 0–15): released at start + length
    SamplerKey = 3, // a sampler key note (note 0–127): on at start, off at start + length
    // Output only, never in a sequence: end every sampler voice the sequence started (offset 0).
    SamplerStop = 4,
};

constexpr int kMaxDrumVoice = 8;
constexpr int kMaxSamplerSlot = 15;

inline bool isSamplerKind(EventKind k) { return k == EventKind::SamplerHit || k == EventKind::SamplerKey; }
// Kinds with a length: the player holds them from their start to start + length.
inline bool isHeldKind(EventKind k) { return k == EventKind::Note || isSamplerKind(k); }

struct Event {
    double start = 0.0;  // beats inside the loop, 0 <= start < loopBeats
    double length = 0.0; // beats > 0 (notes); ignored for drums
    // Absolute transport beat: a firing of this event before it is skipped (the app already
    // sounded it, e.g. a note recorded with input quantize that snapped forward). -inf = none.
    double skipBefore = -std::numeric_limits<double>::infinity();
    uint8_t note = 0;
    uint8_t velocity = 100;
    EventKind kind = EventKind::Note;
};

constexpr int kMaxEvents = 8192;
// The shortest loop setSequence takes (a 1/64 note): shorter loops would make a block walk
// thousands of passes.
constexpr double kMinLoopBeats = 1.0 / 64.0;

// Why SequenceStore::set refused a whole sequence because of one event (0.10.0-rc.2): a kind the
// player does not know, a sampler pad hit's slot outside 0–15 or a sampler key note outside 0–127.
// Other invalid fields (a start outside the loop, a length <= 0, a synth note or drum voice out of
// range) only skip that event (sanitizeEvent), as before.
enum class Reject : uint8_t { None = 0, Kind = 1, SamplerSlot = 2, SamplerKeyNote = 3 };

inline Reject rejectReason(const Event& e) {
    switch (e.kind) {
        case EventKind::Note:
        case EventKind::Drum:
            return Reject::None;
        case EventKind::SamplerHit:
            return e.note > kMaxSamplerSlot ? Reject::SamplerSlot : Reject::None;
        case EventKind::SamplerKey:
            return e.note > 127 ? Reject::SamplerKeyNote : Reject::None;
        default:
            return Reject::Kind;
    }
}

inline const char* rejectText(Reject r) {
    switch (r) {
        case Reject::Kind: return "unknown kind";
        case Reject::SamplerSlot: return "samplerHit slot is not an integer 0-15";
        case Reject::SamplerKeyNote: return "samplerKey note is not an integer 0-127";
        default: return "";
    }
}

// Checks and clamps one event against loopBeats. False: the event can never play (start outside
// the loop, non-finite numbers, a note or sampler event without a positive length, a drum voice
// outside 0–8, a sampler slot outside 0–15, an unknown kind).
inline bool sanitizeEvent(Event& e, double loopBeats) {
    if (!std::isfinite(e.start) || e.start < 0.0 || !(e.start < loopBeats)) return false;
    if (e.kind == EventKind::Drum) {
        if (e.note > kMaxDrumVoice) return false;
        e.length = 0.0;
    } else {
        if (!isHeldKind(e.kind) || rejectReason(e) != Reject::None || e.note > 127) return false;
        if (!std::isfinite(e.length) || !(e.length > 0.0)) return false;
    }
    e.velocity = (uint8_t)std::clamp((int)e.velocity, 1, 127);
    if (std::isnan(e.skipBefore)) e.skipBefore = -std::numeric_limits<double>::infinity();
    return true;
}

// One published sequence.
struct Sequence {
    Event events[kMaxEvents]; // sorted by start (stable: equal starts keep the writer's order)
    int count = 0;
    double loopBeats = 0.0;
    // Absolute transport beats: nothing fires before fromBeat (a count-in) or at / after endBeat
    // (a single pass with the loop off). -inf / +inf = no bound.
    double fromBeat = -std::numeric_limits<double>::infinity();
    double endBeat = std::numeric_limits<double>::infinity();
    uint64_t version = 0;
};

// What setSequence reports.
struct SetResult {
    bool ok = false;  // false: too many events, a bad loop length, a rejected event, or the writer timed out
    int accepted = 0; // events published
    int dropped = 0;  // invalid events skipped (sanitizeEvent): they can never play
    int rejectedIndex = -1; // the first event that refused the whole sequence (rejectReason), -1 = none
    Reject reason = Reject::None;
};

// The process-lifetime store the engine keeps (heap-allocated, about 640 KB: two slots of
// kMaxEvents events). Writers: setSequence / setMuted / clear, any thread but the audio thread.
// Reader: one audio thread at a time (acquire / release around a block).
class SequenceStore {
public:
    static constexpr int kWriterWaitMs = 500;

    SequenceStore() = default;
    SequenceStore(const SequenceStore&) = delete;
    SequenceStore& operator=(const SequenceStore&) = delete;

    // Copies `count` events (any order), keeps those sanitizeEvent accepts, sorts them by start and
    // publishes. With events, loopBeats non-finite or under kMinLoopBeats, more than kMaxEvents
    // events, or an event rejectReason refuses (its index in rejectedIndex): nothing changes, ok =
    // false. count 0 publishes an empty sequence (stops what it plays). Every call sets what
    // lastRejectedEvent / lastReject report (one atomic: concurrent calls never mix theirs).
    SetResult set(const Event* in, int count, double loopBeats, double fromBeat = -std::numeric_limits<double>::infinity(),
                  double endBeat = std::numeric_limits<double>::infinity()) {
        const SetResult r = publish(in, count, loopBeats, fromBeat, endBeat);
        lastRejectPacked.store(r.rejectedIndex >= 0 ? (r.rejectedIndex << 4) | (int)r.reason : -1, std::memory_order_relaxed);
        return r;
    }

    // An empty sequence (stops everything the sequence plays).
    bool clear() { return set(nullptr, 0, 0.0).ok; }

    void setMuted(bool on) { mutedFlag.store(on, std::memory_order_release); }
    bool muted() const { return mutedFlag.load(std::memory_order_acquire); }

private:
    SetResult publish(const Event* in, int count, double loopBeats, double fromBeat, double endBeat) {
        SetResult r;
        if (count < 0 || count > kMaxEvents || (count > 0 && in == nullptr)) return r;
        if (count > 0 && !(std::isfinite(loopBeats) && loopBeats >= kMinLoopBeats)) return r;
        for (int i = 0; i < count; ++i) {
            const Reject why = rejectReason(in[i]);
            if (why != Reject::None) {
                r.rejectedIndex = i;
                r.reason = why;
                return r;
            }
        }
        if (!(loopBeats > 0.0) || !std::isfinite(loopBeats)) loopBeats = 0.0;
        if (std::isnan(fromBeat)) fromBeat = -std::numeric_limits<double>::infinity();
        if (std::isnan(endBeat)) endBeat = std::numeric_limits<double>::infinity();
        std::lock_guard<std::mutex> guard(writerLock);
        const int target = 1 - std::max(0, published.load(std::memory_order_acquire));
        if (!waitUntilNotReading(target)) {
            writerTimeouts.fetch_add(1, std::memory_order_relaxed);
            return r;
        }
        Sequence& s = slots[target];
        int n = 0;
        for (int i = 0; i < count; ++i) {
            Event e = in[i];
            if (sanitizeEvent(e, loopBeats)) s.events[n++] = e;
            else ++r.dropped;
        }
        std::stable_sort(s.events, s.events + n, [](const Event& a, const Event& b) { return a.start < b.start; });
        s.count = n;
        s.loopBeats = n > 0 ? loopBeats : (loopBeats > 0.0 ? loopBeats : 0.0);
        s.fromBeat = fromBeat;
        s.endBeat = endBeat;
        s.version = ++versionCount;
        published.store(target, std::memory_order_seq_cst);
        publishedCount.store(n, std::memory_order_relaxed);
        lastInvalid.store(r.dropped, std::memory_order_relaxed);
        publishedLoopBeats.store(s.loopBeats, std::memory_order_relaxed);
        r.ok = true;
        r.accepted = n;
        return r;
    }

public:

    // ── Audio thread ──

    // The latest sequence (nullptr before the first set), marked as read until release().
    const Sequence* acquire() {
        for (;;) {
            const int p = published.load(std::memory_order_seq_cst);
            if (p < 0) return nullptr;
            reading.store(p, std::memory_order_seq_cst);
            if (published.load(std::memory_order_seq_cst) == p) return &slots[p];
        }
    }
    void release() { reading.store(-1, std::memory_order_seq_cst); }

    // ── Counters (written by the audio thread or a writer, read anywhere; relaxed) ──
    std::atomic<uint64_t> notesFired{0};  // note-ons, drum hits and sampler events the player started
    std::atomic<uint64_t> lateFires{0};   // fired more than one sample after their beat
    // Firings lost since launch (cumulative): a start over a block's kMaxBlockStarts, a note over
    // kMaxActive sounding, or passes over kMaxPassesPerBlock. Never a release.
    std::atomic<uint64_t> dropped{0};
    std::atomic<int> activeNotes{0};      // synth notes and sampler events the player holds
    std::atomic<bool> firing{false};      // the last block played the sequence (transport on, not muted, events)
    std::atomic<uint64_t> writerTimeouts{0};

    int eventCount() const { return publishedCount.load(std::memory_order_relaxed); }
    // Invalid events the last successful set skipped (SetResult::dropped).
    int lastInvalidCount() const { return lastInvalid.load(std::memory_order_relaxed); }
    double loopBeats() const { return publishedLoopBeats.load(std::memory_order_relaxed); }
    uint64_t version() const { return versionCount.load(std::memory_order_relaxed); }
    // The event the last set() refused the whole sequence for, and why; -1 / None when the last
    // set() published or failed for another reason (loop length, too many events, writer timeout).
    int lastRejectedEvent() const {
        const int p = lastRejectPacked.load(std::memory_order_relaxed);
        return p < 0 ? -1 : p >> 4;
    }
    Reject lastReject() const {
        const int p = lastRejectPacked.load(std::memory_order_relaxed);
        return p < 0 ? Reject::None : (Reject)(p & 15);
    }

private:
    bool waitUntilNotReading(int slot) {
        if (reading.load(std::memory_order_seq_cst) != slot) return true;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kWriterWaitMs);
        while (reading.load(std::memory_order_seq_cst) == slot) {
            if (std::chrono::steady_clock::now() > deadline) return false;
            std::this_thread::yield();
        }
        return true;
    }

    Sequence slots[2];
    std::atomic<int> published{-1};
    std::atomic<int> reading{-1};
    std::atomic<bool> mutedFlag{false};
    std::atomic<uint64_t> versionCount{0};
    std::atomic<int> publishedCount{0};
    std::atomic<int> lastInvalid{0};
    std::atomic<double> publishedLoopBeats{0.0};
    std::atomic<int> lastRejectPacked{-1}; // (index << 4) | Reject, or -1
    std::mutex writerLock;
};

// What the player hands the synth for one block (sample offsets ascending; at an equal offset a
// release comes before a start).
struct Output {
    int offset = 0;
    uint8_t note = 0;
    uint8_t velocity = 0;
    EventKind kind = EventKind::Note;
    bool on = false;
    double atBeat = 0.0; // the transport beat it belongs to (a release on a stop / jump: the block start)
    // Sampler events: the start's tag (never 0); its release carries the same tag, and the synth
    // releases only the voices started with it. 0 for notes, drums and SamplerStop.
    uint32_t tag = 0;
};

// Notes the player fired, for the app's JS side (onSequenceNotes): one producer (the audio
// thread), one consumer (the engine's drain timer). Recorded only while `listening`.
class NoteLog {
public:
    static constexpr int kCapacity = 1024;
    struct Entry { double atBeat; uint8_t note; EventKind kind; bool on; };

    std::atomic<bool> listening{false};
    std::atomic<uint64_t> lost{0}; // entries dropped on a full ring

    void push(const Output& o) {
        if (o.kind == EventKind::SamplerStop) return; // internal: not a note of the loop
        if (!listening.load(std::memory_order_relaxed)) return;
        const int wp = writePos.load(std::memory_order_relaxed);
        const int next = (wp + 1) % kCapacity;
        if (next == readPos.load(std::memory_order_acquire)) { lost.fetch_add(1, std::memory_order_relaxed); return; }
        ring[wp] = { o.atBeat, o.note, o.kind, o.on };
        writePos.store(next, std::memory_order_release);
    }

    template <typename F>
    int drain(F&& fn) {
        int rp = readPos.load(std::memory_order_relaxed);
        const int wp = writePos.load(std::memory_order_acquire);
        int n = 0;
        while (rp != wp) {
            fn(ring[rp]);
            rp = (rp + 1) % kCapacity;
            ++n;
        }
        readPos.store(rp, std::memory_order_release);
        return n;
    }

private:
    Entry ring[kCapacity];
    std::atomic<int> writePos{0};
    std::atomic<int> readPos{0};
};

// The audio-thread half: one per synth (a rebuild starts a new one with nothing sounding).
class SequencePlayer {
public:
    static constexpr int kMaxActive = 256;       // synth notes and sampler events held at once
    static constexpr int kMaxBlockStarts = 512;  // note-ons, drum hits and sampler events in one block
    static constexpr int kMaxPassesPerBlock = 1024;
    // Room for every output one block can make: its starts, a release for each note or sampler
    // event held at the block start or started in it, and one SamplerStop. So a release is never
    // dropped.
    static constexpr int kMaxOutputs = kMaxBlockStarts + kMaxActive + kMaxBlockStarts + 1;
    // Tolerance, in samples, for a beat that lands on a sample boundary up to rounding.
    static constexpr double kEpsSamples = 1.0e-6;

    void setStore(SequenceStore* s) { store = s; }
    SequenceStore* getStore() const { return store; }

    // Releases everything at the start of the next block (the engine's allNotesOff).
    void requestReleaseAll() { releaseAllRequested = true; }

    // Audio thread, once per block, before the synth renders. Hands this block's outputs to
    // sink(const Output&) in order. blockStartBeat / bpm / playing / seeked describe the block
    // (TransportEngine::getBlockStartBeat etc.).
    template <typename Sink>
    void process(double blockStartBeat, double bpm, bool playing, bool seeked, double sampleRate, int numSamples,
                 Sink&& sink) {
        outCount = 0;
        startsEmitted = 0;
        if (store == nullptr || numSamples <= 0 || !(sampleRate > 0.0)) {
            flushOutputs(sink);
            return;
        }
        const Sequence* seq = store->acquire();
        const bool muted = store->muted();
        const double loopBeats = seq != nullptr ? seq->loopBeats : 0.0;
        const bool hasEvents = seq != nullptr && seq->count > 0 && loopBeats > 0.0;
        const double b0 = blockStartBeat;

        // Everything stops: transport stopped or jumped, mute, loop length changed, nothing to
        // play, the engine's allNotesOff.
        const bool loopChanged = loopBeats != lastLoopBeats;
        if (releaseAllRequested || !playing || seeked || muted || loopChanged || !hasEvents) {
            releaseAll(b0);
            releaseAllRequested = false;
        }
        lastLoopBeats = loopBeats;

        // A new sequence: sounding notes whose event is gone are released, the others take the
        // new length.
        const uint64_t version = seq != nullptr ? seq->version : 0;
        if (version != lastVersion) {
            reconcile(seq, b0);
            lastVersion = version;
        }

        const bool fire = playing && !muted && hasEvents;
        if (fire) {
            const double bps = bpm / (60.0 * sampleRate); // beats per sample, as the transport advances
            const double lateBps = std::max(bps, lastBps);
            // The window carries on from the last block (contiguous whatever the tempo did); it
            // starts over at this block's first sample on play, a jump, or a position the last
            // window does not join.
            if (!wasFiring || seeked || windowEnd < b0 - 2.0 * lateBps || windowEnd > b0 + (double)numSamples * bps)
                windowEnd = b0 - kEpsSamples * bps;
            const double hi = b0 + ((double)(numSamples - 1) + kEpsSamples) * bps;
            collectStarts(*seq, windowEnd, hi);
            runBlock(b0, bps, lateBps, hi, numSamples);
            windowEnd = hi;
            lastBps = bps;
        }
        wasFiring = fire;
        store->firing.store(fire, std::memory_order_relaxed);
        store->activeNotes.store(activeCount, std::memory_order_relaxed);
        store->release();
        flushOutputs(sink);
    }

    int activeNoteCount() const { return activeCount; }

private:
    struct Active {
        double onBeat; // absolute beat of its start
        double end;    // absolute beat of its release
        double start;  // its event's start in the loop (identity across a new sequence, with kind and note)
        int minOffset; // started in this block at minOffset - 1: not released before minOffset
        uint32_t tag;  // sampler events: the start's tag; 0 for notes
        uint8_t note;
        EventKind kind;
    };
    struct Start { double t; const Event* event; };

    static int offsetFor(double beat, double b0, double bps, int numSamples) {
        const double x = (beat - b0) / bps - kEpsSamples;
        const double k = std::ceil(x);
        if (!(k > 0.0)) return 0;
        return k >= (double)(numSamples - 1) ? numSamples - 1 : (int)k;
    }

    template <typename Sink>
    void flushOutputs(Sink& sink) {
        // Insertion sort by offset, a release before a start at the same offset (n is small).
        for (int i = 1; i < outCount; ++i) {
            const Output o = outputs[i];
            int j = i - 1;
            while (j >= 0 && (outputs[j].offset > o.offset || (outputs[j].offset == o.offset && outputs[j].on && !o.on))) {
                outputs[j + 1] = outputs[j];
                --j;
            }
            outputs[j + 1] = o;
        }
        for (int i = 0; i < outCount; ++i) sink(outputs[i]);
        outCount = 0;
    }

    // A start goes out only while the block has made fewer than kMaxBlockStarts.
    bool emitStart(const Output& o) {
        if (startsEmitted >= kMaxBlockStarts || outCount >= kMaxOutputs) {
            if (store) store->dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        ++startsEmitted;
        outputs[outCount++] = o;
        return true;
    }
    // Always room (kMaxOutputs); the check only guards the array.
    void emitRelease(const Output& o) {
        if (outCount < kMaxOutputs) outputs[outCount++] = o;
    }

    bool stillSounding(uint8_t note) const {
        for (int i = 0; i < activeCount; ++i)
            if (active[i].kind == EventKind::Note && active[i].note == note) return true;
        return false;
    }

    // Ends active[i] (swap-remove) and releases it: a synth note unless another instance of its
    // pitch still sounds, a sampler event always (by its tag).
    void endActive(int i, int offset, double atBeat) {
        const Active a = active[i];
        active[i] = active[--activeCount];
        if (a.kind == EventKind::Note && stillSounding(a.note)) return;
        Output o;
        o.offset = offset;
        o.note = a.note;
        o.kind = a.kind;
        o.on = false;
        o.atBeat = atBeat;
        o.tag = a.tag;
        emitRelease(o);
    }

    void releaseAll(double atBeat) {
        while (activeCount > 0) endActive(activeCount - 1, 0, atBeat);
        if (samplerStarted) {
            // One-shots the sequence started (released or not) and anything else it left on the sampler.
            samplerStarted = false;
            Output o;
            o.offset = 0;
            o.kind = EventKind::SamplerStop;
            o.on = false;
            o.atBeat = atBeat;
            emitRelease(o);
        }
    }

    void reconcile(const Sequence* seq, double b0) {
        for (int i = 0; i < activeCount;) {
            const Event* match = seq != nullptr ? findEvent(*seq, active[i].start, active[i].kind, active[i].note) : nullptr;
            if (match != nullptr) {
                active[i].end = active[i].onBeat + match->length;
                ++i;
                continue;
            }
            endActive(i, 0, b0);
        }
    }

    // The event of `kind` at `start` playing `note`, if any (events are sorted by start).
    static const Event* findEvent(const Sequence& seq, double start, EventKind kind, uint8_t note) {
        const Event* first = seq.events;
        const Event* last = seq.events + seq.count;
        const Event* it = std::lower_bound(first, last, start, [](const Event& e, double s) { return e.start < s; });
        for (; it != last && it->start == start; ++it)
            if (it->kind == kind && it->note == note) return it;
        return nullptr;
    }

    // This block's firings: every event whose beat is in (lo, hi], pass by pass, ascending.
    void collectStarts(const Sequence& seq, double lo, double hi) {
        startCount = 0;
        const double L = seq.loopBeats;
        const double from = std::max(lo, std::nextafter(seq.fromBeat, -std::numeric_limits<double>::infinity()));
        if (!(hi > from)) return;
        double firstPass = std::floor(from / L);
        const double lastPass = std::floor(hi / L);
        if (!std::isfinite(firstPass) || !std::isfinite(lastPass)) return;
        // A block spanning more passes than kMaxPassesPerBlock plays only the last ones.
        if (lastPass - firstPass >= (double)kMaxPassesPerBlock) {
            if (store) store->dropped.fetch_add(1, std::memory_order_relaxed);
            firstPass = lastPass - (double)(kMaxPassesPerBlock - 1);
        }
        const int passes = (int)(lastPass - firstPass) + 1;
        const Event* first = seq.events;
        const Event* last = seq.events + seq.count;
        // An integer counter: past 2^53 beats, pass + 1.0 would not move.
        for (int p = 0; p < passes; ++p) {
            const double base = (firstPass + (double)p) * L;
            // Events with base + start in (from, hi].
            const Event* it = std::upper_bound(first, last, from - base, [](double s, const Event& e) { return s < e.start; });
            for (; it != last; ++it) {
                const double t = base + it->start;
                if (t > hi) break;
                if (t <= from || t < seq.fromBeat || t >= seq.endBeat || t < it->skipBefore) continue;
                if (startCount >= kMaxBlockStarts) {
                    if (store) store->dropped.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                starts[startCount++] = { t, it };
            }
        }
    }

    // The offset a release of active[i] is due at in this block, or -1 if it is not due yet.
    int releaseOffset(int i, double b0, double bps, double hi, int numSamples) const {
        if (!(active[i].end <= hi)) return -1;
        const int k = std::max(offsetFor(active[i].end, b0, bps, numSamples), active[i].minOffset);
        return k <= numSamples - 1 ? k : -1;
    }

    // This block's starts and due releases in time order (at an equal offset the release first),
    // so the count of sounding instances of a pitch is right at every point of the block: a
    // release goes out only if no later-started instance of its pitch is sounding by then.
    void runBlock(double b0, double bps, double lateBps, double hi, int numSamples) {
        int si = 0;
        for (;;) {
            int ri = -1, rk = numSamples;
            double rt = std::numeric_limits<double>::infinity();
            for (int i = 0; i < activeCount; ++i) {
                const int k = releaseOffset(i, b0, bps, hi, numSamples);
                if (k < 0) continue;
                if (k < rk || (k == rk && active[i].end < rt)) { ri = i; rk = k; rt = active[i].end; }
            }
            const bool haveStart = si < startCount;
            if (ri < 0 && !haveStart) break;
            if (ri >= 0 && (!haveStart || rk <= offsetFor(starts[si].t, b0, bps, numSamples))) {
                endActive(ri, rk, rt);
                continue;
            }
            start(*starts[si].event, starts[si].t, b0, bps, lateBps, numSamples);
            ++si;
        }
        for (int i = 0; i < activeCount; ++i) active[i].minOffset = 0;
    }

    void start(const Event& e, double t, double b0, double bps, double lateBps, int numSamples) {
        const int offset = offsetFor(t, b0, bps, numSamples);
        if (t < b0 - lateBps && store) store->lateFires.fetch_add(1, std::memory_order_relaxed);
        Output o;
        o.offset = offset;
        o.note = e.note;
        o.velocity = e.velocity;
        o.kind = e.kind;
        o.on = true;
        o.atBeat = t;
        if (e.kind == EventKind::Drum) {
            if (emitStart(o) && store) store->notesFired.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (activeCount >= kMaxActive) {
            if (store) store->dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (isSamplerKind(e.kind)) {
            if (++tagCounter == 0) ++tagCounter; // 0 = no tag
            o.tag = tagCounter;
        }
        if (!emitStart(o)) return;
        if (store) store->notesFired.fetch_add(1, std::memory_order_relaxed);
        if (isSamplerKind(e.kind)) samplerStarted = true;
        // Its release can come later in this block (after its start) or in a later block.
        active[activeCount++] = { t, t + e.length, e.start, offset + 1, o.tag, e.note, e.kind };
    }

    SequenceStore* store = nullptr;
    Active active[kMaxActive];
    int activeCount = 0;
    Output outputs[kMaxOutputs];
    int outCount = 0;
    int startsEmitted = 0;
    Start starts[kMaxBlockStarts];
    int startCount = 0;
    double windowEnd = 0.0;
    double lastLoopBeats = 0.0;
    double lastBps = 0.0;
    uint64_t lastVersion = 0;
    uint32_t tagCounter = 0;
    bool wasFiring = false;
    bool releaseAllRequested = false;
    bool samplerStarted = false; // a sampler event started since the last SamplerStop
};

} // namespace seqplay
