#pragma once

// Pure C++ (no Objective-C): included by ExpoJuceAudioEngine.mm and by the offline harness
// (test/harness), so the harness exercises exactly the shipped device callback.

#include "JuceConfig.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "PolySynthProcessor.h" // diagPeakDecay
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

// Lightweight AudioIODeviceCallback that drives an AudioProcessorGraph.
//
// No allocation on the audio thread: the render buffer is sized in
// audioDeviceAboutToStart (>= 2x the device buffer, >= 1024 samples). A device block
// larger than that is rendered in chunks of at most the preallocated capacity.
// The graph's own render is allocation-free only with the vendored JUCE patch
// graph-output-buffer-no-realloc (src/scripts/juce-patches.cjs): unpatched,
// AudioProcessorGraph reallocates its output buffer whenever the block length changes.
// The harness counts allocations inside this callback and fails on any.
class GraphPlayer : public juce::AudioIODeviceCallback {
public:
    static constexpr int minCapacitySamples = 1024;

    void setProcessor(juce::AudioProcessor* p) { processor = p; }

    // Any thread: ask the audio thread to mix a 440 Hz test tone into the
    // device output (after the graph) for durationMs.
    void requestTestTone(double durationMs) {
        if (!(durationMs > 0.0)) return;
        toneRequestMs.store((int)std::min(durationMs, 10000.0), std::memory_order_relaxed);
    }

    // Preallocated render capacity (set in audioDeviceAboutToStart).
    int getCapacitySamples() const { return capacitySamples; }

    // Diagnostics: written on the audio thread, read from any thread (relaxed).
    std::atomic<uint64_t> callbackCount{0};
    std::atomic<uint64_t> graphProcessCount{0};
    std::atomic<uint64_t> nanCount{0};       // blocks with a non-finite output sample
    std::atomic<float> outputPeak{0.0f};     // decaying hold of block max |x| (graph output)
    std::atomic<uint64_t> toneSamplesMixed{0}; // cumulative test-tone samples mixed into output

    void audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                          int numInputChannels,
                                          float* const* outputChannelData,
                                          int numOutputChannels,
                                          int numSamples,
                                          const juce::AudioIODeviceCallbackContext& context) override {
        juce::ignoreUnused(inputChannelData, numInputChannels, context);
        callbackCount.fetch_add(1, std::memory_order_relaxed);

        const int chans = std::min(numOutputChannels, capacityChannels);
        if (!processor || chans <= 0 || capacitySamples <= 0) {
            for (int i = 0; i < numOutputChannels; ++i)
                if (outputChannelData[i] != nullptr)
                    juce::FloatVectorOperations::clear(outputChannelData[i], numSamples);
        } else {
            int done = 0;
            while (done < numSamples) {
                const int n = std::min(capacitySamples, numSamples - done);
                // Within the preallocated size: setSize only re-points channels (no allocation).
                buffer.setSize(chans, n, false, false, true);
                buffer.clear();
                juce::MidiBuffer midi;
                processor->processBlock(buffer, midi);
                graphProcessCount.fetch_add(1, std::memory_order_relaxed);
                for (int i = 0; i < chans; ++i)
                    if (outputChannelData[i] != nullptr)
                        juce::FloatVectorOperations::copy(outputChannelData[i] + done, buffer.getReadPointer(i), n);
                done += n;
            }
            for (int i = chans; i < numOutputChannels; ++i)
                if (outputChannelData[i] != nullptr)
                    juce::FloatVectorOperations::clear(outputChannelData[i], numSamples);
        }

        // Measure graph output (before the test tone is mixed in).
        float blockPeak = 0.0f;
        bool nonFinite = false;
        for (int ch = 0; ch < numOutputChannels; ++ch) {
            const float* data = outputChannelData[ch];
            if (data == nullptr) continue;
            for (int s = 0; s < numSamples; ++s) {
                const float v = data[s];
                if (!std::isfinite(v)) nonFinite = true;
                else blockPeak = std::max(blockPeak, std::fabs(v));
            }
        }
        if (nonFinite) nanCount.fetch_add(1, std::memory_order_relaxed);
        const float held = outputPeak.load(std::memory_order_relaxed) * diagPeakDecay(numSamples, deviceSampleRate);
        outputPeak.store(std::max(blockPeak, held), std::memory_order_relaxed);

        mixTestTone(outputChannelData, numOutputChannels, numSamples);
    }

    // Called with the device's callback lock held (no IO callback runs concurrently).
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override {
        const double deviceSr = device->getCurrentSampleRate();
        if (deviceSr > 0.0) deviceSampleRate = deviceSr;

        const int deviceBuffer = device->getCurrentBufferSizeSamples();
        const int outs = device->getActiveOutputChannels().countNumberOfSetBits();
        const int wantSamples = std::max(2 * std::max(deviceBuffer, 0), minCapacitySamples);
        const int wantChannels = std::max(outs, 2);
        if (wantSamples > capacitySamples || wantChannels > capacityChannels) {
            capacitySamples = std::max(wantSamples, capacitySamples);
            capacityChannels = std::max(wantChannels, capacityChannels);
            buffer.setSize(capacityChannels, capacitySamples, false, true, false);
        }

        if (processor) {
            processor->setPlayConfigDetails(0, outs,
                                            device->getCurrentSampleRate(),
                                            device->getCurrentBufferSizeSamples());
            processor->prepareToPlay(device->getCurrentSampleRate(),
                                     device->getCurrentBufferSizeSamples());
        }
    }

    void audioDeviceStopped() override {
        if (processor)
            processor->releaseResources();
    }

private:
    // Audio thread only.
    void mixTestTone(float* const* outputChannelData, int numOutputChannels, int numSamples) {
        const int requestedMs = toneRequestMs.exchange(0, std::memory_order_relaxed);
        if (requestedMs > 0)
            toneSamplesRemaining = (int)(requestedMs * deviceSampleRate / 1000.0);
        if (toneSamplesRemaining <= 0 || numOutputChannels <= 0) return;

        const double twoPi = juce::MathConstants<double>::twoPi;
        const double increment = twoPi * 440.0 / deviceSampleRate;
        const int n = std::min(toneSamplesRemaining, numSamples);
        for (int s = 0; s < n; ++s) {
            const float v = 0.2f * (float)std::sin(tonePhase);
            tonePhase += increment;
            if (tonePhase >= twoPi) tonePhase -= twoPi;
            for (int ch = 0; ch < numOutputChannels; ++ch)
                if (outputChannelData[ch] != nullptr)
                    outputChannelData[ch][s] += v;
        }
        toneSamplesRemaining -= n;
        toneSamplesMixed.fetch_add((uint64_t)n, std::memory_order_relaxed);
    }

    juce::AudioProcessor* processor = nullptr;
    juce::AudioBuffer<float> buffer;     // preallocated in audioDeviceAboutToStart
    int capacitySamples = 0;             // allocated samples per channel
    int capacityChannels = 0;            // allocated channels

    double deviceSampleRate = 48000.0;   // set in audioDeviceAboutToStart
    std::atomic<int> toneRequestMs{0};   // any thread → audio thread
    int toneSamplesRemaining = 0;        // audio thread only
    double tonePhase = 0.0;              // audio thread only
};
