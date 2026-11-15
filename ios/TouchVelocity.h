#pragma once

// Pure C++ (no Objective-C, no Apple headers): the touch velocity estimator of the native pad
// (JucePadView, wired by ExpoJuceTouchVelocity.mm) and its accelerometer ring. Deterministic, so
// the offline harness (test/harness, case "tv") tests it on the host.
//
// At touch-down the pad turns what it knows at that moment into a MIDI velocity 1–127:
//   - force (UITouch.force / maximumPossibleForce) when the hardware reports it, alone;
//   - otherwise a weighted blend of the contact radius (UITouch.majorRadius) and the device
//     impact: the largest deviation of the acceleration vector from its short pre-touch baseline.
// Nothing waits for later samples: the note fires in touchesBegan, so the estimate costs one ring
// scan of a few dozen samples. The peak after the touch (impactLate) is for diagnostics only.
//
// Threads: the ring has one writer (the motion queue, serial) and any number of readers (the main
// thread at touch-down, diagnostics). Writer and readers only touch atomics: no lock, no
// allocation after construction. Everything else here is a pure function or main-thread state.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>

// Test seam: the offline harness defines TOUCH_VELOCITY_READ_HOOK as the name of a function it
// provides; AccelRing::readSince() calls it after copying a slot's data and before re-checking the
// slot's sequence number, so a test can overwrite the slot at exactly that point (a torn read).
// Undefined in the module: no code.
#ifdef TOUCH_VELOCITY_READ_HOOK
void TOUCH_VELOCITY_READ_HOOK();
#endif

namespace touchvel {

// One accelerometer sample: t in seconds on the boot clock (CMAccelerometerData.timestamp, the
// clock of UITouch.timestamp and CACurrentMediaTime), acceleration in g.
struct AccelSample {
    double t = 0.0;
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

// Single-producer ring of the last kSize samples (2.56 s at 100 Hz). Each slot carries a sequence
// number: odd while the writer fills it, 2·(index + 1) once sample `index` is complete. A reader
// keeps a slot only when the number is the one it expects before and after copying the data, so a
// slot the writer overwrote meanwhile (it lapped the reader) is discarded, never mixed.
class AccelRing {
public:
    static constexpr int kSize = 256;

    // Writer (one thread at a time). Non-finite values are dropped.
    void push(double t, double x, double y, double z) noexcept {
        if (!std::isfinite(t) || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return;
        const uint64_t index = head.load(std::memory_order_relaxed);
        Slot& slot = slots[(size_t)(index % kSize)];
        slot.seq.store(2 * index + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        slot.t.store(t, std::memory_order_relaxed);
        slot.x.store(x, std::memory_order_relaxed);
        slot.y.store(y, std::memory_order_relaxed);
        slot.z.store(z, std::memory_order_relaxed);
        slot.seq.store(2 * index + 2, std::memory_order_release);
        head.store(index + 1, std::memory_order_release);
    }

    // Samples ever pushed.
    uint64_t pushed() const noexcept { return head.load(std::memory_order_acquire); }

    // Copies the samples with t >= fromT into out, newest first, at most cap of them; returns how
    // many. Stops at the first sample older than fromT, at the first slot that was overwritten or
    // is being written (everything older is gone too), or at cap.
    int readSince(double fromT, AccelSample* out, int cap) const noexcept {
        if (out == nullptr || cap <= 0) return 0;
        const uint64_t end = head.load(std::memory_order_acquire);
        const uint64_t oldest = end > (uint64_t)kSize ? end - (uint64_t)kSize : 0;
        int n = 0;
        for (uint64_t index = end; index > oldest && n < cap;) {
            --index;
            const Slot& slot = slots[(size_t)(index % kSize)];
            const uint64_t expected = 2 * index + 2;
            if (slot.seq.load(std::memory_order_acquire) != expected) break;
            AccelSample s;
            s.t = slot.t.load(std::memory_order_relaxed);
            s.x = slot.x.load(std::memory_order_relaxed);
            s.y = slot.y.load(std::memory_order_relaxed);
            s.z = slot.z.load(std::memory_order_relaxed);
#ifdef TOUCH_VELOCITY_READ_HOOK
            TOUCH_VELOCITY_READ_HOOK();
#endif
            std::atomic_thread_fence(std::memory_order_acquire);
            if (slot.seq.load(std::memory_order_relaxed) != expected) break; // torn: discarded
            if (s.t < fromT) break;
            out[n++] = s;
        }
        return n;
    }

private:
    struct Slot {
        std::atomic<uint64_t> seq{0};
        std::atomic<double> t{0.0};
        std::atomic<double> x{0.0};
        std::atomic<double> y{0.0};
        std::atomic<double> z{0.0};
    };
    Slot slots[kSize];
    std::atomic<uint64_t> head{0};
};

// The impact windows, relative to the touch's timestamp t (seconds).
struct Windows {
    double baselineFromSec = 0.150; // baseline: mean of the samples in [t − 0.150, t − 0.030]
    double baselineToSec = 0.030;
    double impactFromSec = 0.020;   // impact: peak deviation over [t − 0.020, now]
    double staleSec = 0.100;        // newest sample older than this at `now`: impact invalid
    double lateSec = 0.050;         // impactLate: peak deviation over [t, t + 0.050]
};

// Samples a touch-down needs: the scan reaches back to t − baselineFromSec.
inline double scanFrom(double touchT, const Windows& w = Windows()) { return touchT - w.baselineFromSec; }

struct Impact {
    bool valid = false;
    double peak = 0.0;          // g, when valid
    bool hasSamples = false;    // any sample at all (else the accelerometer is off or silent)
    double ageSec = 0.0;        // now − newest sample, when hasSamples
    int baselineCount = 0;
    int windowCount = 0;
    bool pending = false;       // impactLate only: its window has not closed yet
};

namespace detail {
// Mean of the samples in [touchT − from, touchT − to]; false when there are none.
inline bool baselineOf(const AccelSample* s, int n, double touchT, const Windows& w, double& bx, double& by,
                       double& bz, int& count) {
    bx = by = bz = 0.0;
    count = 0;
    const double lo = touchT - w.baselineFromSec;
    const double hi = touchT - w.baselineToSec;
    for (int i = 0; i < n; ++i) {
        if (s[i].t < lo || s[i].t > hi) continue;
        bx += s[i].x;
        by += s[i].y;
        bz += s[i].z;
        ++count;
    }
    if (count == 0) return false;
    bx /= count;
    by /= count;
    bz /= count;
    return true;
}

inline double deviation(const AccelSample& s, double bx, double by, double bz) {
    const double dx = s.x - bx;
    const double dy = s.y - by;
    const double dz = s.z - bz;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
} // namespace detail

// Impact of a touch at touchT, from what is known at `now` (the handler's entry). s: the samples
// (any order), n of them. Invalid when there are no samples, the newest is older than staleSec,
// or the baseline or impact window is empty.
inline Impact computeImpact(const AccelSample* s, int n, double touchT, double now, const Windows& w = Windows()) {
    Impact r;
    if (s == nullptr || n <= 0) return r;
    double newest = -std::numeric_limits<double>::infinity();
    for (int i = 0; i < n; ++i) newest = std::max(newest, s[i].t);
    r.hasSamples = true;
    r.ageSec = std::max(0.0, now - newest);
    double bx, by, bz;
    const bool haveBaseline = detail::baselineOf(s, n, touchT, w, bx, by, bz, r.baselineCount);
    if (r.ageSec > w.staleSec || !haveBaseline) return r;
    const double from = touchT - w.impactFromSec;
    double peak = 0.0;
    for (int i = 0; i < n; ++i) {
        if (s[i].t < from || s[i].t > now) continue;
        peak = std::max(peak, detail::deviation(s[i], bx, by, bz));
        ++r.windowCount;
    }
    if (r.windowCount == 0) return r;
    r.valid = true;
    r.peak = peak;
    return r;
}

// Diagnostics only: the peak deviation in [touchT, touchT + lateSec] against the same baseline.
// pending (and invalid) until a sample after the window exists; invalid when the baseline or the
// window is empty.
inline Impact computeImpactLate(const AccelSample* s, int n, double touchT, const Windows& w = Windows()) {
    Impact r;
    if (s == nullptr || n <= 0) {
        r.pending = true;
        return r;
    }
    double newest = -std::numeric_limits<double>::infinity();
    for (int i = 0; i < n; ++i) newest = std::max(newest, s[i].t);
    r.hasSamples = true;
    const double to = touchT + w.lateSec;
    if (newest <= to) {
        r.pending = true;
        return r;
    }
    double bx, by, bz;
    if (!detail::baselineOf(s, n, touchT, w, bx, by, bz, r.baselineCount)) return r;
    double peak = 0.0;
    for (int i = 0; i < n; ++i) {
        if (s[i].t < touchT || s[i].t > to) continue;
        peak = std::max(peak, detail::deviation(s[i], bx, by, bz));
        ++r.windowCount;
    }
    if (r.windowCount == 0) return r;
    r.valid = true;
    r.peak = peak;
    return r;
}

// Samples per second over the last windowSec before `now` (s newest first or any order); 0 with
// fewer than two samples there.
inline double measuredHz(const AccelSample* s, int n, double now, double windowSec = 1.0) {
    double lo = std::numeric_limits<double>::infinity();
    double hi = -std::numeric_limits<double>::infinity();
    int count = 0;
    for (int i = 0; i < n; ++i) {
        if (s[i].t < now - windowSec || s[i].t > now) continue;
        lo = std::min(lo, s[i].t);
        hi = std::max(hi, s[i].t);
        ++count;
    }
    if (count < 2 || !(hi > lo)) return 0.0;
    return (double)(count - 1) / (hi - lo);
}

// ── Settings ──────────────────────────────────────────────────────────────────

enum class Curve : int { Soft = 0, Normal = 1, Hard = 2 };

// γ of velocity = min + (max − min) · strength^γ.
inline double curveGamma(Curve c) {
    switch (c) {
        case Curve::Soft: return 0.6;
        case Curve::Hard: return 1.6;
        case Curve::Normal: break;
    }
    return 1.0;
}

// Uncalibrated defaults are first guesses; a calibration replaces them.
struct Calibration {
    double radiusSoft = 20.0;   // pt
    double radiusHard = 34.0;
    double impactSoft = 0.02;   // g
    double impactHard = 0.25;
    double radiusWeight = 0.4;
    double impactWeight = 0.6;
};

struct Settings {
    Curve curve = Curve::Normal;
    int minVelocity = 20;
    int maxVelocity = 127;
    Calibration calibration;
};

constexpr double kRadiusMin = 1.0, kRadiusMax = 200.0;   // pt
constexpr double kImpactMin = 0.001, kImpactMax = 16.0;  // g

namespace detail {
inline double finiteOr(double v, double fallback) { return std::isfinite(v) ? v : fallback; }
} // namespace detail

// Non-finite fields take the default; radii are clamped to 1–200 pt, impacts to 0.001–16 g,
// weights to 0–1. A signal whose hard end is not above its soft end cannot be normalised: its
// weight becomes 0.
inline Calibration sanitizeCalibration(const Calibration& in) {
    const Calibration d;
    Calibration c;
    c.radiusSoft = std::clamp(detail::finiteOr(in.radiusSoft, d.radiusSoft), kRadiusMin, kRadiusMax);
    c.radiusHard = std::clamp(detail::finiteOr(in.radiusHard, d.radiusHard), kRadiusMin, kRadiusMax);
    c.impactSoft = std::clamp(detail::finiteOr(in.impactSoft, d.impactSoft), kImpactMin, kImpactMax);
    c.impactHard = std::clamp(detail::finiteOr(in.impactHard, d.impactHard), kImpactMin, kImpactMax);
    c.radiusWeight = std::clamp(detail::finiteOr(in.radiusWeight, d.radiusWeight), 0.0, 1.0);
    c.impactWeight = std::clamp(detail::finiteOr(in.impactWeight, d.impactWeight), 0.0, 1.0);
    if (!(c.radiusHard > c.radiusSoft)) c.radiusWeight = 0.0;
    if (!(c.impactHard > c.impactSoft)) c.impactWeight = 0.0;
    return c;
}

// A velocity bound from JS: rounded and clamped to 1–127; non-finite → fallback.
inline int sanitizeVelocity(double v, int fallback) {
    if (!std::isfinite(v)) return fallback;
    return (int)std::clamp(std::round(v), 1.0, 127.0);
}

// min and max in order (swapped when min > max).
inline void orderVelocityBounds(Settings& s) {
    s.minVelocity = std::clamp(s.minVelocity, 1, 127);
    s.maxVelocity = std::clamp(s.maxVelocity, 1, 127);
    if (s.minVelocity > s.maxVelocity) std::swap(s.minVelocity, s.maxVelocity);
}

// ── Estimate ──────────────────────────────────────────────────────────────────

// What UIKit reports for one touch at touch-down.
struct TouchInput {
    double radius = 0.0;          // majorRadius, pt
    double radiusTolerance = 0.0; // majorRadiusTolerance, pt
    double force = 0.0;
    double maxForce = 0.0;        // maximumPossibleForce; 0 = no force sensing
};

inline double clamp01(double v) { return std::isfinite(v) ? std::clamp(v, 0.0, 1.0) : 0.0; }

// x between lo (0) and hi (1), clamped; 0 when hi is not above lo.
inline double normLinear(double x, double lo, double hi) {
    if (!(hi > lo) || !std::isfinite(x)) return 0.0;
    return clamp01((x - lo) / (hi - lo));
}

// As normLinear on a log scale (lo, hi > 0); x <= 0 → 0.
inline double normLog(double x, double lo, double hi) {
    if (!(hi > lo) || !(lo > 0.0) || !std::isfinite(x)) return 0.0;
    if (x <= 0.0) return 0.0;
    return clamp01(std::log(x / lo) / std::log(hi / lo));
}

inline bool forceValid(const TouchInput& t) {
    return std::isfinite(t.maxForce) && t.maxForce > 0.0 && std::isfinite(t.force) && t.force >= 0.0;
}

// Strength 0–1 of one touch. Force when valid, alone. Otherwise the weighted mean of the radius
// term and (when the impact is valid) the impact term; an invalid impact or radius drops its
// term. With no usable term (every weight 0, or nothing valid) the strength is 0.5.
inline double strengthOf(const TouchInput& t, const Impact& impact, const Calibration& c) {
    if (forceValid(t)) return clamp01(t.force / t.maxForce);
    double sum = 0.0;
    double weights = 0.0;
    if (c.radiusWeight > 0.0 && std::isfinite(t.radius) && t.radius > 0.0) {
        sum += c.radiusWeight * normLinear(t.radius, c.radiusSoft, c.radiusHard);
        weights += c.radiusWeight;
    }
    if (c.impactWeight > 0.0 && impact.valid) {
        sum += c.impactWeight * normLog(impact.peak, c.impactSoft, c.impactHard);
        weights += c.impactWeight;
    }
    if (!(weights > 0.0)) return 0.5;
    return clamp01(sum / weights);
}

// round(min + (max − min) · strength^γ), clamped to 1–127.
inline int velocityFor(double strength, const Settings& s) {
    const double shaped = std::pow(clamp01(strength), curveGamma(s.curve));
    const double v = (double)s.minVelocity + (double)(s.maxVelocity - s.minVelocity) * shaped;
    return (int)std::clamp(std::round(v), 1.0, 127.0);
}

struct Estimate {
    int velocity = 0;
    double strength = 0.0;
    TouchInput touch;  // the strongest touch
    Impact impact;
    bool forceUsed = false;
};

// The strongest of `count` new touches (the first on a tie) with the impact they share.
inline Estimate estimate(const TouchInput* touches, int count, const Impact& impact, const Settings& s) {
    Estimate e;
    e.impact = impact;
    bool any = false;
    for (int i = 0; i < count; ++i) {
        const double strength = strengthOf(touches[i], impact, s.calibration);
        if (!any || strength > e.strength) {
            any = true;
            e.strength = strength;
            e.touch = touches[i];
            e.forceUsed = forceValid(touches[i]);
        }
    }
    if (!any) e.strength = strengthOf(TouchInput(), impact, s.calibration);
    e.velocity = velocityFor(e.strength, s);
    return e;
}

// ── Recent estimates (diagnostics) ────────────────────────────────────────────

struct Record {
    Estimate estimate;
    double touchT = 0.0;
    Impact late;           // impactLate; pending until filled
    bool lateDone = false;
};

// The last kSize estimates; one thread writes, and readers hold the caller's lock. Preallocated.
class RecentLog {
public:
    static constexpr int kSize = 16;

    void push(const Record& r) noexcept {
        items[(size_t)(count % kSize)] = r;
        ++count;
    }
    int size() const noexcept { return (int)std::min<uint64_t>(count, kSize); }
    uint64_t total() const noexcept { return count; }
    // i = 0 is the oldest kept.
    Record& at(int i) noexcept { return items[(size_t)((count - (uint64_t)size() + (uint64_t)i) % kSize)]; }
    const Record& at(int i) const noexcept {
        return items[(size_t)((count - (uint64_t)size() + (uint64_t)i) % kSize)];
    }

private:
    Record items[kSize];
    uint64_t count = 0;
};

} // namespace touchvel
