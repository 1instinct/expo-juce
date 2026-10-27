#pragma once

// Pure C++ (no Objective-C): used by PolySynthProcessor, ExpoJuceAudioEngine.mm and the offline
// harness (test/harness).
//
// Arpeggiator between the note queue and the voices. The audio thread feeds it the drained
// note-ons (noteOn), tells it which held notes the player has released (releaseHeld) and calls
// process() once per block; process() hands back note-on / note-off events with sample offsets
// inside the block, in ascending order, which PolySynthProcessor applies between render segments.
//
// No allocation after construction, no locks: the options are atomics written from any thread
// (setSettings) and read once per block; everything else is audio-thread state. Held notes
// (max kMaxHeld), the latched list, the step counter, the pending hits / note-offs that fall in
// later blocks (max kMaxPending) and the per-block event list (max kMaxEvents) are fixed arrays.
//
// Pattern logic (shared with the app's TypeScript fallback; both test suites assert the same
// vectors):
//  - Held list in press order with velocities. `latch` keeps the list when every note is
//    released; the first note-on after a full release replaces it. allNotesOff clears it.
//  - Cycle: `up` = held sorted ascending, then +12 per extra octave appended; `down` = reversed;
//    `upDown` = up followed by its interior reversed; `played` = press order, octave copies
//    appended in press order; `random` = a uniform pick from the up cycle per step; `chord` =
//    every held note on every step.
//  - Step k (from the arp start): pattern slot k mod length; an off slot plays nothing and does
//    not advance the cycle. On: cycle[k' mod len], k' counting the on-steps so far. Pass
//    p = floor(k / length); with transposeSteps > 0 the note rises by
//    (p mod (transposeSteps + 1)) × transposeDistance semitones. Notes outside 0..127 are skipped.
//  - Velocity: the held note's; slots other than 0 scale by 1 − 0.6 × accent.
//  - Timing: step length L = 60 / bpm / stepsPerBeat. Step k starts at k × L, odd steps
//    swing × L later. Ratchet r splits the step into r hits of L / r with gate × L / r each.
//    Note-off at start + gate × L (or gate × L / r). Transport playing: the grid is
//    beat × stepsPerBeat ∈ ℤ (tempo changes re-derive L from the block's bpm); step 0 is the
//    grid line just passed when the arp starts at most ¼ step after it, else the next line.
//    Stopped: the grid starts at the first note-on (step 0 plays in that block) and runs free at
//    the current tempo.
//  - The cycle restarts (k = 0) when the held list goes from empty to non-empty (or is replaced
//    after a latched release). Adding / removing notes while running keeps k.
//  - A note-on of a note that is still sounding drops that note's pending note-off: the voice
//    allocator cuts the previous voice of the same note on retrigger, so the off that follows
//    the new hit is the one that stops it (gate > 1 on a one-note cycle = mono legato).

#include "JuceConfig.h"
#include <juce_core/juce_core.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

enum class ArpMode { Up, Down, UpDown, Played, Random, Chord };
enum class ArpRate { Quarter, Eighth, EighthT, EighthD, Sixteenth, SixteenthT, ThirtySecond };

inline constexpr int kArpModeCount = 6;
inline constexpr int kArpRateCount = 7;
inline constexpr const char* kArpModeNames[kArpModeCount] = { "up", "down", "upDown", "played", "random", "chord" };
inline constexpr const char* kArpRateNames[kArpRateCount] = { "1/4", "1/8", "1/8T", "1/8D", "1/16", "1/16T", "1/32" };
// Steps per beat × 12, an integer so the dotted eighth (4/3) is exact.
inline constexpr int kArpStepsPerBeat12[kArpRateCount] = { 12, 24, 36, 16, 48, 72, 96 };

// -1 for an unknown name.
inline int arpModeIndex(const char* name) {
    if (name == nullptr) return -1;
    for (int i = 0; i < kArpModeCount; ++i)
        if (std::strcmp(kArpModeNames[i], name) == 0) return i;
    return -1;
}
inline int arpRateIndex(const char* name) {
    if (name == nullptr) return -1;
    for (int i = 0; i < kArpRateCount; ++i)
        if (std::strcmp(kArpRateNames[i], name) == 0) return i;
    return -1;
}

// Every arpeggiator option, as stored (after clamping). Mirrors the JS ArpOptions; the pattern
// is a 16-bit mask (bit i = step i plays) plus its length.
struct ArpSettings {
    bool enabled;
    ArpMode mode;
    ArpRate rate;
    int octaves;           // 1–4
    float gate;            // 0.05–1.5 of a step
    float swing;           // 0–0.5 of a step, odd steps
    bool latch;
    uint16_t patternMask;
    int patternLength;     // 1–16
    float accent;          // 0–1
    int ratchet;           // 1–4
    int transposeSteps;    // 0–8 (0 = off)
    int transposeDistance; // −12..12

    static ArpSettings defaults() {
        return { false, ArpMode::Up, ArpRate::Sixteenth, 1, 0.6f, 0.0f, false, 0xFFFF, 16, 0.0f, 1, 0, 0 };
    }
    bool operator==(const ArpSettings& o) const {
        return enabled == o.enabled && mode == o.mode && rate == o.rate && octaves == o.octaves && gate == o.gate
            && swing == o.swing && latch == o.latch && patternMask == o.patternMask && patternLength == o.patternLength
            && accent == o.accent && ratchet == o.ratchet && transposeSteps == o.transposeSteps
            && transposeDistance == o.transposeDistance;
    }
    bool operator!=(const ArpSettings& o) const { return !(*this == o); }
};

// The options as atomics: written from any thread (clamped), read by the audio thread once per
// block. The Arpeggiator embeds one; the engine keeps another for the life of the process so
// setArpeggiator works before setup and across rebuilds (AuxSettingsStore does the same).
struct ArpSettingsStore {
    static float clampFinite(float v, float lo, float hi, float fallback) {
        return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback;
    }

    void set(const ArpSettings& s) {
        const auto r = std::memory_order_relaxed;
        enabled.store(s.enabled, r);
        mode.store(std::clamp((int)s.mode, 0, kArpModeCount - 1), r);
        rate.store(std::clamp((int)s.rate, 0, kArpRateCount - 1), r);
        octaves.store(std::clamp(s.octaves, 1, 4), r);
        gate.store(clampFinite(s.gate, 0.05f, 1.5f, 0.6f), r);
        swing.store(clampFinite(s.swing, 0.0f, 0.5f, 0.0f), r);
        latch.store(s.latch, r);
        patternMask.store((int)s.patternMask & 0xFFFF, r);
        patternLen.store(std::clamp(s.patternLength, 1, 16), r);
        accent.store(clampFinite(s.accent, 0.0f, 1.0f, 0.0f), r);
        ratchet.store(std::clamp(s.ratchet, 1, 4), r);
        transposeSteps.store(std::clamp(s.transposeSteps, 0, 8), r);
        transposeDistance.store(std::clamp(s.transposeDistance, -12, 12), r);
    }

    ArpSettings get() const {
        const auto r = std::memory_order_relaxed;
        return { enabled.load(r), (ArpMode)mode.load(r), (ArpRate)rate.load(r), octaves.load(r), gate.load(r),
                 swing.load(r), latch.load(r), (uint16_t)patternMask.load(r), patternLen.load(r), accent.load(r),
                 ratchet.load(r), transposeSteps.load(r), transposeDistance.load(r) };
    }

    std::atomic<bool> enabled{false};
    std::atomic<int> mode{(int)ArpMode::Up};
    std::atomic<int> rate{(int)ArpRate::Sixteenth};
    std::atomic<int> octaves{1};
    std::atomic<float> gate{0.6f};
    std::atomic<float> swing{0.0f};
    std::atomic<bool> latch{false};
    std::atomic<int> patternMask{0xFFFF};
    std::atomic<int> patternLen{16};
    std::atomic<float> accent{0.0f};
    std::atomic<int> ratchet{1};
    std::atomic<int> transposeSteps{0};
    std::atomic<int> transposeDistance{0};
};

// One event for the processor: at `sampleOffset` of the current block, start (`on`) or stop
// the arp voice(s) of `note`.
struct ArpEvent {
    int sampleOffset;
    int note;
    int velocity;
    bool on;
};

class Arpeggiator {
public:
    static constexpr int kMaxHeld = 16;
    static constexpr int kMaxOctaves = 4;
    static constexpr int kMaxCycle = kMaxHeld * kMaxOctaves * 2; // upDown doubles the up cycle
    static constexpr int kMaxPending = 128;
    static constexpr int kMaxEvents = 1024;

    // ── Options (any thread; atomics, clamped: see ArpSettingsStore) ──

    void setSettings(const ArpSettings& s) { opts.set(s); }
    ArpSettings getSettings() const { return opts.get(); }
    bool isEnabled() const { return opts.enabled.load(std::memory_order_relaxed); }

    // ── Audio thread ──

    // A drained note-on. Ignored while disabled (the processor plays those notes directly).
    void noteOn(int note, int velocity, uint64_t seq) {
        if (!isEnabled()) return;
        note = std::clamp(note, 0, 127);
        velocity = std::clamp(velocity, 1, 127);
        if (latchedAll) { numHeld = 0; latchedAll = false; running = false; }
        const bool wasEmpty = numHeld == 0;
        int i = 0;
        for (; i < numHeld; ++i)
            if (held[i].note == note) break;
        if (i == numHeld) {
            if (numHeld >= kMaxHeld) return;
            ++numHeld;
        }
        held[i] = { note, velocity, seq, false };
        if (wasEmpty || !running) start();
    }

    // The player released `note` (every held entry of it).
    void noteOff(int note) {
        releaseHeld([note](int n, uint64_t) { return n == note; });
    }

    // Releases every held entry for which isReleased(note, seq) is true (the processor passes
    // NoteEventQueue::isReleased). Latched: the entries stay until the next press replaces them.
    template <typename Pred>
    void releaseHeld(Pred&& isReleased) {
        const bool latching = opts.latch.load(std::memory_order_relaxed);
        int kept = 0;
        for (int i = 0; i < numHeld; ++i) {
            HeldNote h = held[i];
            if (isReleased(h.note, h.seq)) {
                if (!latching) continue;
                h.released = true;
            }
            held[kept++] = h;
        }
        numHeld = kept;
        afterListChange(latching);
    }

    // Stops the arp's sounding voices in the next process() and clears the held / latched list.
    void allNotesOff() { flushRequested = true; }

    // Emits this block's events through sink(const ArpEvent&), sample offsets ascending (a
    // note-off before a note-on at the same offset). blockStartBeat / bpm / playing describe the
    // block (TransportEngine::getBlockStartBeat etc.).
    template <typename Sink>
    void process(double blockStartBeat, double bpm, bool playing, double sampleRate, int numSamples, Sink&& sink) {
        eventCount = 0;
        if (numSamples <= 0) return;
        const auto r = std::memory_order_relaxed;
        const bool on = opts.enabled.load(r);
        if (!on || flushRequested) {
            flush();
            flushRequested = false;
            if (!on) { running = false; anchored = false; }
            emit(sink);
            return;
        }
        if (!opts.latch.load(r) && latchedAll) {
            // Latch turned off while holding: the released entries go.
            int kept = 0;
            for (int i = 0; i < numHeld; ++i)
                if (!held[i].released) held[kept++] = held[i];
            numHeld = kept;
            afterListChange(false);
        }

        // Pending hits and note-offs that fall in this block.
        {
            Pending keep[kMaxPending];
            int keepCount = 0;
            const int count = pendingCount;
            Pending due[kMaxPending];
            int dueCount = 0;
            for (int i = 0; i < count; ++i) {
                Pending p = pending[i];
                if (p.samplesUntil < numSamples) due[dueCount++] = p;
                else { p.samplesUntil -= numSamples; keep[keepCount++] = p; }
            }
            std::memcpy(pending, keep, sizeof(Pending) * (size_t)keepCount);
            pendingCount = keepCount;
            for (int i = 0; i < dueCount; ++i) {
                const Pending& p = due[i];
                if (p.on) {
                    if (!running) continue; // the list emptied meanwhile: the hit is dropped
                    emitOn(p.samplesUntil, p.note, p.velocity);
                    scheduleOff(p.samplesUntil + p.gateSamples, p.note, numSamples);
                } else {
                    emitOff(p.samplesUntil, p.note);
                }
            }
        }

        if (running && bpm > 0.0 && sampleRate > 0.0) {
            const int spb12 = kArpStepsPerBeat12[std::clamp(opts.rate.load(r), 0, kArpRateCount - 1)];
            const double spb = spb12 / 12.0;
            const double L = 60.0 / bpm / spb * sampleRate;
            const float sw = opts.swing.load(r);
            if (!anchored || playing != wasPlaying) {
                if (playing) anchorToGrid(blockStartBeat, spb12);
                else { haveLastGrid = haveFired; lastGridIn = -samplesSinceGrid; }
                anchored = true;
            }
            wasPlaying = playing;
            bool firedThisBlock = false;
            for (int guard = 0; guard < 4096; ++guard) {
                double g; // offset of the next grid line from the block start, samples
                if (playing) {
                    g = (gridBeat(spb12) - blockStartBeat) * 60.0 / bpm * sampleRate;
                    if (g < -1.0 || g > numSamples + 2.0 * L + 1.0) { // the transport moved
                        anchorToGrid(blockStartBeat, spb12);
                        g = (gridBeat(spb12) - blockStartBeat) * 60.0 / bpm * sampleRate;
                    }
                } else {
                    g = haveLastGrid ? lastGridIn + L : 0.0;
                }
                const double e = g + ((stepIndex & 1) ? sw * L : 0.0);
                const long eo = std::lround(e);
                if (eo >= numSamples) break;
                fireStep(std::max(0.0, e), L, numSamples, r);
                if (playing) ++gridIndex;
                else { lastGridIn = g; haveLastGrid = true; }
                samplesSinceGrid = numSamples - g;
                haveFired = true;
                firedThisBlock = true;
            }
            if (!firedThisBlock) samplesSinceGrid += numSamples;
            if (!playing) lastGridIn -= numSamples;
        } else {
            samplesSinceGrid += numSamples;
            if (!playing) lastGridIn -= numSamples;
        }
        emit(sink);
    }

    // ── State (audio thread writes; relaxed reads from the processor for the analysis publish) ──

    int currentStep() const { return curStep; }
    int patternLength() const { return opts.patternLen.load(std::memory_order_relaxed); }
    int lastNote() const { return lastNoteStarted; }
    // Enabled and holding (or latched) notes: a step is sounding or coming.
    bool isActive() const { return isEnabled() && running && numHeld > 0; }
    int heldCount() const { return numHeld; }

    // Hits / note-offs dropped because a fixed list was full (diagnostic, relaxed).
    std::atomic<uint32_t> eventsDropped{0};

private:
    struct HeldNote { int note; int velocity; uint64_t seq; bool released; };
    struct Pending { int samplesUntil; int note; int velocity; bool on; int gateSamples; };
    struct Event { int offset; int note; int velocity; bool on; int order; };

    void start() {
        running = true;
        stepIndex = 0;
        onSteps = 0;
        anchored = false;
        haveFired = false;
        haveLastGrid = false;
        samplesSinceGrid = 0.0;
        // Drop hits still pending from the previous run; their note-offs stay.
        int kept = 0;
        for (int i = 0; i < pendingCount; ++i)
            if (!pending[i].on) pending[kept++] = pending[i];
        pendingCount = kept;
        ++startCount;
        rng.setSeed((juce::int64)juce::Time::getHighResolutionTicks() + (juce::int64)startCount * 7919);
    }

    void afterListChange(bool latching) {
        if (numHeld == 0) { running = false; latchedAll = false; return; }
        bool allReleased = latching;
        for (int i = 0; i < numHeld && allReleased; ++i) allReleased = held[i].released;
        latchedAll = allReleased;
    }

    // Offs for every sounding note at offset 0; everything cleared.
    void flush() {
        for (int n = 0; n < 128; ++n)
            if (sounding[n]) emitOff(0, n);
        pendingCount = 0;
        numHeld = 0;
        latchedAll = false;
        running = false;
        anchored = false;
    }

    double gridBeat(int spb12) const { return (double)(gridIndex * 12) / (double)spb12; }

    // Playing: step 0 is the grid line just passed if the block starts at most ¼ step after it,
    // else the next line.
    void anchorToGrid(double blockStartBeat, int spb12) {
        const double x = blockStartBeat * spb12 / 12.0;
        const double prev = std::floor(x + 1e-9);
        gridIndex = (x - prev <= 0.25) ? (int64_t)prev : (int64_t)std::ceil(x - 1e-9);
    }

    void fireStep(double at, double L, int numSamples, std::memory_order r) {
        const int length = std::clamp(opts.patternLen.load(r), 1, 16);
        const int slot = (int)(stepIndex % length);
        curStep = slot;
        const bool slotOn = ((opts.patternMask.load(r) >> slot) & 1) != 0;
        if (slotOn) {
            const int tSteps = opts.transposeSteps.load(r);
            const int64_t pass = stepIndex / length;
            const int transpose = tSteps > 0 ? (int)(pass % (tSteps + 1)) * opts.transposeDistance.load(r) : 0;
            const float velScale = slot != 0 ? 1.0f - 0.6f * opts.accent.load(r) : 1.0f;
            const int hits = std::clamp(opts.ratchet.load(r), 1, 4);
            const double hitLen = L / hits;
            const int gateSamples = std::max(1, (int)std::lround(opts.gate.load(r) * hitLen));
            const ArpMode m = (ArpMode)opts.mode.load(r);
            if (m == ArpMode::Chord) {
                for (int i = 0; i < numHeld; ++i)
                    hit(at, hitLen, hits, gateSamples, held[i].note + transpose, held[i].velocity, velScale, numSamples);
            } else {
                buildCycle(m, opts.octaves.load(r));
                if (cycleLen > 0) {
                    int idx = (int)(onSteps % cycleLen);
                    if (m == ArpMode::Random) idx = rng.nextInt(cycleLen);
                    hit(at, hitLen, hits, gateSamples, cycleNotes[idx] + transpose, cycleVel[idx], velScale, numSamples);
                }
            }
            ++onSteps;
        }
        ++stepIndex;
    }

    // `hits` hits of `hitLen` samples from `at`, each `gateSamples` long.
    void hit(double at, double hitLen, int hits, int gateSamples, int note, int velocity, float velScale, int numSamples) {
        if (note < 0 || note > 127) return;
        const int vel = std::clamp((int)std::lround(velocity * velScale), 1, 127);
        for (int h = 0; h < hits; ++h) {
            const long o = std::lround(at + h * hitLen);
            if (o < numSamples) {
                emitOn((int)o, note, vel);
                scheduleOff((int)o + gateSamples, note, numSamples);
            } else if (pendingCount < kMaxPending) {
                pending[pendingCount++] = { (int)o - numSamples, note, vel, true, gateSamples };
            } else {
                eventsDropped.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    void buildCycle(ArpMode m, int octs) {
        octs = std::clamp(octs, 1, kMaxOctaves);
        cycleLen = 0;
        if (numHeld == 0) return;
        if (m == ArpMode::Played) {
            for (int o = 0; o < octs; ++o)
                for (int i = 0; i < numHeld; ++i) { cycleNotes[cycleLen] = held[i].note + 12 * o; cycleVel[cycleLen++] = held[i].velocity; }
            return;
        }
        // Up: sorted ascending (insertion sort on at most kMaxHeld entries), octaves appended.
        int order[kMaxHeld];
        for (int i = 0; i < numHeld; ++i) {
            int j = i;
            while (j > 0 && held[order[j - 1]].note > held[i].note) { order[j] = order[j - 1]; --j; }
            order[j] = i;
        }
        for (int o = 0; o < octs; ++o)
            for (int i = 0; i < numHeld; ++i) {
                cycleNotes[cycleLen] = held[order[i]].note + 12 * o;
                cycleVel[cycleLen++] = held[order[i]].velocity;
            }
        if (m == ArpMode::Down) {
            std::reverse(cycleNotes, cycleNotes + cycleLen);
            std::reverse(cycleVel, cycleVel + cycleLen);
        } else if (m == ArpMode::UpDown) {
            const int upLen = cycleLen;
            for (int i = upLen - 2; i >= 1 && cycleLen < kMaxCycle; --i) {
                cycleNotes[cycleLen] = cycleNotes[i];
                cycleVel[cycleLen++] = cycleVel[i];
            }
        }
    }

    void emitOn(int offset, int note, int velocity) {
        // The allocator cuts the previous voice of this note: its pending off would stop the new
        // one, so it goes (see the header comment).
        int kept = 0;
        for (int i = 0; i < pendingCount; ++i)
            if (pending[i].on || pending[i].note != note) pending[kept++] = pending[i];
        pendingCount = kept;
        int ek = 0;
        for (int i = 0; i < eventCount; ++i)
            if (events[i].on || events[i].note != note || events[i].offset < offset) events[ek++] = events[i];
        eventCount = ek;
        push(offset, note, velocity, true);
        sounding[note] = 1;
        lastNoteStarted = note;
    }

    void emitOff(int offset, int note) {
        push(offset, note, 0, false);
        sounding[note] = 0;
    }

    void scheduleOff(int offset, int note, int numSamples) {
        if (offset < numSamples) { emitOff(offset, note); return; }
        if (pendingCount < kMaxPending) { pending[pendingCount++] = { offset - numSamples, note, 0, false, 0 }; return; }
        emitOff(numSamples - 1, note); // never leave a note hanging
        eventsDropped.fetch_add(1, std::memory_order_relaxed);
    }

    void push(int offset, int note, int velocity, bool on) {
        if (eventCount >= kMaxEvents) { eventsDropped.fetch_add(1, std::memory_order_relaxed); return; }
        events[eventCount] = { offset, note, velocity, on, eventCount };
        ++eventCount;
    }

    template <typename Sink>
    void emit(Sink&& sink) {
        std::sort(events, events + eventCount, [](const Event& a, const Event& b) {
            if (a.offset != b.offset) return a.offset < b.offset;
            if (a.on != b.on) return !a.on; // off before on at the same offset
            return a.order < b.order;
        });
        for (int i = 0; i < eventCount; ++i)
            sink(ArpEvent{ events[i].offset, events[i].note, events[i].velocity, events[i].on });
        eventCount = 0;
    }

    // Options (any thread).
    ArpSettingsStore opts;

    // Audio thread.
    HeldNote held[kMaxHeld] = {};
    int numHeld = 0;
    bool latchedAll = false;      // latch on and every held note released
    bool running = false;         // list non-empty since the last start
    bool flushRequested = false;  // allNotesOff
    int64_t stepIndex = 0;        // k
    int64_t onSteps = 0;          // k'
    bool anchored = false;
    bool wasPlaying = false;
    int64_t gridIndex = 0;        // playing: next grid line (beat × stepsPerBeat)
    double lastGridIn = 0.0;      // stopped: last grid line relative to the current block start
    bool haveLastGrid = false;
    bool haveFired = false;
    double samplesSinceGrid = 0.0; // since the last grid line, at the start of the next block
    int curStep = 0;
    int lastNoteStarted = -1;
    uint32_t startCount = 0;
    juce::Random rng;
    uint8_t sounding[128] = {};
    int cycleNotes[kMaxCycle] = {};
    int cycleVel[kMaxCycle] = {};
    int cycleLen = 0;
    Pending pending[kMaxPending] = {};
    int pendingCount = 0;
    Event events[kMaxEvents] = {};
    int eventCount = 0;
};
