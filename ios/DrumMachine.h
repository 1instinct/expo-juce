#pragma once

// Pure C++ (no Objective-C, no JUCE): used by PolySynthProcessor, ExpoJuceAudioEngine.mm and the
// offline harness (test/harness).
//
// A synthesized eight-voice drum kit (no samples). Each voice is monophonic: a hit restarts it
// (the previous sound is faded out over kDeclickSamples so the restart does not click). The
// closed hat chokes the open hat. Hits arrive on the audio thread (PolySynthProcessor drains them
// from the note queue) and start at the first sample of the block they are drained in.
//
// Parameters live in a DrumParamStore of relaxed atomics: any thread writes them, the audio thread
// reads them once per block. Nothing here allocates, locks or blocks after construction.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

inline constexpr int kDrumVoices = 8;

// Pad order (top-left to bottom-right).
enum DrumVoiceIndex : int {
    kDrumKick = 0,
    kDrumSnare = 1,
    kDrumClosedHat = 2,
    kDrumOpenHat = 3,
    kDrumClap = 4,
    kDrumLowTom = 5,
    kDrumHighTom = 6,
    kDrumRim = 7,
};

inline constexpr const char* kDrumVoiceNames[kDrumVoices] = {
    "kick", "snare", "closedHat", "openHat", "clap", "lowTom", "highTom", "rim",
};

struct DrumVoiceParams {
    float tune;  // semitones, -24..24
    float decay; // 0..1, scales the voice's envelopes (0.5 = nominal, 0 = x0.25, 1 = x4)
    float tone;  // 0..1, voice-specific brightness / noise mix
    float level; // 0..1
};

struct DrumKitParams {
    float level; // 0..1
    bool mute;
    float drive; // 0..1 soft saturation of the kit sum
};

inline constexpr DrumVoiceParams kDrumVoiceDefaults = {0.0f, 0.5f, 0.5f, 0.8f};
inline constexpr DrumKitParams kDrumKitDefaults = {0.8f, false, 0.0f};

enum DrumParam : int { kDrumParamTune = 0, kDrumParamDecay, kDrumParamTone, kDrumParamLevel, kDrumParamCount };
inline constexpr const char* kDrumParamNames[kDrumParamCount] = {"tune", "decay", "tone", "level"};

// Index of a voice parameter name, -1 if unknown.
inline int drumParamIndex(const char* name) {
    if (name == nullptr) return -1;
    for (int i = 0; i < kDrumParamCount; ++i) {
        const char* a = kDrumParamNames[i];
        const char* b = name;
        while (*a != '\0' && *a == *b) { ++a; ++b; }
        if (*a == '\0' && *b == '\0') return i;
    }
    return -1;
}

// Every drum parameter as relaxed atomics. Any thread, never blocks. Setters clamp; a non-finite
// value or an out-of-range voice is ignored (returns false, nothing stored).
class DrumParamStore {
public:
    DrumParamStore() { reset(); }

    static float clampParam(int param, double value) {
        if (param == kDrumParamTune) return (float)std::clamp(value, -24.0, 24.0);
        return (float)std::clamp(value, 0.0, 1.0);
    }

    bool setVoiceParam(int voice, int param, double value) {
        if (voice < 0 || voice >= kDrumVoices || param < 0 || param >= kDrumParamCount) return false;
        if (!std::isfinite(value)) return false;
        values[voice][param].store(clampParam(param, value), std::memory_order_relaxed);
        return true;
    }
    bool setKitLevel(double value) {
        if (!std::isfinite(value)) return false;
        kitLevel.store((float)std::clamp(value, 0.0, 1.0), std::memory_order_relaxed);
        return true;
    }
    bool setKitDrive(double value) {
        if (!std::isfinite(value)) return false;
        kitDrive.store((float)std::clamp(value, 0.0, 1.0), std::memory_order_relaxed);
        return true;
    }
    void setKitMute(bool mute) { kitMute.store(mute, std::memory_order_relaxed); }

    DrumVoiceParams voice(int v) const {
        v = std::clamp(v, 0, kDrumVoices - 1);
        const auto r = std::memory_order_relaxed;
        return {values[v][kDrumParamTune].load(r), values[v][kDrumParamDecay].load(r),
                values[v][kDrumParamTone].load(r), values[v][kDrumParamLevel].load(r)};
    }
    DrumKitParams kit() const {
        const auto r = std::memory_order_relaxed;
        return {kitLevel.load(r), kitMute.load(r), kitDrive.load(r)};
    }

    // Back to the defaults (control threads).
    void reset() {
        for (int v = 0; v < kDrumVoices; ++v) {
            values[v][kDrumParamTune].store(kDrumVoiceDefaults.tune, std::memory_order_relaxed);
            values[v][kDrumParamDecay].store(kDrumVoiceDefaults.decay, std::memory_order_relaxed);
            values[v][kDrumParamTone].store(kDrumVoiceDefaults.tone, std::memory_order_relaxed);
            values[v][kDrumParamLevel].store(kDrumVoiceDefaults.level, std::memory_order_relaxed);
        }
        kitLevel.store(kDrumKitDefaults.level, std::memory_order_relaxed);
        kitMute.store(kDrumKitDefaults.mute, std::memory_order_relaxed);
        kitDrive.store(kDrumKitDefaults.drive, std::memory_order_relaxed);
    }

private:
    std::atomic<float> values[kDrumVoices][kDrumParamCount];
    std::atomic<float> kitLevel{kDrumKitDefaults.level};
    std::atomic<bool> kitMute{kDrumKitDefaults.mute};
    std::atomic<float> kitDrive{kDrumKitDefaults.drive};
};

class DrumMachine {
public:
    // A voice stops once every envelope it plays is below this (-80 dB); its output is then exact 0.
    static constexpr float kSilence = 1.0e-4f;
    // A retrigger fades the previous sound out linearly over this many samples.
    static constexpr int kDeclickSamples = 64;

    // Written on the audio thread, read from any thread (relaxed).
    struct Diagnostics {
        std::atomic<uint64_t> hits{0};      // hits started by this machine
        std::atomic<float> peak{0.0f};      // decaying hold of the kit's block max |x| (~300 ms half-life)
        std::atomic<int> activeVoices{0};
    };

    DrumMachine() { reset(); }

    // nullptr = the machine's own store. The engine attaches its process-lifetime store.
    void setParamStore(DrumParamStore* s) { store = s ? s : &ownStore; }
    DrumParamStore& params() { return *store; }
    const DrumParamStore& params() const { return *store; }

    // Not concurrently with render (prepareToPlay). Stops every voice.
    void prepare(double sampleRate) {
        sr = sampleRate > 0.0 ? sampleRate : 48000.0;
        reset();
    }
    void reset() {
        for (auto& v : voices) v = VoiceState{};
        kitGain = -1.0f; // first block jumps to its target
    }

    double sampleRate() const { return sr; }
    bool isVoiceActive(int v) const { return v >= 0 && v < kDrumVoices && voices[v].active; }
    int activeVoiceCount() const {
        int n = 0;
        for (const auto& v : voices) n += v.active ? 1 : 0;
        return n;
    }

    // ── Audio thread ──

    // Starts voice 0..7 at velocity 1..127 from the next rendered sample. Out of range: ignored.
    void trigger(int voice, int velocity) {
        if (voice < 0 || voice >= kDrumVoices) return;
        VoiceState& v = voices[voice];
        // lastOut already includes what was left of an earlier fade-out.
        const float declick = v.active ? v.lastOut : 0.0f;
        v = VoiceState{};
        v.active = true;
        v.velocity = (float)std::clamp(velocity, 1, 127) / 127.0f;
        v.amp = 1.0f;
        v.amp2 = 1.0f;
        v.pitch = 1.0f;
        v.click = 1.0f;
        v.declickFrom = std::isfinite(declick) ? std::clamp(declick, -4.0f, 4.0f) : 0.0f;
        v.declickLeft = v.declickFrom != 0.0f ? kDeclickSamples : 0;
        if (voice == kDrumClosedHat && voices[kDrumOpenHat].active) voices[kDrumOpenHat].choked = true;
        diag.hits.fetch_add(1, std::memory_order_relaxed);
    }

    // Adds the kit into `out` (mono). With no voice sounding and nothing to fade, `out` is not touched.
    void render(float* out, int numSamples) {
        if (out == nullptr || numSamples <= 0) return;
        const DrumKitParams kit = store->kit();
        const float targetGain = kit.mute ? 0.0f : std::clamp(kit.level, 0.0f, 1.0f);
        const float startGain = kitGain < 0.0f ? targetGain : kitGain;
        kitGain = targetGain;

        bool any = false;
        for (const auto& v : voices) any = any || v.active;
        float blockPeak = 0.0f;
        if (any) {
            // The kit sum in chunks of kScratchSize, saturated and scaled by the kit gain.
            const float drive = std::clamp(kit.drive, 0.0f, 1.0f);
            const float k = 1.0f + 7.0f * drive;
            const float satNorm = 1.0f / std::sqrt(k);
            const float gainStep = (targetGain - startGain) / (float)numSamples;
            for (int done = 0; done < numSamples;) {
                const int n = std::min(kScratchSize, numSamples - done);
                std::fill(scratch, scratch + n, 0.0f);
                for (int i = 0; i < kDrumVoices; ++i)
                    if (voices[i].active) renderVoice(i, store->voice(i), scratch, n);
                for (int s = 0; s < n; ++s) {
                    float x = scratch[s];
                    if (drive > 0.0f) x = (1.0f - drive) * x + drive * std::tanh(k * x) * satNorm;
                    const float g = startGain + gainStep * (float)(done + s);
                    const float y = x * g;
                    out[done + s] += y;
                    blockPeak = std::max(blockPeak, std::fabs(y));
                }
                done += n;
            }
        }

        // Diagnostics: decaying peak hold (halves about every 300 ms), active voices.
        const float decay = (float)std::exp(-(double)numSamples / (0.43 * sr));
        const float held = diag.peak.load(std::memory_order_relaxed) * decay;
        diag.peak.store(std::isfinite(blockPeak) ? std::max(blockPeak, held) : held, std::memory_order_relaxed);
        diag.activeVoices.store(activeVoiceCount(), std::memory_order_relaxed);
    }

    Diagnostics diag;

private:
    static constexpr int kScratchSize = 256;

    // Simper / Cytomic trapezoidal state-variable filter.
    struct Svf {
        float ic1 = 0.0f, ic2 = 0.0f;
        float a1 = 0.0f, a2 = 0.0f, a3 = 0.0f, k = 1.0f;
        void set(float cutoff, float q, double sampleRate) {
            const float nyq = (float)(0.45 * sampleRate);
            const float fc = std::clamp(cutoff, 20.0f, nyq);
            const float g = (float)std::tan(3.14159265358979 * (double)fc / sampleRate);
            k = 1.0f / std::max(q, 0.05f);
            a1 = 1.0f / (1.0f + g * (g + k));
            a2 = g * a1;
            a3 = g * a2;
        }
        // Returns band-pass; low-pass / high-pass through the out-params.
        float process(float x, float& lp, float& hp) {
            const float v3 = x - ic2;
            const float v1 = a1 * ic1 + a2 * v3;
            const float v2 = ic2 + a2 * ic1 + a3 * v3;
            ic1 = 2.0f * v1 - ic1;
            ic2 = 2.0f * v2 - ic2;
            lp = v2;
            hp = x - k * v1 - v2;
            return v1;
        }
        float bandPass(float x) { float lp, hp; return process(x, lp, hp); }
        float highPass(float x) { float lp, hp; process(x, lp, hp); return hp; }
    };

    struct VoiceState {
        bool active = false;
        bool choked = false;
        float velocity = 0.0f;
        float amp = 0.0f;   // main amplitude envelope
        float amp2 = 0.0f;  // second envelope (snare noise, clap burst)
        float pitch = 0.0f; // pitch envelope
        float click = 0.0f; // click / transient envelope
        int age = 0;        // samples since the hit
        double phase[6] = {0, 0, 0, 0, 0, 0};
        Svf f1, f2;
        float lastOut = 0.0f;     // last sample emitted
        float declickFrom = 0.0f; // a retriggered sound's last value, ramped to 0 over kDeclickSamples
        int declickLeft = 0;
    };

    static constexpr double kTwoPi = 6.283185307179586;
    static constexpr float kHatRatios[6] = {205.3f, 304.4f, 369.6f, 522.7f, 540.0f, 800.0f};

    // Per-sample multiplier that falls by 60 dB over `seconds`.
    float coef60(double seconds) const {
        return (float)std::exp(-6.907755278982137 / (std::max(seconds, 1.0e-4) * sr));
    }

    float noise() {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        return (float)(int32_t)rng * (1.0f / 2147483648.0f);
    }

    // Renders `n` samples of voice `index` added into `dst`; deactivates it when silent.
    void renderVoice(int index, const DrumVoiceParams& p, float* dst, int n) {
        VoiceState& v = voices[index];
        const float tune = std::isfinite(p.tune) ? std::clamp(p.tune, -24.0f, 24.0f) : 0.0f;
        const float ratio = (float)std::pow(2.0, (double)tune / 12.0);
        const double decayScale = std::pow(2.0, ((double)std::clamp(p.decay, 0.0f, 1.0f) - 0.5) * 4.0);
        const float tone = std::clamp(p.tone, 0.0f, 1.0f);
        const float gain = std::clamp(p.level, 0.0f, 1.0f) * v.velocity;
        const float invSr = (float)(1.0 / sr);
        const float chokeCoef = coef60(0.012);

        bool silent = false;
        int s = 0;
        switch (index) {
        case kDrumKick: {
            const float base = 52.0f * ratio;
            const float ampC = coef60(0.6 * decayScale);
            const float pitchC = coef60(0.08);   // ~4x → 1x over the first ~40 ms
            const float clickC = coef60(0.012);
            const float clickGain = 0.1f + 0.5f * tone;
            for (; s < n; ++s) {
                const float f = base * (1.0f + 3.0f * v.pitch);
                v.phase[0] += kTwoPi * (double)(f * invSr);
                if (v.phase[0] >= kTwoPi) v.phase[0] -= kTwoPi;
                const float x = (float)std::sin(v.phase[0]) * v.amp + noise() * v.click * clickGain;
                emit(v, dst, s, x * gain);
                v.amp *= ampC; v.pitch *= pitchC; v.click *= clickC;
                if (v.amp < kSilence && v.click < kSilence) { silent = true; ++s; break; }
            }
            break;
        }
        case kDrumSnare: {
            const float ampC = coef60(0.2 * decayScale);
            const float noiseC = coef60(0.3 * decayScale);
            const float pitchC = coef60(0.03);
            v.f1.set((2500.0f + 4500.0f * tone) * ratio, 0.8f, sr);
            const float bodyGain = 0.8f * (1.0f - 0.5f * tone);
            const float noiseGain = 0.5f + 0.9f * tone;
            for (; s < n; ++s) {
                const float bend = 1.0f + 0.4f * v.pitch;
                v.phase[0] += kTwoPi * (double)(185.0f * ratio * bend * invSr);
                v.phase[1] += kTwoPi * (double)(330.0f * ratio * bend * invSr);
                if (v.phase[0] >= kTwoPi) v.phase[0] -= kTwoPi;
                if (v.phase[1] >= kTwoPi) v.phase[1] -= kTwoPi;
                const float body = 0.6f * (float)std::sin(v.phase[0]) + 0.4f * (float)std::sin(v.phase[1]);
                const float snares = v.f1.bandPass(noise());
                const float x = body * v.amp * bodyGain + snares * v.amp2 * noiseGain;
                emit(v, dst, s, x * gain);
                v.amp *= ampC; v.amp2 *= noiseC; v.pitch *= pitchC;
                if (v.amp < kSilence && v.amp2 < kSilence) { silent = true; ++s; break; }
            }
            break;
        }
        case kDrumClosedHat:
        case kDrumOpenHat: {
            const bool open = index == kDrumOpenHat;
            const float ampC = v.choked ? chokeCoef : coef60((open ? 0.7 : 0.09) * decayScale);
            v.f1.set((7000.0f + 4000.0f * tone) * ratio, 1.2f, sr);
            v.f2.set((5000.0f + 3000.0f * tone) * ratio, 0.7f, sr);
            const float metalGain = 1.0f - 0.6f * tone;
            const float noiseGain = 0.2f + 0.6f * tone;
            double inc[6];
            for (int o = 0; o < 6; ++o) inc[o] = (double)(kHatRatios[o] * ratio * invSr);
            for (; s < n; ++s) {
                float metal = 0.0f;
                for (int o = 0; o < 6; ++o) {
                    v.phase[o] += inc[o];
                    if (v.phase[o] >= 1.0) v.phase[o] -= 1.0;
                    metal += v.phase[o] < 0.5 ? 1.0f : -1.0f;
                }
                const float src = metal * (1.0f / 6.0f) * metalGain + noise() * noiseGain;
                const float x = v.f2.highPass(v.f1.bandPass(src)) * 1.6f * v.amp;
                emit(v, dst, s, x * gain);
                v.amp *= ampC;
                if (v.amp < kSilence) { silent = true; ++s; break; }
            }
            break;
        }
        case kDrumClap: {
            const int burst = std::max(1, (int)std::lround(0.010 * sr));
            const float burstC = coef60(0.03);
            const float tailC = coef60(0.35 * decayScale);
            v.f1.set((900.0f + 1600.0f * tone) * ratio, 1.4f, sr);
            for (; s < n; ++s) {
                if (v.age > 0 && v.age < 3 * burst && v.age % burst == 0) v.amp2 = 1.0f; // bursts 2 and 3
                const bool tail = v.age >= 3 * burst;
                const float env = v.amp2 + (tail ? 0.7f * v.amp : 0.0f);
                const float x = v.f1.bandPass(noise()) * 2.5f * env;
                emit(v, dst, s, x * gain);
                v.amp2 *= burstC;
                if (tail) v.amp *= tailC;
                ++v.age;
                if (tail && v.amp < kSilence && v.amp2 < kSilence) { silent = true; ++s; break; }
            }
            break;
        }
        case kDrumLowTom:
        case kDrumHighTom: {
            const bool high = index == kDrumHighTom;
            const float base = (high ? 150.0f : 95.0f) * ratio;
            const float ampC = coef60((high ? 0.4 : 0.5) * decayScale);
            const float pitchC = coef60(0.25);
            const float clickC = coef60(0.015);
            const float clickGain = 0.35f * tone;
            for (; s < n; ++s) {
                const float f = base * (1.0f + 0.5f * v.pitch);
                v.phase[0] += kTwoPi * (double)(f * invSr);
                if (v.phase[0] >= kTwoPi) v.phase[0] -= kTwoPi;
                const float x = (float)std::sin(v.phase[0]) * v.amp + noise() * v.click * clickGain;
                emit(v, dst, s, x * gain);
                v.amp *= ampC; v.pitch *= pitchC; v.click *= clickC;
                if (v.amp < kSilence && v.click < kSilence) { silent = true; ++s; break; }
            }
            break;
        }
        case kDrumRim:
        default: {
            const float pingC = coef60(0.07 * decayScale);
            const float clickC = coef60(0.008);
            v.f1.set(3500.0f * ratio, 2.0f, sr);
            const float pingGain = 0.7f * (1.0f - 0.5f * tone);
            const float clickGain = 1.0f + 2.0f * tone;
            for (; s < n; ++s) {
                v.phase[0] += kTwoPi * (double)(1660.0f * ratio * invSr);
                v.phase[1] += kTwoPi * (double)(480.0f * ratio * invSr);
                if (v.phase[0] >= kTwoPi) v.phase[0] -= kTwoPi;
                if (v.phase[1] >= kTwoPi) v.phase[1] -= kTwoPi;
                const float ping = 0.7f * (float)std::sin(v.phase[0]) + 0.3f * (float)std::sin(v.phase[1]);
                const float click = v.f1.bandPass(noise()) * v.click;
                const float x = ping * v.amp * pingGain + click * clickGain;
                emit(v, dst, s, x * gain);
                v.amp *= pingC; v.click *= clickC;
                if (v.amp < kSilence && v.click < kSilence) { silent = true; ++s; break; }
            }
            break;
        }
        }
        if (index != kDrumClap) v.age += s;
        if (silent) {
            // Finish the declick ramp, then stop: everything after is exactly 0.
            for (; s < n && v.declickLeft > 0; ++s) emit(v, dst, s, 0.0f);
            if (v.declickLeft == 0) v = VoiceState{};
        }
    }

    // One output sample of voice `v` (plus what is left of a retriggered sound's fade-out).
    static inline void emit(VoiceState& v, float* dst, int s, float x) {
        if (v.declickLeft > 0) {
            x += v.declickFrom * ((float)v.declickLeft / (float)kDeclickSamples);
            --v.declickLeft;
        }
        if (!std::isfinite(x)) x = 0.0f;
        v.lastOut = x;
        dst[s] += x;
    }

    VoiceState voices[kDrumVoices];
    double sr = 48000.0;
    float kitGain = -1.0f; // gain at the end of the last block (< 0: none yet)
    uint32_t rng = 0x9E3779B9u;
    float scratch[kScratchSize] = {};
    DrumParamStore ownStore;
    DrumParamStore* store = &ownStore;
};
