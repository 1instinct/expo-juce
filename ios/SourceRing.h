#pragma once

// Pure C++ (no Objective-C, no JUCE): one audio source's tap for getSourceAnalysis. The instrument
// ring is written by FxChainProcessor at the end of its block (synth + drums after the effects);
// the backing ring by AuxProcessor right after it rendered the backing track and before it adds the
// metronome. The writer only stores samples and a few counters (relaxed atomics, no allocation,
// no lock); spectrum and scope are computed by the reader (SourceAnalysis.h). Read from any thread
// without locks; a reader may see a ring that is being written (tearing is fine for display).
//
// Longer than AnalysisRing (8192 samples, 170 ms at 48 kHz) so a one-cycle scope works down to
// about 25 Hz.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

class SourceRing {
public:
    static constexpr int kSize = 8192;          // ring of mono samples ((L + R) / 2)
    static constexpr int kReadable = kSize - 1024; // what a reader copies at most (the writer may advance meanwhile)
    static constexpr float kLoudFloor = 1e-4f;  // −80 dBFS: a block at or above it counts as sounding
    static constexpr double kLoudHoldSeconds = 0.1;

    // Audio thread: one block (any length).
    void write(const float* const* channels, int numChannels, int numSamples) {
        if (numSamples <= 0) return;
        const float* l = numChannels > 0 ? channels[0] : nullptr;
        const float* r = numChannels > 1 ? channels[1] : nullptr;
        const bool stereo = l != nullptr && r != nullptr;
        uint32_t w = writeCount.load(std::memory_order_relaxed);
        float peak = 0.0f;
        double sumSquares = 0.0;
        for (int i = 0; i < numSamples; ++i) {
            const float a = l != nullptr ? l[i] : 0.0f;
            const float b = stereo ? r[i] : a;
            const float fa = std::isfinite(a) ? a : 0.0f, fb = std::isfinite(b) ? b : 0.0f;
            peak = std::max(peak, std::max(std::fabs(fa), std::fabs(fb)));
            sumSquares += stereo ? (double)fa * fa + (double)fb * fb : (double)fa * fa;
            ring[w % kSize].store(0.5f * (fa + fb), std::memory_order_relaxed);
            ++w;
        }
        writeCount.store(w, std::memory_order_release);
        if (peak >= kLoudFloor) lastLoudEnd.store(w, std::memory_order_relaxed);
        float held = peakHold.load(std::memory_order_relaxed);
        while (peak > held && !peakHold.compare_exchange_weak(held, peak, std::memory_order_relaxed)) {}
        energyTotal += sumSquares;
        valuesTotal += (uint64_t)numSamples * (stereo ? 2u : 1u);
        energy.store(energyTotal, std::memory_order_relaxed);
        values.store(valuesTotal, std::memory_order_release);
    }

    // Audio thread (the backing ring's writer): the source is really sounding as of this block.
    void publishSounding(bool on) { soundingValue.store(on, std::memory_order_relaxed); }
    bool sounding() const { return soundingValue.load(std::memory_order_relaxed); }

    // Any thread: a block at or above kLoudFloor was written in the last kLoudHoldSeconds of samples
    // (counted in samples written, so it holds while nothing renders; the caller checks the engine).
    bool loudRecently() const {
        const uint32_t end = writeCount.load(std::memory_order_acquire);
        const uint32_t loud = lastLoudEnd.load(std::memory_order_relaxed);
        if (loud == 0) return false;
        return (uint32_t)(end - loud) <= (uint32_t)(kLoudHoldSeconds * std::max(sampleRate(), 1.0));
    }

    // Control side (the writer's prepareToPlay): the rate the ring is written at.
    void setSampleRate(double rate) { if (rate > 0.0) sampleRateValue.store(rate, std::memory_order_relaxed); }
    double sampleRate() const { return sampleRateValue.load(std::memory_order_relaxed); }

    // Any thread: the most recent n samples (n ≤ kReadable), oldest first.
    void readLatest(float* out, int n) const {
        n = std::clamp(n, 0, kReadable);
        const uint32_t end = writeCount.load(std::memory_order_acquire);
        for (int j = 0; j < n; ++j)
            out[j] = ring[(end - (uint32_t)n + (uint32_t)j) % kSize].load(std::memory_order_relaxed);
    }

    // One reader: max |x| and RMS (all channels) of everything written since the previous call.
    struct Levels { float peak; float rms; };
    Levels readLevels() {
        const float peak = peakHold.exchange(0.0f, std::memory_order_relaxed);
        const uint64_t v = values.load(std::memory_order_acquire);
        const double e = energy.load(std::memory_order_relaxed);
        float rms = 0.0f;
        if (v > readValues) rms = (float)std::sqrt(std::max(0.0, (e - readEnergy) / (double)(v - readValues)));
        readValues = v;
        readEnergy = e;
        return { peak, rms };
    }

    uint32_t samplesWritten() const { return writeCount.load(std::memory_order_relaxed); }

private:
    std::atomic<float> ring[kSize] = {};
    std::atomic<uint32_t> writeCount{0};
    std::atomic<uint32_t> lastLoudEnd{0};
    std::atomic<float> peakHold{0.0f};
    double energyTotal = 0.0;   // audio thread only
    uint64_t valuesTotal = 0;   // audio thread only
    std::atomic<double> energy{0.0};
    std::atomic<uint64_t> values{0};
    double readEnergy = 0.0;    // reader only
    uint64_t readValues = 0;    // reader only
    std::atomic<bool> soundingValue{false};
    std::atomic<double> sampleRateValue{48000.0};
};
