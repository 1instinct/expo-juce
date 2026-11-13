#pragma once

// Pure C++ (no Objective-C, no JUCE): the display-rate hint's math and the measured display rate.
// Used by ExpoJuceAudioEngine.mm (the main-thread display link and diagnostics) and the offline
// harness (test/harness, case "rate").
//
// The hint asks the display for a higher refresh rate on ProMotion screens: while it is above 60
// and the screen can go faster, a main-thread CADisplayLink asks for (min 80, max, preferred) =
// (80, hint, hint) clamped to the screen's maximum. iOS runs the display at the highest rate any
// link or animation asks for. At or below 60 the link is held to 60, so the link itself never
// raises the display rate (with CADisableMinimumFrameDurationOnPhone, a link without a range runs
// at the screen's maximum).

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace displayrate {

// The link's frame-rate range for a hint on a screen whose maximum is maxFps.
struct RateRange {
    bool raised = false;   // the hint holds the display above 60
    float minimum = 0.0f;
    float maximum = 0.0f;
    float preferred = 0.0f;
};

constexpr int kMaxHintHz = 240;
constexpr float kBaseHz = 60.0f;
constexpr float kRaisedMinimumHz = 80.0f;

// A hint from JS: non-finite or negative is 0 (no hint), anything else rounded and capped at 240.
inline int sanitizeHint(double hz) noexcept {
    if (!std::isfinite(hz) || hz <= 0.0) return 0;
    return (int)std::lround(std::min(hz, (double)kMaxHintHz));
}

// The range for `hint` (sanitized) on a screen with maximumFramesPerSecond maxFps (0: unknown).
// Above 60 on a screen that goes above 60: (min(80, target), target, target), target = min(hint,
// maxFps). Otherwise held to 60 (60, 60, 60) when the screen goes above 60, and no range at all
// (all 0: leave the link's default) on a 60 Hz screen or when maxFps is unknown.
inline RateRange rangeFor(int hint, int maxFps) noexcept {
    RateRange r;
    if (maxFps <= (int)kBaseHz) return r;
    if (hint > (int)kBaseHz) {
        const float target = (float)std::min(hint, maxFps);
        r.raised = true;
        r.minimum = std::min(kRaisedMinimumHz, target);
        r.maximum = target;
        r.preferred = target;
        return r;
    }
    r.minimum = kBaseHz;
    r.maximum = kBaseHz;
    r.preferred = kBaseHz;
    return r;
}

// The display link's measured rate: a rolling median of its frame duration (targetTimestamp −
// timestamp) over the last kSize ticks. One writer (the link's callback on the main thread), any
// number of readers; atomics only, no lock, no allocation.
class RateWindow {
public:
    static constexpr int kSize = 32;

    // Writer. durationSec: targetTimestamp − timestamp. Ignored unless finite and in (0, 1) s.
    void push(double durationSec) noexcept {
        if (!std::isfinite(durationSec) || durationSec <= 0.0 || durationSec >= 1.0) return;
        const uint64_t n = written.load(std::memory_order_relaxed);
        slots[(size_t)(n % kSize)].store(durationSec, std::memory_order_relaxed);
        written.store(n + 1, std::memory_order_release);
    }

    // Writer: forget the samples (the link stopped; a new run measures afresh).
    void clear() noexcept { written.store(0, std::memory_order_release); }

    // Any thread: 1 / median frame duration in Hz, to 0.1 Hz; 0 with no samples.
    double medianHz() const noexcept {
        const uint64_t n = written.load(std::memory_order_acquire);
        const int m = (int)std::min<uint64_t>(n, (uint64_t)kSize);
        if (m == 0) return 0.0;
        std::array<double, kSize> copy{};
        for (int k = 0; k < m; ++k) copy[(size_t)k] = slots[(size_t)k].load(std::memory_order_relaxed);
        std::sort(copy.begin(), copy.begin() + m);
        // Median: the middle value, or the mean of the two middle values.
        const double median = (m % 2 == 1) ? copy[(size_t)(m / 2)] : 0.5 * (copy[(size_t)(m / 2 - 1)] + copy[(size_t)(m / 2)]);
        if (!(median > 0.0)) return 0.0;
        return std::round(10.0 / median) / 10.0;
    }

private:
    std::atomic<double> slots[kSize] = {};
    std::atomic<uint64_t> written{0};
};

} // namespace displayrate
