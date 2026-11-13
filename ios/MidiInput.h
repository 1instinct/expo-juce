#pragma once

// Pure C++ (no Objective-C): MIDI byte input that never passes through JavaScript. Used by
// ExpoJuceAudioEngine.mm (behind the C entry point expojuce_midi_input_bytes, see
// ExpoJuceMidiInput.h) and the offline harness (test/harness, cases "mip", "mim", "mih", "mic").
//
// Bytes arrive from the app's own native code (a Bluetooth notification callback, a serial port)
// on any thread. They are parsed here, mapped by an input map the app pushes from JS, and a mapped
// note-on goes straight onto the engine's note queue. Every parsed message is also recorded as an
// Event for the app's JS side (logging, learning, recording), which drains them in batches.
//
// Parsers (one state per format, carried across calls, so a message split between packets
// completes on the next one):
//  - kFormatBleMidi: one BLE-MIDI packet per call. A header byte (0b10tttttt), then a
//    timestamp-low byte (0b1ttttttt) before each status byte; running status may follow with or
//    without its own timestamp byte. Timestamps are dropped (messages play on arrival). A packet
//    without a valid header yields nothing.
//  - kFormatRawStream: raw MIDI bytes, any split. Parsing starts at the first status byte.
//  - kFormatRawPackets: raw MIDI where each call must be complete MIDI on its own: it starts with
//    a status byte and holds nothing but whole channel voice messages (running status and
//    real-time bytes allowed). Anything else (text a serial bridge interleaves, a truncated
//    message, SysEx) is rejected whole and clears running status, so text can never play a note.
//    The complete note-offs in a rejected packet (8n kk vv, 9n kk 00) still release the input's
//    holds (salvageNoteOffs), so a note-off that shared a packet with text never leaves a note on.
// All three: real-time bytes (F8–FF) are ignored anywhere and keep running status; SysEx is
// skipped until F7 or any other status; system common clears running status and its data bytes
// are skipped; a note-on with velocity 0 is a note-off; program change and channel pressure are
// parsed and not reported.
//
// Input map (InputMap): for each of the 128 input notes an action (play synth note n, hit drum
// voice v, or pass to the app), plus a 128-entry velocity curve. Writers (any thread, serialised)
// swap it whole; the reading thread never takes a lock (a sequence-count check per read, retried
// only while a writer is mid-write on the slot it picked). `enabled` is a separate flag: off, every
// note-on passes to the app.
//
// Holds: an input note-on that plays synth note s is remembered (input note → s, and a count of
// input holds per s), so its note-off releases exactly s whatever the map says by then. A note-off
// sends the engine's release for s only when the last input hold of s ends and no other source of
// the engine holds s (externalNoteOn / externalNoteOff mirror the engine's own noteOn / noteOff:
// the engine's release of a note stops every voice of it, so a note another source still holds
// is left to that source's release). A second note-on of an input note without its note-off
// releases the first (an Event with kSynthetic). releaseAll() ends every input hold (disconnect);
// the engine's allNotesOff forgets them under the same lock as its queue release.
//
// Threads: input(), releaseAll(), allNotesOff() and the hold bookkeeping run under one lock
// among producers (InputLock: os_unfair_lock on Apple) (a few hundred stores at most per call; never taken by the audio thread). The
// map and the counters are atomics. Events go into a fixed ring (producers under the lock, one
// consumer: drainEvents). No allocation anywhere after construction.

#include "NoteEventQueue.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#if defined(__APPLE__)
#include <os/lock.h>
#endif

namespace midiin {

enum Format : int {
    kFormatBleMidi = 0,
    kFormatRawStream = 1,
    kFormatRawPackets = 2,
};
constexpr int kFormatCount = 3;

// Message types are the status nibbles.
enum MessageType : uint8_t {
    kNoteOff = 0x80,
    kNoteOn = 0x90,
    kPolyPressure = 0xA0,
    kControlChange = 0xB0,
    kPitchBend = 0xE0,
};

// One complete channel voice message. Note on / off: data1 = note, data2 = velocity (0 for a
// note-on with velocity 0). Poly pressure: data1 = note, data2 = pressure. Control change: data1 =
// controller, data2 = value. Pitch bend: value = 0–16383 (8192 = centre).
struct Message {
    uint8_t type = 0;
    uint8_t channel = 0;
    uint8_t data1 = 0;
    uint8_t data2 = 0;
    uint16_t value = 0;
};

struct ParserState {
    uint8_t status = 0; // running status (a channel voice status byte), 0 = none
    uint8_t data0 = 0;  // first data byte of the message in progress
    uint8_t count = 0;  // data bytes received for it
    bool sysex = false; // inside a SysEx (dropped until F7 or any other status)
    uint8_t skip = 0;   // data bytes of a system common message still to skip
    void reset() { status = 0; data0 = 0; count = 0; sysex = false; skip = 0; }
};

template <typename Out>
inline void emitMessage(uint8_t status, uint8_t d0, uint8_t d1, Out&& out) {
    Message m;
    m.channel = (uint8_t)(status & 0x0F);
    m.data1 = d0;
    m.data2 = d1;
    switch (status & 0xF0) {
        case 0x90: m.type = d1 > 0 ? kNoteOn : kNoteOff; break;
        case 0x80: m.type = kNoteOff; break;
        case 0xA0: m.type = kPolyPressure; break;
        case 0xB0: m.type = kControlChange; m.value = d1; break;
        case 0xE0: m.type = kPitchBend; m.value = (uint16_t)(d0 | (d1 << 7)); break;
        default: return; // program change and channel pressure: not reported
    }
    out(m);
}

// One MIDI byte through the state machine; complete messages go to `out`.
template <typename Out>
inline void feed(ParserState& s, uint8_t b, Out&& out) {
    if (b >= 0xF8) return; // real-time: anywhere, never breaks running status
    if (b >= 0x80) {
        s.count = 0;
        s.skip = 0;
        if (b == 0xF0) {
            s.sysex = true;
            s.status = 0;
            return;
        }
        s.sysex = false;
        if (b >= 0xF0) {
            // System common (F7 ends a SysEx): clears running status, its data bytes are skipped.
            s.status = 0;
            s.skip = b == 0xF2 ? 2 : (b == 0xF1 || b == 0xF3) ? 1 : 0;
            return;
        }
        s.status = b;
        return;
    }
    if (s.sysex) return;
    if (s.skip > 0) {
        s.skip -= 1;
        return;
    }
    if (s.status == 0) return; // stray data byte
    const uint8_t hi = s.status & 0xF0;
    if (hi == 0xC0 || hi == 0xD0) {
        s.count = 0; // one data byte; not reported
        return;
    }
    if (s.count == 0) {
        s.data0 = b;
        s.count = 1;
        return;
    }
    s.count = 0;
    emitMessage(s.status, s.data0, b, out);
}

template <typename Out>
inline void parseRawStream(const uint8_t* bytes, size_t length, ParserState& s, Out&& out) {
    for (size_t i = 0; i < length; ++i) feed(s, bytes[i], out);
}

// Data bytes a channel voice status takes (0 for anything else).
inline int dataBytesOf(uint8_t status) {
    const uint8_t hi = status & 0xF0;
    if (hi == 0xC0 || hi == 0xD0) return 1;
    return (status >= 0x80 && status < 0xF0) ? 2 : 0;
}

// True when the bytes are nothing but complete channel voice messages, starting with a status
// byte (running status and real-time bytes allowed).
inline bool isCompleteMidi(const uint8_t* bytes, size_t length) {
    if (length == 0 || bytes[0] < 0x80) return false;
    int need = 0, left = 0;
    for (size_t i = 0; i < length; ++i) {
        const uint8_t b = bytes[i];
        if (b >= 0xF8) continue;
        if (b >= 0x80) {
            if (left != 0) return false; // status in the middle of a message
            need = dataBytesOf(b);
            if (need == 0) return false; // SysEx / system common
            left = need;
            continue;
        }
        if (need == 0) return false;
        if (left == 0) left = need; // running status
        left -= 1;
    }
    return left == 0 && need != 0;
}

// kFormatRawPackets: resets the state, then parses the packet only if it is complete MIDI.
// Returns false (nothing reported) otherwise.
template <typename Out>
inline bool parseRawPacket(const uint8_t* bytes, size_t length, ParserState& s, Out&& out) {
    s.reset();
    if (!isCompleteMidi(bytes, length)) return false;
    parseRawStream(bytes, length, s, out);
    return true;
}

// A rejected raw packet's complete note-offs: a status 8n, or 9n with velocity 0, followed by two
// data bytes. Nothing else (a note-on can never come from a packet that is not complete MIDI). Text
// is 7-bit, so it never holds a status byte; a stray UTF-8 continuation byte can read as a note-off,
// which at worst releases a note the input holds.
template <typename Out>
inline void salvageNoteOffs(const uint8_t* bytes, size_t length, Out&& out) {
    for (size_t i = 0; i + 2 < length; ++i) {
        const uint8_t st = bytes[i];
        const uint8_t hi = st & 0xF0;
        if (hi != 0x80 && hi != 0x90) continue;
        const uint8_t note = bytes[i + 1], velocity = bytes[i + 2];
        if (note >= 0x80 || velocity >= 0x80) continue;
        if (hi == 0x90 && velocity != 0) continue;
        Message m;
        m.type = kNoteOff;
        m.channel = (uint8_t)(st & 0x0F);
        m.data1 = note;
        m.data2 = hi == 0x80 ? velocity : 0;
        out(m);
        i += 2;
    }
}

// kFormatBleMidi: one BLE-MIDI packet.
template <typename Out>
inline void parseBleMidiPacket(const uint8_t* bytes, size_t length, ParserState& s, Out&& out) {
    if (length < 2 || (bytes[0] & 0xC0) != 0x80) return;
    size_t i = 1;
    while (i < length) {
        const uint8_t b = bytes[i];
        if (b & 0x80) {
            // Timestamp-low byte. A status byte may follow; a data byte means running status.
            i += 1;
            if (i >= length) break;
            const uint8_t next = bytes[i];
            if (next & 0x80) {
                feed(s, next, out);
                i += 1;
            }
            continue;
        }
        feed(s, b, out);
        i += 1;
    }
}

// The producers' lock. Never taken on the audio thread. On Apple platforms an os_unfair_lock:
// it donates the waiter's priority to the holder (a Bluetooth queue holding it while a
// user-interactive thread waits), where a spinlock would spin against a preempted holder. The
// spinlock (test_and_set acquire / clear release, yielding) is for other hosts (the harness).
#if defined(__APPLE__)
class InputLock {
public:
    void lock() noexcept { os_unfair_lock_lock(&l); }
    void unlock() noexcept { os_unfair_lock_unlock(&l); }
private:
    os_unfair_lock l = OS_UNFAIR_LOCK_INIT;
};
#else
class InputLock {
public:
    void lock() noexcept { while (flag.test_and_set(std::memory_order_acquire)) std::this_thread::yield(); }
    void unlock() noexcept { flag.clear(std::memory_order_release); }
private:
    std::atomic_flag flag = ATOMIC_FLAG_INIT;
};
#endif

struct LockScope {
    explicit LockScope(InputLock& l) : lock(l) { lock.lock(); }
    ~LockScope() { lock.unlock(); }
    LockScope(const LockScope&) = delete;
    LockScope& operator=(const LockScope&) = delete;
    InputLock& lock;
};

// ── Input map ────────────────────────────────────────────────────

// Action per input note: kPass, a synth note 0–127, or kDrumBase + drum voice (0–kMaxDrumVoice).
class InputMap {
public:
    static constexpr int16_t kPass = -1;
    static constexpr int16_t kDrumBase = 256;
    static constexpr int kMaxDrumVoice = NoteEventQueue::kMaxDrumVoice;

    struct Entry {
        int16_t action = kPass;
        uint8_t velocity = 1; // the curve's value for the message's velocity (1–127)
    };

    InputMap() {
        for (Slot& slot : slots) {
            for (int i = 0; i < 128; ++i) {
                slot.action[i].store(kPass, std::memory_order_relaxed);
                slot.velocity[i].store((uint8_t)(i < 1 ? 1 : i), std::memory_order_relaxed);
            }
        }
    }

    // A valid action, or kPass.
    static int16_t sanitizeAction(int value) {
        if (value >= 0 && value <= 127) return (int16_t)value;
        if (value >= kDrumBase && value <= kDrumBase + kMaxDrumVoice) return (int16_t)value;
        return kPass;
    }

    // Any thread (writers serialised). actions[128]: per input note (sanitised); velocity[128]:
    // input velocity → played velocity, clamped to 1–127 (nullptr = identity). Readers see either
    // the old map or the new one, never a mix.
    void set(const int* actions, const int* velocity) {
        LockScope guard(writerLock);
        const int next = 1 - current.load(std::memory_order_relaxed);
        Slot& slot = slots[next];
        const uint32_t g = slot.gen.load(std::memory_order_relaxed);
        slot.gen.store(g + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        for (int i = 0; i < 128; ++i) {
            slot.action[i].store(actions ? sanitizeAction(actions[i]) : kPass, std::memory_order_relaxed);
            int v = velocity ? velocity[i] : i;
            v = v < 1 ? 1 : v > 127 ? 127 : v;
            slot.velocity[i].store((uint8_t)v, std::memory_order_relaxed);
        }
        slot.gen.store(g + 2, std::memory_order_release);
        current.store(next, std::memory_order_release);
        versionCount.fetch_add(1, std::memory_order_relaxed);
    }

    // Any thread, lock-free. note / velocity 0–127.
    Entry read(int note, int velocity) const {
        note &= 127;
        velocity &= 127;
        for (;;) {
            const Slot& slot = slots[current.load(std::memory_order_acquire)];
            const uint32_t g1 = slot.gen.load(std::memory_order_acquire);
            if (g1 & 1u) continue; // a writer is on this slot (it was swapped out twice meanwhile)
            Entry e;
            e.action = slot.action[note].load(std::memory_order_relaxed);
            e.velocity = slot.velocity[velocity].load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (slot.gen.load(std::memory_order_relaxed) == g1) return e;
        }
    }

    // Maps set so far.
    uint32_t version() const { return versionCount.load(std::memory_order_relaxed); }

private:
    struct Slot {
        std::atomic<uint32_t> gen{0}; // odd while a writer is writing it
        std::atomic<int16_t> action[128];
        std::atomic<uint8_t> velocity[128];
    };
    Slot slots[2];
    std::atomic<int> current{0};
    std::atomic<uint32_t> versionCount{0};
    InputLock writerLock;
};

// ── Where mapped messages go ─────────────────────────────────────

class MidiInputSink {
public:
    virtual ~MidiInputSink() = default;
    // false: not played (engine not ready, queue full); the message then passes to the app.
    virtual bool noteOn(int note, int velocity, double arrivalSeconds) = 0;
    virtual bool drumHit(int voice, int velocity, double arrivalSeconds) = 0;
    virtual void noteOff(int note) = 0;
    // Releases every note-on queued so far (the engine's allNotesOff).
    virtual void allNotesOff() = 0;
};

// The engine's sink: the process-lifetime NoteEventQueue, accepting note-ons only while
// `accepting` is true (nullptr = always). Arrival seconds are on the juce::Time high-resolution
// clock (mach_absolute_time on Apple = CACurrentMediaTime()); the queue records them for the
// arrival → render latency leg.
class NoteQueueSink : public MidiInputSink {
public:
    NoteQueueSink(NoteEventQueue& q, const std::atomic<bool>* acceptingFlag) : queue(q), accepting(acceptingFlag) {}

    static juce::int64 ticksFor(double seconds) {
        return seconds > 0.0 ? juce::Time::secondsToHighResolutionTicks(seconds) : (juce::int64)0;
    }
    static double nowSeconds() {
        return juce::Time::highResolutionTicksToSeconds(juce::Time::getHighResolutionTicks());
    }

    bool noteOn(int note, int velocity, double arrivalSeconds) override {
        if (accepting && !accepting->load(std::memory_order_acquire)) return false;
        return queue.pushNoteOn(note, velocity, 0, 0, ticksFor(arrivalSeconds));
    }
    bool drumHit(int voice, int velocity, double arrivalSeconds) override {
        if (accepting && !accepting->load(std::memory_order_acquire)) return false;
        return queue.pushDrumHit(voice, velocity, 0, 0, ticksFor(arrivalSeconds));
    }
    void noteOff(int note) override { queue.pushNoteOff(note); }
    void allNotesOff() override { queue.pushAllNotesOff(); }

private:
    NoteEventQueue& queue;
    const std::atomic<bool>* accepting;
};

// ── Events for the app ───────────────────────────────────────────

enum EventFlags : uint8_t {
    kEventPlayed = 1,    // handled here: a note-on played (out / outVelocity), a note-off released its input hold
    kEventLast = 2,      // note-off: that was the last input hold of `out`
    kEventDrum = 4,      // `out` is a drum voice
    kEventSynthetic = 8, // a note-off made here: a second note-on of the same input note released the first
};

struct Event {
    double arrival = 0.0; // seconds, the arrival clock (see NoteQueueSink)
    uint32_t epoch = 0;   // MidiInput::epoch() when it was handled (releaseAll / allNotesOff bump it)
    Message message;
    uint8_t flags = 0;
    int16_t out = -1;     // synth note (or drum voice) the message played or released; -1 = none
    uint8_t outVelocity = 0;
};

struct Stats {
    uint64_t packets = 0;         // input() calls with a valid format
    uint64_t messages = 0;        // complete messages parsed
    uint64_t played = 0;          // note-ons played here (synth and drum)
    uint64_t passed = 0;          // note-ons passed to the app (disabled, unmapped)
    uint64_t dropped = 0;         // note-ons the map played that the engine refused (not ready, queue full)
    uint64_t rejectedPackets = 0; // kFormatRawPackets calls that were not complete MIDI
    uint64_t eventsDropped = 0;   // events lost to a full ring (the app was not draining)
    int held = 0;                 // input notes holding a synth note now
};

// ── The input ────────────────────────────────────────────────────

class MidiInput {
public:
    static constexpr int kEventCapacity = 512;

    explicit MidiInput(MidiInputSink& s) : sink(s) {
        for (int i = 0; i < 128; ++i) {
            heldBy[i] = -1;
            holdCount[i] = 0;
            external[i].store(false, std::memory_order_relaxed);
        }
    }
    MidiInput(const MidiInput&) = delete;
    MidiInput& operator=(const MidiInput&) = delete;

    InputMap& map() { return inputMap; }
    const InputMap& map() const { return inputMap; }
    void setEnabled(bool on) { enabledFlag.store(on, std::memory_order_release); }
    bool enabled() const { return enabledFlag.load(std::memory_order_acquire); }

    // Any thread. Parses `bytes` in `format`, plays what the map says, records an Event per
    // message. Returns the number of messages parsed, or -1 for an unknown format / null bytes.
    // *flushNeeded (optional) is set when the caller should schedule a drain: the first event
    // recorded since the last beginDrain().
    int input(const uint8_t* bytes, size_t length, int format, double arrivalSeconds, bool* flushNeeded = nullptr) {
        if (flushNeeded) *flushNeeded = false;
        if (format < 0 || format >= kFormatCount || (bytes == nullptr && length > 0)) return -1;
        int parsed = 0;
        bool recorded = false;
        {
            LockScope guard(lock);
            packets.fetch_add(1, std::memory_order_relaxed);
            auto handle = [&](const Message& m) {
                ++parsed;
                handleLocked(m, arrivalSeconds);
                recorded = true;
            };
            ParserState& state = parsers[format];
            switch (format) {
                case kFormatBleMidi: parseBleMidiPacket(bytes, length, state, handle); break;
                case kFormatRawStream: parseRawStream(bytes, length, state, handle); break;
                default:
                    if (!parseRawPacket(bytes, length, state, handle)) {
                        rejectedPackets.fetch_add(1, std::memory_order_relaxed);
                        // Never a note-on from it; its note-offs still end what they held.
                        salvageNoteOffs(bytes, length, handle);
                    }
                    break;
            }
        }
        messages.fetch_add((uint64_t)parsed, std::memory_order_relaxed);
        // acq_rel: the events above happen-before a drain whose beginDrain reads this value.
        if (recorded && !flushPending.exchange(true, std::memory_order_acq_rel) && flushNeeded)
            *flushNeeded = true;
        return parsed;
    }

    // Any thread: another source of the engine started / released `note` (the engine's own
    // noteOn / noteOff). An input release of a note another source holds is left to that source.
    // Mark before the note-on is queued (so an input note-off racing it never releases it) and
    // undo the mark if the push failed: externalNoteOn returns the previous mark for
    // restoreExternal.
    bool externalNoteOn(int note) {
        if (note < 0 || note > 127) return false;
        return external[note].exchange(true, std::memory_order_acq_rel);
    }
    void restoreExternal(int note, bool previous) {
        if (note >= 0 && note <= 127) external[note].store(previous, std::memory_order_release);
    }
    void externalNoteOff(int note) {
        if (note >= 0 && note <= 127) external[note].store(false, std::memory_order_release);
    }

    // Any thread: the engine's allNotesOff. Releases everything queued (sink.allNotesOff) and
    // forgets the input holds and the external marks, under the input lock, so an input note-on
    // is either released by it or held after it. Bumps the epoch.
    void allNotesOff() {
        LockScope guard(lock);
        sink.allNotesOff();
        for (int i = 0; i < 128; ++i) {
            heldBy[i] = -1;
            holdCount[i] = 0;
            external[i].store(false, std::memory_order_relaxed);
        }
        heldNow.store(0, std::memory_order_relaxed);
        epochCount.fetch_add(1, std::memory_order_release);
    }

    // Any thread: ends every input hold (the input's device went away): the release of each synth
    // note the input holds, unless another source holds it; resets the parsers. Returns the new
    // epoch (events recorded before it carry a lower one).
    uint32_t releaseAll() {
        LockScope guard(lock);
        for (int s = 0; s < 128; ++s) {
            if (holdCount[s] == 0) continue;
            holdCount[s] = 0;
            if (!external[s].load(std::memory_order_acquire)) sink.noteOff(s);
        }
        for (int i = 0; i < 128; ++i) heldBy[i] = -1;
        for (ParserState& p : parsers) p.reset();
        heldNow.store(0, std::memory_order_relaxed);
        return epochCount.fetch_add(1, std::memory_order_release) + 1;
    }

    uint32_t epoch() const { return epochCount.load(std::memory_order_acquire); }

    // ── Event consumer (one thread) ──

    // Call before draining: clears the pending flag, so an event recorded during or after the
    // drain asks for another one.
    void beginDrain() { flushPending.exchange(false, std::memory_order_acq_rel); }

    // Hands every recorded event to fn (oldest first); returns how many.
    template <typename F>
    int drainEvents(F&& fn) {
        int rp = readPos.load(std::memory_order_relaxed);
        const int wp = writePos.load(std::memory_order_acquire);
        int n = 0;
        while (rp != wp) {
            fn(ring[rp]);
            rp = (rp + 1) % kEventCapacity;
            ++n;
        }
        readPos.store(rp, std::memory_order_release);
        return n;
    }

    Stats stats() const {
        Stats s;
        s.packets = packets.load(std::memory_order_relaxed);
        s.messages = messages.load(std::memory_order_relaxed);
        s.played = playedCount.load(std::memory_order_relaxed);
        s.passed = passedCount.load(std::memory_order_relaxed);
        s.dropped = droppedCount.load(std::memory_order_relaxed);
        s.rejectedPackets = rejectedPackets.load(std::memory_order_relaxed);
        s.eventsDropped = eventsDropped.load(std::memory_order_relaxed);
        s.held = heldNow.load(std::memory_order_relaxed);
        return s;
    }

    // Tests: the synth note input note n holds (-1 none) and the input holds of synth note s.
    int heldNoteForTests(int n) { LockScope guard(lock); return heldBy[n & 127]; }
    int holdCountForTests(int s) { LockScope guard(lock); return holdCount[s & 127]; }

private:
    // Caller holds `lock`.
    void handleLocked(const Message& m, double arrival) {
        Event e;
        e.arrival = arrival;
        e.epoch = epochCount.load(std::memory_order_relaxed);
        e.message = m;
        if (m.type == kNoteOn) {
            const int n = m.data1 & 127;
            if (heldBy[n] >= 0) releaseLocked(n, arrival, true);
            const InputMap::Entry entry = inputMap.read(n, m.data2);
            if (!enabled() || entry.action == InputMap::kPass) {
                passedCount.fetch_add(1, std::memory_order_relaxed);
            } else if (entry.action >= InputMap::kDrumBase) {
                const int voice = entry.action - InputMap::kDrumBase;
                if (sink.drumHit(voice, entry.velocity, arrival)) {
                    e.flags = kEventPlayed | kEventDrum;
                    e.out = (int16_t)voice;
                    e.outVelocity = entry.velocity;
                    playedCount.fetch_add(1, std::memory_order_relaxed);
                } else {
                    droppedCount.fetch_add(1, std::memory_order_relaxed);
                }
            } else {
                const int s = entry.action;
                if (sink.noteOn(s, entry.velocity, arrival)) {
                    heldBy[n] = (int16_t)s;
                    holdCount[s] += 1;
                    heldNow.fetch_add(1, std::memory_order_relaxed);
                    e.flags = kEventPlayed;
                    e.out = (int16_t)s;
                    e.outVelocity = entry.velocity;
                    playedCount.fetch_add(1, std::memory_order_relaxed);
                } else {
                    droppedCount.fetch_add(1, std::memory_order_relaxed);
                }
            }
        } else if (m.type == kNoteOff) {
            const int n = m.data1 & 127;
            if (heldBy[n] >= 0) {
                releaseLocked(n, arrival, false, &e);
                return;
            }
        }
        pushEventLocked(e);
    }

    // Caller holds `lock`; heldBy[n] >= 0. Ends input note n's hold. A synthetic release records
    // its own note-off event; otherwise `e` (the incoming note-off) is completed and recorded.
    void releaseLocked(int n, double arrival, bool synthetic, Event* e = nullptr) {
        const int s = heldBy[n];
        heldBy[n] = -1;
        heldNow.fetch_sub(1, std::memory_order_relaxed);
        bool last = false;
        if (holdCount[s] > 0) holdCount[s] -= 1;
        if (holdCount[s] == 0) {
            last = true;
            if (!external[s].load(std::memory_order_acquire)) sink.noteOff(s);
        }
        Event local;
        Event& out = e ? *e : local;
        if (!e) {
            out.arrival = arrival;
            out.epoch = epochCount.load(std::memory_order_relaxed);
            out.message.type = kNoteOff;
            out.message.data1 = (uint8_t)n;
        }
        out.flags = (uint8_t)(kEventPlayed | (last ? kEventLast : 0) | (synthetic ? kEventSynthetic : 0));
        out.out = (int16_t)s;
        pushEventLocked(out);
    }

    // Caller holds `lock` (producers serialised); one consumer.
    void pushEventLocked(const Event& e) {
        const int wp = writePos.load(std::memory_order_relaxed);
        const int next = (wp + 1) % kEventCapacity;
        if (next == readPos.load(std::memory_order_acquire)) {
            eventsDropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        ring[wp] = e;
        writePos.store(next, std::memory_order_release);
    }

    MidiInputSink& sink;
    InputMap inputMap;
    std::atomic<bool> enabledFlag{false};
    InputLock lock;

    // Guarded by `lock`.
    ParserState parsers[kFormatCount];
    int16_t heldBy[128];     // input note → synth note its note-on started, -1 = none
    uint16_t holdCount[128]; // synth note → input holds of it
    std::atomic<bool> external[128];

    std::atomic<uint32_t> epochCount{0};
    std::atomic<bool> flushPending{false};
    Event ring[kEventCapacity];
    std::atomic<int> writePos{0};
    std::atomic<int> readPos{0};

    std::atomic<uint64_t> packets{0};
    std::atomic<uint64_t> messages{0};
    std::atomic<uint64_t> playedCount{0};
    std::atomic<uint64_t> passedCount{0};
    std::atomic<uint64_t> droppedCount{0};
    std::atomic<uint64_t> rejectedPackets{0};
    std::atomic<uint64_t> eventsDropped{0};
    std::atomic<int> heldNow{0};
};

// The input behind the C entry point expojuce_midi_input_bytes (ExpoJuceMidiInput.mm), and the
// function it calls when a drain should be scheduled (nullptr = none). Before the first call the
// entry point returns -1.
void installEntryPoint(MidiInput* input, void (*scheduleDrain)());

} // namespace midiin
