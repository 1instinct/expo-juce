#pragma once

// Pure C++ (no Objective-C, no JUCE): rolling latency statistics for the touch → sound legs and
// the main-thread stall probe (its gap window and its log of the last stalls). Used by PolySynthProcessor (audio thread), ExpoJuceAudioEngine.mm
// (main thread, diagnostics) and the offline harness (test/harness, case "lat").
//
// Every window has exactly one writer thread and any number of readers. Writers only store
// atomics: no lock, no allocation, so the audio thread can write. Readers copy the slots and
// compute on the copy (a fixed-size array on the stack, no allocation either). A reader racing
// the writer can see a slot that is newer than the count it read; for diagnostics that is fine.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>

// last / p50 / p95 / max over the values in a window; count = values ever pushed (not just the
// ones still in the window). Empty window: count 0, the others -1.
struct LatencySummary {
    double last = -1.0;
    double p50 = -1.0;
    double p95 = -1.0;
    double max = -1.0;
    uint64_t count = 0;
};

// Nearest-rank percentile of `sorted` (ascending, n > 0): the smallest value with at least p of
// the values at or below it.
inline double latencyPercentile(const double* sorted, int n, double p) {
    int rank = (int)std::ceil(p * (double)n);
    rank = std::clamp(rank, 1, n);
    return sorted[rank - 1];
}

// Summary of `values` (n of them, oldest first; the last one is `last`). `values` is sorted in
// place. count is set by the caller.
inline LatencySummary summariseLatencies(double* values, int n) {
    LatencySummary s;
    if (n <= 0) return s;
    s.last = values[n - 1];
    std::sort(values, values + n);
    s.p50 = latencyPercentile(values, n, 0.50);
    s.p95 = latencyPercentile(values, n, 0.95);
    s.max = values[n - 1];
    return s;
}

// The last kSize values of one measurement (milliseconds). One writer thread.
class LatencyWindow {
public:
    static constexpr int kSize = 64;

    // Writer thread only. Non-finite values are ignored.
    void push(double ms) noexcept {
        if (!std::isfinite(ms)) return;
        const uint64_t n = written.load(std::memory_order_relaxed);
        slots[(size_t)(n % kSize)].store(ms, std::memory_order_relaxed);
        written.store(n + 1, std::memory_order_release);
    }

    // Any thread.
    LatencySummary summary() const noexcept {
        const uint64_t n = written.load(std::memory_order_acquire);
        const int m = (int)std::min<uint64_t>(n, (uint64_t)kSize);
        std::array<double, kSize> copy{};
        // Oldest first: the window holds values n - m … n - 1.
        for (int k = 0; k < m; ++k)
            copy[(size_t)k] = slots[(size_t)((n - (uint64_t)m + (uint64_t)k) % kSize)].load(std::memory_order_relaxed);
        LatencySummary s = summariseLatencies(copy.data(), m);
        s.count = n;
        return s;
    }

    uint64_t count() const noexcept { return written.load(std::memory_order_acquire); }

private:
    std::atomic<double> slots[kSize] = {};
    std::atomic<uint64_t> written{0};
};

// Touch → sound, per leg, for notes and drum hits started by a native touch (JucePadView).
// Process lifetime in the engine (outside the objects a rebuild replaces).
struct TouchLatencyStats {
    // Main thread: touchesBegan entry − UITouch.timestamp (iOS delivering the touch to the view).
    LatencyWindow delivery;
    // Audio thread: touchesBegan entry → note queue push (the view's own code).
    LatencyWindow handler;
    // Audio thread: note queue push → the render block that consumed it.
    LatencyWindow queueToRender;
    // Audio thread: UITouch.timestamp → the render block that consumed it (the whole in-app path).
    LatencyWindow touchToRender;
};

// Main-thread stall probe statistics. The writer is the display-link callback on the main thread:
// addGap(now, gap) for every tick after the first. Gaps are kept as the largest per second for
// the last kSeconds seconds (one slot per second, reused round-robin), so the reader gets the
// longest gap of a sliding window without a list of samples.
class StallWindow {
public:
    static constexpr int kSeconds = 10;
    static constexpr double kStallMs = 50.0;

    // Writer (main thread). nowSec: a monotonic clock in seconds (CACurrentMediaTime), gapMs: time
    // since the previous tick.
    void addGap(double nowSec, double gapMs) noexcept {
        if (!std::isfinite(nowSec) || !std::isfinite(gapMs) || gapMs < 0.0) return;
        const int64_t second = (int64_t)std::floor(nowSec);
        const size_t slot = (size_t)(((second % kSeconds) + kSeconds) % kSeconds);
        if (slotSecond[slot].load(std::memory_order_relaxed) != second) {
            slotMax[slot].store(gapMs, std::memory_order_relaxed);
            slotSecond[slot].store(second, std::memory_order_release);
        } else if (gapMs > slotMax[slot].load(std::memory_order_relaxed)) {
            slotMax[slot].store(gapMs, std::memory_order_relaxed);
        }
        if (gapMs > kStallMs) stalls.fetch_add(1, std::memory_order_relaxed);
        ticks.fetch_add(1, std::memory_order_relaxed);
    }

    // Any thread: the longest gap that ended in the last kSeconds seconds before nowSec; 0 if none.
    double maxGapMs(double nowSec) const noexcept {
        if (!std::isfinite(nowSec)) return 0.0;
        const int64_t second = (int64_t)std::floor(nowSec);
        double best = 0.0;
        for (size_t i = 0; i < (size_t)kSeconds; ++i) {
            const int64_t s = slotSecond[i].load(std::memory_order_acquire);
            if (s > second - kSeconds && s <= second) best = std::max(best, slotMax[i].load(std::memory_order_relaxed));
        }
        return best;
    }

    // Gaps over kStallMs since the probe was created (cumulative).
    uint64_t stallCount() const noexcept { return stalls.load(std::memory_order_relaxed); }
    uint64_t tickCount() const noexcept { return ticks.load(std::memory_order_relaxed); }

private:
    std::atomic<int64_t> slotSecond[kSeconds] = { {INT64_MIN}, {INT64_MIN}, {INT64_MIN}, {INT64_MIN}, {INT64_MIN},
                                                  {INT64_MIN}, {INT64_MIN}, {INT64_MIN}, {INT64_MIN}, {INT64_MIN} };
    std::atomic<double> slotMax[kSeconds] = {};
    std::atomic<uint64_t> stalls{0};
    std::atomic<uint64_t> ticks{0};
};

// The last kSize main-thread stalls (gaps over StallWindow::kStallMs), each with its start time, so
// their cadence can be read: a stall every second points at a periodic main-thread job. The writer
// is the stall probe's display-link callback (main thread): a fixed ring of atomics, no lock, no
// allocation. A reader racing the writer can pair one slot's start with a newer duration; for
// diagnostics that is fine.
struct StallRecord {
    double atSec = 0.0; // start of the gap (the tick before it), monotonic seconds
    double ms = 0.0;    // its length
};

class StallLog {
public:
    static constexpr int kSize = 32;

    // Writer (main thread). Records the gap ending at endSec if it is longer than kStallMs.
    void addGap(double endSec, double gapMs) noexcept {
        if (!std::isfinite(endSec) || !std::isfinite(gapMs) || !(gapMs > StallWindow::kStallMs)) return;
        const uint64_t n = written.load(std::memory_order_relaxed);
        const size_t slot = (size_t)(n % kSize);
        startSec[slot].store(endSec - gapMs / 1000.0, std::memory_order_relaxed);
        lengthMs[slot].store(gapMs, std::memory_order_relaxed);
        written.store(n + 1, std::memory_order_release);
    }

    // Any thread: copies up to kSize records into `out`, oldest first; returns how many.
    int copy(StallRecord* out) const noexcept {
        const uint64_t n = written.load(std::memory_order_acquire);
        const int m = (int)std::min<uint64_t>(n, (uint64_t)kSize);
        for (int k = 0; k < m; ++k) {
            const size_t slot = (size_t)((n - (uint64_t)m + (uint64_t)k) % kSize);
            out[k].atSec = startSec[slot].load(std::memory_order_relaxed);
            out[k].ms = lengthMs[slot].load(std::memory_order_relaxed);
        }
        return m;
    }

    // Median (nearest rank) of the gaps between consecutive stall starts in `records` (n of them,
    // oldest first), in ms; 0 with fewer than two records. Intervals that are not positive (a
    // reader that raced the writer) are skipped.
    static double intervalP50Ms(const StallRecord* records, int n) noexcept {
        std::array<double, kSize> intervals{};
        int m = 0;
        for (int k = 1; k < n && m < kSize; ++k) {
            const double d = (records[k].atSec - records[k - 1].atSec) * 1000.0;
            if (std::isfinite(d) && d > 0.0) intervals[(size_t)m++] = d;
        }
        if (m == 0) return 0.0;
        std::sort(intervals.begin(), intervals.begin() + m);
        return latencyPercentile(intervals.data(), m, 0.50);
    }

    uint64_t count() const noexcept { return written.load(std::memory_order_acquire); }

private:
    std::atomic<double> startSec[kSize] = {};
    std::atomic<double> lengthMs[kSize] = {};
    std::atomic<uint64_t> written{0};
};
