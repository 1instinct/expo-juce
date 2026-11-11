#pragma once

// Pure C++ (no Objective-C): used by ExpoJuceAudioEngine.mm (-analysis) and the offline harness
// (test/harness).
//
// 24 log-spaced spectrum bands from the last 2048 output samples (AnalysisRing::readRaw), for
// HUDs. Computed on the reading thread (one 2048-point FFT per call, about 50 µs), never on the
// audio thread. Constructed once: the FFT and its buffers are allocated then; analyse() does not
// allocate.
//
// Hann window, juce::dsp::FFT order 11, magnitude × 4 / N (the Hann coherent gain is 0.5) so a
// full-scale sine reads 0 dBFS. Band edges: edge_0 = 40 Hz, edge_i = max(40 × 400^(i/24),
// edge_(i−1) + sampleRate / 2048) for i in 1..23, edge_24 = 16 kHz: the first bands are one FFT
// bin wide (23.4 Hz at 48 kHz) until the geometric spacing overtakes (around 140 Hz), so no two
// bands read the same bin. A band's value is the loudest bin whose centre lies inside it (the
// nearest bin to the band centre if none does), in dB mapped −60..0 → 0..1 and clamped. The app
// computes the same edge table.

#include "JuceConfig.h"
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <cmath>
#include <vector>

class SpectrumAnalyser {
public:
    static constexpr int kOrder = 11;
    static constexpr int kSize = 1 << kOrder; // 2048
    static constexpr int kBands = 24;
    static constexpr float kFloorDb = -60.0f;

    SpectrumAnalyser()
        : fft(kOrder), window((size_t)kSize), data((size_t)kSize * 2, 0.0f) {
        // Periodic Hann: coherent gain 0.5.
        for (int n = 0; n < kSize; ++n)
            window[(size_t)n] = (float)(0.5 - 0.5 * std::cos(juce::MathConstants<double>::twoPi * n / kSize));
    }

    // `edges25[i]`, `edges25[i + 1]`: band i covers [edges[i], edges[i + 1]) Hz.
    static void bandEdges(double sampleRate, float* edges25) {
        const double bin = sampleRate > 0.0 ? sampleRate / kSize : 48000.0 / kSize;
        edges25[0] = 40.0f;
        for (int i = 1; i < kBands; ++i) {
            const double geometric = 40.0 * std::pow(400.0, (double)i / kBands);
            edges25[i] = (float)std::max(geometric, (double)edges25[i - 1] + bin);
        }
        edges25[kBands] = (float)std::max(16000.0, (double)edges25[kBands - 1] + bin);
    }

    // mono: kSize samples (oldest first). bands: kBands values in 0..1. No allocation.
    void analyse(const float* mono, double sampleRate, float* bands) {
        float* d = data.data();
        for (int n = 0; n < kSize; ++n) {
            const float v = mono[n];
            d[n] = (std::isfinite(v) ? v : 0.0f) * window[(size_t)n];
        }
        std::fill(d + kSize, d + 2 * kSize, 0.0f);
        fft.performFrequencyOnlyForwardTransform(d, true); // d[0 .. kSize/2] = magnitudes
        const float scale = 4.0f / (float)kSize;
        const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
        const double binHz = sr / kSize;
        float edges[kBands + 1];
        bandEdges(sr, edges);
        const int maxBin = kSize / 2;
        for (int b = 0; b < kBands; ++b) {
            // Bins whose centre k × binHz lies in [edge_b, edge_(b+1)).
            int lo = (int)std::ceil(edges[b] / binHz - 1e-9);
            int hi = (int)std::ceil(edges[b + 1] / binHz - 1e-9) - 1;
            if (hi < lo) lo = hi = (int)std::lround(0.5 * (edges[b] + edges[b + 1]) / binHz);
            lo = std::clamp(lo, 1, maxBin);
            hi = std::clamp(hi, lo, maxBin);
            float peak = 0.0f;
            for (int k = lo; k <= hi; ++k) peak = std::max(peak, d[k]);
            const float magnitude = peak * scale;
            const float db = 20.0f * std::log10(std::max(magnitude, 1e-9f));
            bands[b] = std::clamp((db - kFloorDb) / -kFloorDb, 0.0f, 1.0f);
        }
    }

private:
    juce::dsp::FFT fft;
    std::vector<float> window;
    std::vector<float> data; // FFT in/out, 2 × kSize
};
