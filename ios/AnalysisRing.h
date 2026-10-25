#pragma once

// Pure C++ (no Objective-C): output analysis for HUDs, written by GraphPlayer on the audio thread
// after the graph (what the device plays, effects and aux included), read from any thread.
// Lock-free; a reader may see a ring that is being written (tearing is fine for display).

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

class AnalysisRing {
public:
    static constexpr int kSize = 2048;       // ring of mono samples ((L + R) / 2)
    static constexpr int kScopePoints = 128; // decimated by kSize / kScopePoints = 16

    // Audio thread: one device block.
    void write(const float* const* channels, int numChannels, int numSamples) {
        if (numSamples <= 0) return;
        const float* l = numChannels > 0 ? channels[0] : nullptr;
        const float* r = numChannels > 1 ? channels[1] : l;
        uint32_t w = writeCount.load(std::memory_order_relaxed);
        float peak = 0.0f;
        double sumSquares = 0.0;
        int counted = 0;
        for (int i = 0; i < numSamples; ++i) {
            const float a = l != nullptr ? l[i] : 0.0f;
            const float b = r != nullptr ? r[i] : 0.0f;
            const float fa = std::isfinite(a) ? a : 0.0f, fb = std::isfinite(b) ? b : 0.0f;
            peak = std::max(peak, std::max(std::fabs(fa), std::fabs(fb)));
            sumSquares += (double)fa * fa + (double)fb * fb;
            counted += r != l ? 2 : 1;
            ring[w % kSize].store(0.5f * (fa + fb), std::memory_order_relaxed);
            ++w;
        }
        writeCount.store(w, std::memory_order_release);
        lastPeak.store(peak, std::memory_order_relaxed);
        lastRms.store(counted > 0 ? (float)std::sqrt(sumSquares / counted) : 0.0f, std::memory_order_relaxed);
    }

    // Any thread: the most recent kSize samples decimated to kScopePoints, oldest first; the last
    // point is the newest sample.
    void readScope(float* out) const {
        const uint32_t end = writeCount.load(std::memory_order_acquire);
        constexpr int stride = kSize / kScopePoints;
        for (int j = 0; j < kScopePoints; ++j) {
            const uint32_t index = end - 1u - (uint32_t)((kScopePoints - 1 - j) * stride);
            out[j] = ring[index % kSize].load(std::memory_order_relaxed);
        }
    }

    float peak() const { return lastPeak.load(std::memory_order_relaxed); } // last block, max |x|
    float rms() const { return lastRms.load(std::memory_order_relaxed); }   // last block, all channels
    uint32_t samplesWritten() const { return writeCount.load(std::memory_order_relaxed); }

private:
    std::atomic<float> ring[kSize] = {};
    std::atomic<uint32_t> writeCount{0};
    std::atomic<float> lastPeak{0.0f};
    std::atomic<float> lastRms{0.0f};
};
