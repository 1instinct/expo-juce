#pragma once

// Pure C++ (no Objective-C, no JUCE): used by PolySynthProcessor, SamplerBank, ExpoJuceAudioEngine.mm
// and the offline harness (test/harness).
//
// Sampler voice core: 16 sample slots (numbered 0..15) played by 32 preallocated stereo voices.
// A slot holds one buffer (borrowed: the engine never owns, allocates or frees sample memory; see
// SamplerBank for loading and the hand-off) and its playing parameters (SamplerSlotParams). Pads
// play a slot at its root note (hit / release); keys play every loaded slot whose key zone holds
// the note (noteOn / noteOff). Pitch is the playback rate, 2^((note - root + tune) / 12) times the
// buffer's rate over the engine's, read with 4-point Hermite interpolation.
//
// Envelope: linear attack from 0 over attackMs, hold, linear release to exact 0 over releaseMs
// from wherever the level is. A one-shot slot ignores note-offs and releases and plays to its end;
// a gated slot plays while held, then releases (looping, if it loops, until the release ends).
// A loop is crossfaded over kSamplerLoopCrossfadeMs: approaching the loop's end, the voice fades
// to the audio just before the loop's start, then jumps there, so the seam is continuous. A loop
// that starts within the crossfade's length of the buffer's start begins that much later.
// Choke groups 1..8: a voice starting in a group fades every other sounding voice of that group
// out over kSamplerChokeMs (voices started by the same call are spared). Voice stealing takes the
// voice released earliest, else the oldest. A stolen voice, and a voice whose slot changes buffer,
// stop reading their buffer at once and fade their last output out over kDeclickSamples.
//
// Parameters live in a SamplerParamStore: any thread writes it, the audio thread copies a slot's
// parameters when it starts a voice and at the start of each block (a write in progress keeps
// the previous copy; the audio thread never waits). Nothing here allocates, locks or blocks on
// the audio thread.

#include "Hermite.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>

inline constexpr int kSamplerSlots = 16;
inline constexpr int kSamplerVoices = 32;
inline constexpr int kSamplerChokeGroups = 8;
inline constexpr float kSamplerLoopCrossfadeMs = 5.0f;
inline constexpr float kSamplerChokeMs = 5.0f;

// A playable buffer, borrowed by the engine: stereo (L may equal R), `frames` frames at `rate` Hz.
struct SampleBuffer {
    const float* L;
    const float* R;
    int frames;
    double rate;
};

// One slot's playing parameters. Times in milliseconds of the buffer; -1 = the buffer's end.
// lowNote > highNote: no key plays the slot (pads still do).
struct SamplerSlotParams {
    int rootNote = 60, lowNote = 0, highNote = 127;
    float startMs = 0, endMs = -1;
    bool loop = false;
    float loopStartMs = 0, loopEndMs = -1;
    bool oneShot = true;
    float gainDb = 0, pan = 0, tuneSemis = 0, tuneCents = 0, attackMs = 1, releaseMs = 50, velocitySens = 1;
    int chokeGroup = 0;
};

// Engine-wide settings.
struct SamplerSettings {
    float level = 0.8f; // 0..1
    bool mute = false;
    int polyphony = kSamplerVoices; // 1..32
};

// Clamped copy of `p`; a non-finite number takes `fallback`'s value.
inline SamplerSlotParams samplerSanitize(const SamplerSlotParams& p, const SamplerSlotParams& fallback) {
    auto num = [](float v, float fb, float lo, float hi) {
        return std::isfinite(v) ? std::clamp(v, lo, hi) : std::clamp(fb, lo, hi);
    };
    // A time that may be -1 (the buffer's end): any negative value means the end.
    auto endTime = [](float v, float fb) {
        const float x = std::isfinite(v) ? v : fb;
        return x < 0.0f ? -1.0f : std::min(x, 600000.0f);
    };
    SamplerSlotParams o;
    o.rootNote = std::clamp(p.rootNote, 0, 127);
    o.lowNote = std::clamp(p.lowNote, 0, 127);
    o.highNote = std::clamp(p.highNote, 0, 127);
    o.startMs = num(p.startMs, fallback.startMs, 0.0f, 600000.0f);
    o.endMs = endTime(p.endMs, fallback.endMs);
    o.loop = p.loop;
    o.loopStartMs = num(p.loopStartMs, fallback.loopStartMs, 0.0f, 600000.0f);
    o.loopEndMs = endTime(p.loopEndMs, fallback.loopEndMs);
    o.oneShot = p.oneShot;
    o.gainDb = num(p.gainDb, fallback.gainDb, -60.0f, 24.0f);
    o.pan = num(p.pan, fallback.pan, -1.0f, 1.0f);
    o.tuneSemis = num(p.tuneSemis, fallback.tuneSemis, -48.0f, 48.0f);
    o.tuneCents = num(p.tuneCents, fallback.tuneCents, -100.0f, 100.0f);
    o.attackMs = num(p.attackMs, fallback.attackMs, 0.0f, 10000.0f);
    o.releaseMs = num(p.releaseMs, fallback.releaseMs, 1.0f, 10000.0f);
    o.velocitySens = num(p.velocitySens, fallback.velocitySens, 0.0f, 1.0f);
    o.chokeGroup = std::clamp(p.chokeGroup, 0, kSamplerChokeGroups);
    return o;
}

// Every slot's parameters and the engine settings, as atomics. Writers (any non-audio thread) are
// serialized by a mutex; each slot carries a version (odd while a write is in progress), so the
// audio thread reads a slot without waiting and only keeps the copy it read when no write
// overlapped it (a sequence lock with a single attempt).
class SamplerParamStore {
public:
    SamplerParamStore() { reset(); }

    // Clamps (see samplerSanitize; non-finite numbers keep the current value). false: slot out of range.
    bool setSlot(int slot, const SamplerSlotParams& p) {
        if (slot < 0 || slot >= kSamplerSlots) return false;
        std::lock_guard<std::mutex> lock(writer);
        Slot& s = slots[slot];
        const SamplerSlotParams clean = samplerSanitize(p, readRelaxed(s));
        const uint32_t v = s.version.load(std::memory_order_relaxed);
        s.version.store(v + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        writeRelaxed(s, clean);
        s.version.store(v + 2, std::memory_order_release);
        return true;
    }

    // Control threads: the slot's stored parameters (defaults for an out-of-range slot).
    SamplerSlotParams slot(int slot) const {
        if (slot < 0 || slot >= kSamplerSlots) return SamplerSlotParams{};
        std::lock_guard<std::mutex> lock(writer);
        return readRelaxed(slots[slot]);
    }

    // Audio thread: copies the slot into `out` unless a write is in progress or overlapped the read
    // (then `out` is left as it was and false is returned). Never waits.
    bool trySnapshot(int slot, SamplerSlotParams& out) const noexcept {
        if (slot < 0 || slot >= kSamplerSlots) return false;
        const Slot& s = slots[slot];
        const uint32_t v1 = s.version.load(std::memory_order_acquire);
        if ((v1 & 1u) != 0) return false;
        const SamplerSlotParams p = readRelaxed(s);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (s.version.load(std::memory_order_relaxed) != v1) return false;
        out = p;
        return true;
    }

    bool setLevel(double v) {
        if (!std::isfinite(v)) return false;
        level.store((float)std::clamp(v, 0.0, 1.0), std::memory_order_relaxed);
        return true;
    }
    void setMute(bool m) { mute.store(m, std::memory_order_relaxed); }
    bool setPolyphony(double v) {
        if (!std::isfinite(v)) return false;
        polyphony.store((int)std::clamp(std::lround(v), 1L, (long)kSamplerVoices), std::memory_order_relaxed);
        return true;
    }
    SamplerSettings settings() const {
        const auto r = std::memory_order_relaxed;
        return { level.load(r), mute.load(r), polyphony.load(r) };
    }

    // Back to the defaults (control threads).
    void reset() {
        std::lock_guard<std::mutex> lock(writer);
        for (Slot& s : slots) {
            const uint32_t v = s.version.load(std::memory_order_relaxed); // even: writers hold `writer`
            s.version.store(v + 1, std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_release);
            writeRelaxed(s, SamplerSlotParams{});
            s.version.store(v + 2, std::memory_order_release);
        }
        const SamplerSettings d;
        level.store(d.level, std::memory_order_relaxed);
        mute.store(d.mute, std::memory_order_relaxed);
        polyphony.store(d.polyphony, std::memory_order_relaxed);
    }

private:
    struct Slot {
        std::atomic<uint32_t> version{0};
        std::atomic<int> rootNote{60}, lowNote{0}, highNote{127}, chokeGroup{0};
        std::atomic<bool> loop{false}, oneShot{true};
        std::atomic<float> startMs{0}, endMs{-1}, loopStartMs{0}, loopEndMs{-1}, gainDb{0}, pan{0}, tuneSemis{0},
            tuneCents{0}, attackMs{1}, releaseMs{50}, velocitySens{1};
    };

    static SamplerSlotParams readRelaxed(const Slot& s) {
        const auto r = std::memory_order_relaxed;
        SamplerSlotParams p;
        p.rootNote = s.rootNote.load(r);
        p.lowNote = s.lowNote.load(r);
        p.highNote = s.highNote.load(r);
        p.startMs = s.startMs.load(r);
        p.endMs = s.endMs.load(r);
        p.loop = s.loop.load(r);
        p.loopStartMs = s.loopStartMs.load(r);
        p.loopEndMs = s.loopEndMs.load(r);
        p.oneShot = s.oneShot.load(r);
        p.gainDb = s.gainDb.load(r);
        p.pan = s.pan.load(r);
        p.tuneSemis = s.tuneSemis.load(r);
        p.tuneCents = s.tuneCents.load(r);
        p.attackMs = s.attackMs.load(r);
        p.releaseMs = s.releaseMs.load(r);
        p.velocitySens = s.velocitySens.load(r);
        p.chokeGroup = s.chokeGroup.load(r);
        return p;
    }
    static void writeRelaxed(Slot& s, const SamplerSlotParams& p) {
        const auto r = std::memory_order_relaxed;
        s.rootNote.store(p.rootNote, r);
        s.lowNote.store(p.lowNote, r);
        s.highNote.store(p.highNote, r);
        s.startMs.store(p.startMs, r);
        s.endMs.store(p.endMs, r);
        s.loop.store(p.loop, r);
        s.loopStartMs.store(p.loopStartMs, r);
        s.loopEndMs.store(p.loopEndMs, r);
        s.oneShot.store(p.oneShot, r);
        s.gainDb.store(p.gainDb, r);
        s.pan.store(p.pan, r);
        s.tuneSemis.store(p.tuneSemis, r);
        s.tuneCents.store(p.tuneCents, r);
        s.attackMs.store(p.attackMs, r);
        s.releaseMs.store(p.releaseMs, r);
        s.velocitySens.store(p.velocitySens, r);
        s.chokeGroup.store(p.chokeGroup, r);
    }

    Slot slots[kSamplerSlots];
    std::atomic<float> level{0.8f};
    std::atomic<bool> mute{false};
    std::atomic<int> polyphony{kSamplerVoices};
    mutable std::mutex writer; // control threads only
};

// Where the engine takes each slot's buffer from at the start of a block (SamplerBank). Called on
// the audio thread only; acquireSlot must not block or allocate. The returned buffer stays valid
// until the next acquireSlot of that slot or releaseReader().
class SamplerBufferSource {
public:
    virtual ~SamplerBufferSource() = default;
    virtual const SampleBuffer* acquireSlot(int slot) noexcept = 0;
    // The reader is gone (its processor is being destroyed): it references no buffer any more.
    virtual void releaseReader() noexcept = 0;
};

// What a voice is doing (tests, diagnostics).
struct SamplerVoiceInfo {
    bool active = false;
    int slot = -1;
    int note = -1; // the key for a note-on, the slot's root note for a hit
    bool releasing = false;
};

class SamplerEngine {
public:
    // A stolen voice, or one whose buffer went away, fades its last output out over this many samples.
    static constexpr int kDeclickSamples = 64;
    // render() works in chunks of at most this many frames (preallocated scratch).
    static constexpr int kChunk = 256;

    // Written on the audio thread, read from any thread (relaxed).
    struct Diagnostics {
        std::atomic<uint64_t> hits{0};    // hits that started a voice
        std::atomic<uint64_t> noteOns{0}; // note-ons received (whether or not a zone held them)
        std::atomic<uint64_t> steals{0};  // voices taken from a sounding note
        std::atomic<uint64_t> ignored{0}; // calls with a slot or note out of range
        std::atomic<float> peak{0.0f};    // decaying hold of the sampler's block max |x| (~300 ms half-life)
        std::atomic<int> activeVoices{0};
    };

    SamplerEngine() { reset(); }

    // nullptr = the engine's own store. The audio engine attaches its process-lifetime store.
    // Not concurrently with render.
    void setParamStore(SamplerParamStore* s) {
        store = s ? s : &ownStore;
        for (int slot = 0; slot < kSamplerSlots; ++slot) slots[slot].params = store->slot(slot);
    }
    SamplerParamStore& params() { return *store; }
    const SamplerParamStore& params() const { return *store; }

    // Buffers come from `source` at every beginBlock() (nullptr = none: setSlotBuffer only).
    // Not concurrently with render.
    void setSource(SamplerBufferSource* s) { source = s; }
    SamplerBufferSource* bufferSource() const { return source; }

    // Not concurrently with render (prepareToPlay). Stops every voice; slot buffers are kept.
    void prepare(double sampleRate, int /*maxBlock*/) {
        sr = sampleRate > 0.0 ? sampleRate : 48000.0;
        reset();
    }
    void reset() {
        for (Voice& v : voices) v = Voice{};
        levelGain = -1.0f; // the first block jumps to its target
    }
    double sampleRate() const { return sr; }

    // Any thread (they write the store).
    bool setSlotParams(int slot, const SamplerSlotParams& p) { return store->setSlot(slot, p); }
    void setLevel(float level) { store->setLevel(level); }
    void setMute(bool mute) { store->setMute(mute); }
    void setPolyphony(int voices) { store->setPolyphony(voices); }

    // ── Audio thread ──

    // Start of a block: every slot's buffer from the source (if one is set) and parameters from the store.
    void beginBlock() {
        if (source != nullptr)
            for (int slot = 0; slot < kSamplerSlots; ++slot) setSlotBuffer(slot, source->acquireSlot(slot));
        for (int slot = 0; slot < kSamplerSlots; ++slot) store->trySnapshot(slot, slots[slot].params);
    }

    // The slot's buffer (nullptr = none). A change ends the slot's sounding voices (declicked); they
    // never read the previous buffer again.
    void setSlotBuffer(int slot, const SampleBuffer* buffer) {
        if (slot < 0 || slot >= kSamplerSlots) return;
        if (slots[slot].buffer == buffer) return;
        for (Voice& v : voices)
            if (v.active && v.slot == slot) kill(v);
        slots[slot].buffer = buffer;
    }
    bool isSlotLoaded(int slot) const {
        if (slot < 0 || slot >= kSamplerSlots) return false;
        const SampleBuffer* b = slots[slot].buffer;
        return b != nullptr && b->frames > 0 && b->L != nullptr && b->R != nullptr;
    }

    // Pad: plays the slot at its root note. seq: a release mark the voice answers to (releaseHeld), 0 = none.
    void hit(int slot, int velocity, uint64_t seq = 0) {
        if (slot < 0 || slot >= kSamplerSlots) { diag.ignored.fetch_add(1, std::memory_order_relaxed); return; }
        store->trySnapshot(slot, slots[slot].params);
        const uint32_t event = ++eventCounter;
        if (startVoice(slot, slots[slot].params.rootNote, velocity, false, seq, event))
            diag.hits.fetch_add(1, std::memory_order_relaxed);
    }

    // Key: plays every loaded slot whose zone holds `note`.
    void noteOn(int note, int velocity, uint64_t seq = 0) {
        if (note < 0 || note > 127) { diag.ignored.fetch_add(1, std::memory_order_relaxed); return; }
        diag.noteOns.fetch_add(1, std::memory_order_relaxed);
        const uint32_t event = ++eventCounter;
        for (int slot = 0; slot < kSamplerSlots; ++slot) {
            store->trySnapshot(slot, slots[slot].params);
            const SamplerSlotParams& p = slots[slot].params;
            if (note >= p.lowNote && note <= p.highNote) startVoice(slot, note, velocity, true, seq, event);
        }
    }

    // Releases every gated voice a note-on of `note` started.
    void noteOff(int note) {
        for (Voice& v : voices)
            if (v.active && v.fromKey && v.note == note) releaseVoice(v);
    }

    // Releases every gated voice a hit on `slot` started.
    void release(int slot) {
        for (Voice& v : voices)
            if (v.active && !v.fromKey && v.slot == slot) releaseVoice(v);
    }

    // Releases the held gated voices for which isReleased(fromKey, note or slot, seq) is true.
    // Voices started with seq 0 are left alone.
    template <typename IsReleased>
    void releaseHeld(IsReleased&& isReleased) {
        for (Voice& v : voices) {
            if (!v.active || v.releasing || v.oneShot || v.seq == 0) continue;
            if (isReleased(v.fromKey, v.fromKey ? v.note : v.slot, v.seq)) releaseVoice(v);
        }
    }

    // Adds the sampler (stereo) into L / R. With nothing sounding the buffers are not touched.
    void render(float* L, float* R, int numSamples) {
        const SamplerSettings st = store->settings();
        const float target = st.mute ? 0.0f : st.level;
        bool any = false;
        for (const Voice& v : voices) any = any || v.active || v.tailLeft > 0;
        float blockPeak = 0.0f;
        if (!any) {
            levelGain = target;
        } else {
            for (int done = 0; done < numSamples;) {
                const int m = std::min(kChunk, numSamples - done);
                std::fill(scratchL, scratchL + m, 0.0f);
                std::fill(scratchR, scratchR + m, 0.0f);
                for (Voice& v : voices) {
                    if (v.tailLeft > 0) renderTail(v, m);
                    if (v.active) renderVoice(v, m);
                }
                const float g0 = levelGain < 0.0f ? target : levelGain;
                const float step = (target - g0) / (float)m;
                for (int i = 0; i < m; ++i) {
                    const float g = step == 0.0f ? g0 : g0 + step * (float)(i + 1);
                    const float l = scratchL[i] * g, r = scratchR[i] * g;
                    L[done + i] += l;
                    R[done + i] += r;
                    blockPeak = std::max(blockPeak, std::max(std::fabs(l), std::fabs(r)));
                }
                levelGain = target;
                done += m;
            }
        }
        const float decay = sr > 0.0 ? (float)std::exp(-(double)numSamples / (0.43 * sr)) : 0.0f;
        diag.peak.store(std::max(blockPeak, diag.peak.load(std::memory_order_relaxed) * decay), std::memory_order_relaxed);
        diag.activeVoices.store(activeVoiceCount(), std::memory_order_relaxed);
    }

    int activeVoiceCount() const {
        int n = 0;
        for (const Voice& v : voices) n += v.active ? 1 : 0;
        return n;
    }
    SamplerVoiceInfo voiceInfo(int index) const {
        if (index < 0 || index >= kSamplerVoices) return {};
        const Voice& v = voices[index];
        return { v.active, v.active ? v.slot : -1, v.active ? v.note : -1, v.active && v.releasing };
    }

    Diagnostics diag;

private:
    struct Voice {
        bool active = false, releasing = false, fromKey = false, oneShot = true, loop = false;
        int slot = -1, note = -1, choke = 0;
        uint64_t seq = 0, order = 0, releaseOrder = 0;
        uint32_t event = 0;
        const float* L = nullptr;
        const float* R = nullptr;
        int frames = 0;
        double pos = 0.0, inc = 1.0, end = 0.0;
        double loopStart = 0.0, loopEnd = 0.0, loopLen = 0.0, xfade = 0.0;
        float gainL = 0.0f, gainR = 0.0f;
        float env = 0.0f, envStep = 0.0f; // attack: +step, release: -step
        int attackLeft = 0, releaseLeft = 0;
        float lastL = 0.0f, lastR = 0.0f; // last output (before the engine level)
        float tailL = 0.0f, tailR = 0.0f; // declick of an ended sound, fading over kDeclickSamples
        int tailLeft = 0;
    };

    struct SlotState {
        const SampleBuffer* buffer = nullptr;
        SamplerSlotParams params;
    };

    static float at(const float* x, int frames, int64_t i) {
        return x[i < 0 ? 0 : (i >= frames ? frames - 1 : i)];
    }
    static void read(const Voice& v, double pos, float& l, float& r) {
        const int64_t k = (int64_t)pos;
        const float t = (float)(pos - (double)k);
        if (t == 0.0f && k < v.frames) {
            l = v.L[k];
            r = v.R[k];
            return;
        }
        if (k >= 1 && k + 2 < v.frames) {
            l = hermite4(v.L[k - 1], v.L[k], v.L[k + 1], v.L[k + 2], t);
            r = hermite4(v.R[k - 1], v.R[k], v.R[k + 1], v.R[k + 2], t);
            return;
        }
        l = hermite4(at(v.L, v.frames, k - 1), at(v.L, v.frames, k), at(v.L, v.frames, k + 1), at(v.L, v.frames, k + 2), t);
        r = hermite4(at(v.R, v.frames, k - 1), at(v.R, v.frames, k), at(v.R, v.frames, k + 1), at(v.R, v.frames, k + 2), t);
    }

    // The voice's sound ends now: it fades its last output (plus what was left of an earlier fade).
    void startTail(Voice& v) {
        const float keep = v.tailLeft > 0 ? (float)v.tailLeft / (float)kDeclickSamples : 0.0f;
        v.tailL = v.lastL + v.tailL * keep;
        v.tailR = v.lastR + v.tailR * keep;
        if (!std::isfinite(v.tailL) || !std::isfinite(v.tailR)) v.tailL = v.tailR = 0.0f;
        v.tailLeft = (v.tailL != 0.0f || v.tailR != 0.0f) ? kDeclickSamples : 0;
        v.lastL = v.lastR = 0.0f;
    }
    void kill(Voice& v) {
        startTail(v);
        v.active = false;
    }

    void beginRelease(Voice& v, float ms) {
        const int frames = std::max(1, (int)std::lround((double)ms * sr / 1000.0));
        if (v.releasing && v.releaseLeft <= frames) return; // already ending sooner
        if (!v.releasing) v.releaseOrder = ++orderCounter;
        v.releasing = true;
        v.attackLeft = 0;
        v.releaseLeft = frames;
        v.envStep = v.env / (float)frames;
    }
    void releaseVoice(Voice& v) {
        if (v.oneShot || v.releasing) return;
        beginRelease(v, slots[v.slot].params.releaseMs);
    }

    // A free voice, or the one to steal: the voice released earliest, else the oldest.
    int allocateVoice() {
        const int polyphony = std::clamp(store->settings().polyphony, 1, kSamplerVoices);
        int active = 0, idle = -1;
        for (int i = 0; i < kSamplerVoices; ++i) {
            if (voices[i].active) ++active;
            else if (idle < 0) idle = i;
        }
        if (idle >= 0 && active < polyphony) return idle;
        int best = -1;
        for (int i = 0; i < kSamplerVoices; ++i)
            if (voices[i].active && voices[i].releasing && (best < 0 || voices[i].releaseOrder < voices[best].releaseOrder)) best = i;
        if (best < 0)
            for (int i = 0; i < kSamplerVoices; ++i)
                if (voices[i].active && (best < 0 || voices[i].order < voices[best].order)) best = i;
        return best >= 0 ? best : 0;
    }

    bool startVoice(int slot, int note, int velocity, bool fromKey, uint64_t seq, uint32_t event) {
        if (!isSlotLoaded(slot)) return false;
        const SampleBuffer& b = *slots[slot].buffer;
        const SamplerSlotParams& p = slots[slot].params;
        if (p.chokeGroup > 0) {
            for (Voice& o : voices)
                if (o.active && o.choke == p.chokeGroup && o.event != event) beginRelease(o, kSamplerChokeMs);
        }
        Voice& v = voices[allocateVoice()];
        if (v.active) {
            kill(v);
            diag.steals.fetch_add(1, std::memory_order_relaxed);
        }
        const double rate = b.rate > 0.0 ? b.rate : sr;
        const double perMs = rate / 1000.0;
        const int frames = b.frames;
        const int64_t start = std::clamp<int64_t>(std::llround(p.startMs * perMs), 0, frames - 1);
        const int64_t end = p.endMs < 0.0f ? frames : std::clamp<int64_t>(std::llround(p.endMs * perMs), start + 1, frames);

        v.active = true;
        v.releasing = false;
        v.fromKey = fromKey;
        v.oneShot = p.oneShot;
        v.slot = slot;
        v.note = note;
        v.choke = p.chokeGroup;
        v.seq = seq;
        v.event = event;
        v.order = ++orderCounter;
        v.releaseOrder = 0;
        v.L = b.L;
        v.R = b.R;
        v.frames = frames;
        const double semis = (double)(note - p.rootNote) + (double)p.tuneSemis + (double)p.tuneCents / 100.0;
        v.inc = std::exp2(semis / 12.0) * rate / sr;
        v.pos = (double)start;
        v.end = (double)end;
        v.loop = false;
        if (p.loop && !p.oneShot) {
            int64_t ls = std::clamp<int64_t>(std::llround(p.loopStartMs * perMs), 0, end - 1);
            const int64_t le = p.loopEndMs < 0.0f ? end : std::clamp<int64_t>(std::llround(p.loopEndMs * perMs), ls + 1, end);
            if (le - ls >= 4) {
                const int64_t xf = std::min<int64_t>(std::llround(kSamplerLoopCrossfadeMs * perMs), (le - ls) / 3);
                ls = std::max(ls, xf); // the crossfade reads the xf frames before the loop's start
                v.loop = true;
                v.loopStart = (double)ls;
                v.loopEnd = (double)le;
                v.loopLen = (double)(le - ls);
                v.xfade = (double)xf;
            }
        }
        const double velGain = (1.0 - p.velocitySens) + p.velocitySens * (double)std::clamp(velocity, 1, 127) / 127.0;
        const float g = (float)(std::pow(10.0, (double)p.gainDb / 20.0) * velGain);
        v.gainL = g * std::min(1.0f, 1.0f - p.pan);
        v.gainR = g * std::min(1.0f, 1.0f + p.pan);
        v.attackLeft = (int)std::lround((double)p.attackMs * sr / 1000.0);
        v.releaseLeft = 0;
        if (v.attackLeft > 0) {
            v.env = 0.0f;
            v.envStep = 1.0f / (float)v.attackLeft;
        } else {
            v.env = 1.0f;
            v.envStep = 0.0f;
        }
        return true;
    }

    void renderTail(Voice& v, int m) {
        const float inv = 1.0f / (float)kDeclickSamples;
        for (int i = 0; i < m && v.tailLeft > 0; ++i) {
            --v.tailLeft;
            const float f = (float)v.tailLeft * inv;
            scratchL[i] += v.tailL * f;
            scratchR[i] += v.tailR * f;
        }
        if (v.tailLeft == 0) v.tailL = v.tailR = 0.0f;
    }

    void renderVoice(Voice& v, int m) {
        for (int i = 0; i < m; ++i) {
            if (v.loop) {
                if (v.pos >= v.loopEnd) v.pos = v.loopStart + std::fmod(v.pos - v.loopStart, v.loopLen);
            } else if (v.pos >= v.end) {
                v.active = false; // its natural end: nothing left to fade
                v.lastL = v.lastR = 0.0f;
                return;
            }
            float l, r;
            read(v, v.pos, l, r);
            if (v.loop && v.pos >= v.loopEnd - v.xfade) {
                float l2, r2;
                read(v, v.pos - v.loopLen, l2, r2);
                const float a = (float)((v.pos - (v.loopEnd - v.xfade)) / v.xfade);
                l += a * (l2 - l);
                r += a * (r2 - r);
            }
            const float outL = l * v.gainL * v.env, outR = r * v.gainR * v.env;
            scratchL[i] += outL;
            scratchR[i] += outR;
            v.lastL = outL;
            v.lastR = outR;
            if (v.attackLeft > 0) {
                v.env = --v.attackLeft == 0 ? 1.0f : v.env + v.envStep;
            } else if (v.releaseLeft > 0) {
                if (--v.releaseLeft == 0) {
                    v.active = false; // released to exact 0
                    v.lastL = v.lastR = 0.0f;
                    return;
                }
                v.env = std::max(0.0f, v.env - v.envStep);
            }
            v.pos += v.inc;
        }
    }

    Voice voices[kSamplerVoices];
    SlotState slots[kSamplerSlots];
    SamplerParamStore ownStore;
    SamplerParamStore* store = &ownStore;
    SamplerBufferSource* source = nullptr;
    double sr = 48000.0;
    float levelGain = -1.0f;
    uint64_t orderCounter = 0;
    uint32_t eventCounter = 0;
    float scratchL[kChunk] = {};
    float scratchR[kChunk] = {};
};
