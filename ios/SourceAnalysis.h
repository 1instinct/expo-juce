#pragma once

// Pure C++ (no Objective-C): getSourceAnalysis's reader side, used by ExpoJuceAudioEngine.mm, the
// Android host and the offline harness (test/harness). Everything here runs on the calling thread,
// never on the audio thread: one copy of the source's ring (SourceRing.h), one 2048-point FFT when
// bands are asked (the table and scaling of getAnalysis, SpectrumAnalyser.h) and the scope.
//
// Scope modes:
//
// - window: the last `windowMs` of samples, `scopePoints` long. Each point is a Hann-weighted
//   average over ±(span / points) samples around its position, so nothing above the points' own
//   Nyquist folds back (the old every-16th-sample pick aliased everything above ~1.5 kHz). It is
//   triggered: the window is centred on the largest positive sample (after the same smoothing)
//   between half a window and one and a half windows old (the window stays inside the last two), refined between samples, so a steady tone stands still
//   and a hit sits in the middle.
// - cycle: one period P = sampleRate / f of the source (f = `cycleHz`, or the last note the synth
//   started). The DC-free signal is low-passed at about 4 f (one pole) to find the latest rising
//   zero crossing t that leaves a full period on both sides; the period after t must correlate
//   with the one before (normalised correlation ≥ kMinCycleCorrelation), else the scope falls
//   back to `window` (chords, drums, noise, glides). The period is returned starting at its
//   maximum (point 0 = the peak, refined between samples), resampled to `scopePoints` with the
//   same anti-alias weighting, so consecutive calls on a steady tone return the same shape.
//
// Cost on the calling thread (est.): ring copy ~7 k loads, FFT ~50 µs, scope well under 0.1 ms.

#include "SourceRing.h"
#include "SpectrumAnalyser.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

enum class SourceScopeMode : int { None = 0, Window = 1, Cycle = 2 };

struct SourceAnalysisOptions {
    static constexpr int kMinPoints = 16, kMaxPoints = 128, kDefaultPoints = 64;
    static constexpr double kMinWindowMs = 5.0, kMaxWindowMs = 42.0, kDefaultWindowMs = 20.0;

    bool bands = true;
    SourceScopeMode scope = SourceScopeMode::Window;
    int scopePoints = kDefaultPoints;
    double cycleHz = 0.0;   // ≤ 0 / non-finite: the last note the synth started (at its detune)
    double windowMs = kDefaultWindowMs;

    // Clamps / defaults (call after filling from the caller's options).
    void sanitize() {
        scopePoints = std::clamp(scopePoints, kMinPoints, kMaxPoints);
        windowMs = std::isfinite(windowMs) ? std::clamp(windowMs, kMinWindowMs, kMaxWindowMs) : kDefaultWindowMs;
        if (!std::isfinite(cycleHz) || cycleHz < 0.0) cycleHz = 0.0;
    }
    static int pointsFrom(double value) {
        return std::isfinite(value) ? std::clamp((int)std::lround(value), kMinPoints, kMaxPoints) : kDefaultPoints;
    }
};

struct SourceAnalysisResult {
    bool active = false;
    float peak = 0.0f, rms = 0.0f;
    float bands[SpectrumAnalyser::kBands] = {};
    float scope[SourceAnalysisOptions::kMaxPoints] = {};
    int scopeCount = 0;                       // points in `scope` (0 for mode None)
    SourceScopeMode scopeMode = SourceScopeMode::None;
    double scopePeriod = 0.0;                 // samples per cycle in Cycle mode, else 0
    double sampleRate = 0.0;                  // 0 while the engine is not running
};

inline const char* sourceScopeModeName(SourceScopeMode m) {
    return m == SourceScopeMode::Cycle ? "cycle" : m == SourceScopeMode::Window ? "window" : "none";
}
inline SourceScopeMode sourceScopeModeFrom(const char* name, SourceScopeMode fallback) {
    if (name == nullptr) return fallback;
    const std::string s(name);
    if (s == "none") return SourceScopeMode::None;
    if (s == "window") return SourceScopeMode::Window;
    if (s == "cycle") return SourceScopeMode::Cycle;
    return fallback;
}

namespace sourcescope {

constexpr double kMinCycleCorrelation = 0.9;

// x at `pos` (fractional), Hann-weighted over (pos − h, pos + h), h ≥ 1; samples outside [0, n)
// are left out (their weight too).
inline float filteredAt(const float* x, int n, double pos, double h) {
    h = std::max(h, 1.0);
    const int lo = std::max(0, (int)std::ceil(pos - h + 1e-9));
    const int hi = std::min(n - 1, (int)std::floor(pos + h - 1e-9));
    double sum = 0.0, weights = 0.0;
    for (int k = lo; k <= hi; ++k) {
        const double w = 0.5 + 0.5 * std::cos(3.14159265358979323846 * ((double)k - pos) / h);
        sum += w * x[k];
        weights += w;
    }
    if (weights <= 0.0) return x[std::clamp((int)std::lround(pos), 0, std::max(n - 1, 0))];
    return (float)(sum / weights);
}

// Linear interpolation of x at a fractional position inside [0, n − 1].
inline float linearAt(const float* x, int n, double pos) {
    pos = std::clamp(pos, 0.0, (double)(n - 1));
    const int i = std::min((int)pos, n - 2 < 0 ? 0 : n - 2);
    const double f = pos - i;
    return (float)(x[i] + f * ((i + 1 < n ? x[i + 1] : x[i]) - x[i]));
}

// Fractional position of the largest positive value of the h-smoothed x in [from, to] (integer
// bounds inside [0, n)): a box of about 2h finds the candidate, the Hann kernel refines it within
// ±h, a parabola places it between samples. Latest wins ties.
inline double peakPosition(const float* x, int n, int from, int to, double h) {
    from = std::clamp(from, 0, n - 1);
    to = std::clamp(to, from, n - 1);
    const int half = std::max(0, (int)std::lround(h) / 2);
    // Box (moving average) over [k − half, k + half].
    double run = 0.0;
    int count = 0;
    for (int k = from - half; k <= from + half; ++k)
        if (k >= 0 && k < n) { run += x[k]; ++count; }
    int best = from;
    double bestValue = count > 0 ? run / count : 0.0;
    for (int k = from + 1; k <= to; ++k) {
        const int out = k - half - 1, in = k + half;
        if (out >= 0 && out < n) { run -= x[out]; --count; }
        if (in >= 0 && in < n) { run += x[in]; ++count; }
        const double v = count > 0 ? run / count : 0.0;
        if (v >= bestValue) { bestValue = v; best = k; }
    }
    // Refine with the Hann kernel.
    const int reach = (int)std::ceil(h);
    int peak = best;
    float peakValue = filteredAt(x, n, best, h);
    for (int k = std::max(from, best - reach); k <= std::min(to, best + reach); ++k) {
        const float v = filteredAt(x, n, k, h);
        if (v >= peakValue) { peakValue = v; peak = k; }
    }
    if (peak <= 0 || peak >= n - 1) return peak;
    const double a = filteredAt(x, n, peak - 1, h), b = peakValue, c = filteredAt(x, n, peak + 1, h);
    const double denom = a - 2.0 * b + c;
    const double offset = denom < 0.0 ? std::clamp(0.5 * (a - c) / denom, -0.5, 0.5) : 0.0;
    return peak + offset;
}

// Window scope over x[0, n) (oldest first). Always succeeds when n is large enough.
inline bool window(const float* x, int n, double sr, double windowMs, int points, float* out) {
    if (n < 4 * points || points <= 0) return false;
    const int span = std::clamp((int)std::lround(windowMs * sr / 1000.0), points, n / 2 - 64);
    const double step = (double)span / points;
    const double h = std::max(step, 1.0);
    const int margin = (int)std::ceil(h) + 1;
    const int hi = n - 1 - span / 2 - margin;
    const int lo = std::max(span / 2 + margin, hi - span);
    const double centre = peakPosition(x, n, lo, hi, h);
    for (int j = 0; j < points; ++j)
        out[j] = filteredAt(x, n, centre + ((double)j - 0.5 * (points - 1)) * step, h);
    return true;
}

// Cycle scope over x[0, n) at `hz`; false (nothing written) when the source is not periodic at that
// period, silent, or the period does not fit. `scratch` holds n floats.
inline bool cycle(const float* x, int n, double sr, double hz, int points, float* out, double& period,
                  float* scratch) {
    period = 0.0;
    if (!(hz > 0.0) || !(sr > 0.0) || points <= 0) return false;
    const double p = sr / hz;
    if (p < 4.0) return false;
    const double h = std::max(p / points, 1.0);
    // Low-pass warm-up (half a period), the period before t, one period to find t in, the period
    // after t, the kernel on both ends.
    const int need = (int)std::ceil(3.5 * p + 2.0 * h) + 8;
    if (need > n) return false;
    const int start = n - need;
    double mean = 0.0;
    for (int k = start; k < n; ++k) mean += x[k];
    mean /= (double)need;
    // One-pole low-pass at ~4 f of the DC-free signal (trigger only).
    const double cutoff = std::min(4.0 * hz, 0.45 * sr);
    const double a = 1.0 - std::exp(-2.0 * 3.14159265358979323846 * cutoff / sr);
    double z = x[start] - mean;
    for (int k = start; k < n; ++k) {
        z += a * ((x[k] - mean) - z);
        scratch[k] = (float)z;
    }
    // Latest rising zero crossing with a full period (and the kernel) on both sides.
    const double latest = (double)(n - 2) - p - h;
    const double earliest = (double)start + 0.5 * p + p + h + 1.0;
    double t = -1.0;
    for (int k = (int)std::floor(latest); k > (int)std::ceil(earliest); --k) {
        if (scratch[k - 1] < 0.0f && scratch[k] >= 0.0f) {
            t = (double)(k - 1) + (double)scratch[k - 1] / ((double)scratch[k - 1] - (double)scratch[k]);
            break;
        }
    }
    if (t < earliest || t > latest) return false;
    // Periodicity: the period after t against the one before.
    const int m = (int)std::floor(p);
    double sab = 0.0, saa = 0.0, sbb = 0.0;
    for (int u = 0; u < m; ++u) {
        const double va = linearAt(x, n, t + u) - mean;
        const double vb = linearAt(x, n, t + u - p) - mean;
        sab += va * vb;
        saa += va * va;
        sbb += vb * vb;
    }
    const double floorEnergy = 1e-8 * m; // −80 dBFS RMS
    if (saa < floorEnergy || sbb < floorEnergy) return false;
    if (sab / std::sqrt(saa * sbb) < kMinCycleCorrelation) return false;
    // The period before t, starting at its maximum (one sample of overlap so no phase is missed).
    const double top = peakPosition(x, n, (int)std::ceil(t - p), (int)std::floor(t) + 1, h);
    if (top - h < 0.0 || top + p + h > (double)(n - 1)) return false;
    const double step = p / points;
    for (int j = 0; j < points; ++j) out[j] = filteredAt(x, n, top + j * step, h);
    period = p;
    return true;
}

} // namespace sourcescope

// One per source. analyse() is called from any thread (not the audio thread); calls on one
// analyser are serialised by its mutex (readers only: the audio thread never takes it). The FFT
// and scratch are allocated in the constructor; analyse() does not allocate.
class SourceAnalyser {
public:
    SourceAnalyser() : raw((size_t)SourceRing::kReadable), scratch((size_t)SourceRing::kReadable) {}

    // `running`: the engine is rendering (else zeros). `active`: the source is sounding.
    // `lastNote`: MIDI note for Cycle mode when options.cycleHz is 0 (−1 = none); `detuneCents`: the
    // synth's detune, which moves every voice (NoteEventQueue::synthDetuneCents).
    void analyse(SourceRing& ring, bool running, bool active, int lastNote, SourceAnalysisOptions options,
                 SourceAnalysisResult& r, double detuneCents = 0.0) {
        options.sanitize();
        r = SourceAnalysisResult{};
        const int points = options.scopePoints;
        r.scopeCount = options.scope == SourceScopeMode::None ? 0 : points;
        r.scopeMode = options.scope == SourceScopeMode::None ? SourceScopeMode::None : SourceScopeMode::Window;
        if (!running) return;
        std::lock_guard<std::mutex> guard(lock);
        const double sr = ring.sampleRate();
        r.sampleRate = sr;
        r.active = active;
        const SourceRing::Levels levels = ring.readLevels();
        r.peak = levels.peak;
        r.rms = levels.rms;
        const int n = SourceRing::kReadable;
        ring.readLatest(raw.data(), n);
        if (options.bands) spectrum.analyse(raw.data() + (n - SpectrumAnalyser::kSize), sr, r.bands);
        if (options.scope == SourceScopeMode::Cycle) {
            double hz = options.cycleHz;
            if (!(hz > 0.0) && lastNote >= 0 && lastNote <= 127) {
                hz = 440.0 * std::pow(2.0, (lastNote - 69) / 12.0);
                // As PolySynthProcessor::renderVoices applies it.
                if (std::isfinite(detuneCents) && std::fabs(detuneCents) > 0.01)
                    hz *= std::pow(2.0, std::clamp(detuneCents, -1200.0, 1200.0) / 1200.0);
            }
            double period = 0.0;
            if (sourcescope::cycle(raw.data(), n, sr, hz, points, r.scope, period, scratch.data())) {
                r.scopeMode = SourceScopeMode::Cycle;
                r.scopePeriod = period;
                return;
            }
        }
        if (options.scope != SourceScopeMode::None) sourcescope::window(raw.data(), n, sr, options.windowMs, points, r.scope);
    }

private:
    std::mutex lock;
    SpectrumAnalyser spectrum;
    std::vector<float> raw, scratch;
};
