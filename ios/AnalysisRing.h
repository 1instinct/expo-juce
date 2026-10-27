#pragma once

// Pure C++ (no Objective-C): output analysis for HUDs, written by GraphPlayer on the audio thread
// after the graph (what the device plays, effects and aux included), and the transport state as
// of the last rendered block (written by AuxProcessor). Read from any thread without locks; a
// reader may see a ring that is being written (tearing is fine for display).

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
        // Peak hold since the last read (the reader exchanges it with 0).
        float held = peakHold.load(std::memory_order_relaxed);
        while (peak > held && !peakHold.compare_exchange_weak(held, peak, std::memory_order_relaxed)) {}
        // Running energy / value count since launch: the reader takes the difference.
        energyTotal += sumSquares;
        valuesTotal += (uint64_t)numSamples * (stereo ? 2u : 1u);
        energy.store(energyTotal, std::memory_order_relaxed);
        values.store(valuesTotal, std::memory_order_release);
    }

    // Audio thread (AuxProcessor): transport as of the end of the block just rendered.
    void publishTransport(double beat, double bpm, bool playing) {
        transportBeat.store(beat, std::memory_order_relaxed);
        transportBpm.store(bpm, std::memory_order_relaxed);
        transportPlaying.store(playing, std::memory_order_relaxed);
    }

    // Audio thread (PolySynthProcessor): arpeggiator state as of the block just rendered.
    void publishArp(int step, int steps, int note, bool active) {
        arpStepValue.store(step, std::memory_order_relaxed);
        arpStepsValue.store(steps, std::memory_order_relaxed);
        arpNoteValue.store(note, std::memory_order_relaxed);
        arpActiveValue.store(active, std::memory_order_relaxed);
    }
    int arpStep() const { return arpStepValue.load(std::memory_order_relaxed); }
    int arpSteps() const { return arpStepsValue.load(std::memory_order_relaxed); }
    int arpNote() const { return arpNoteValue.load(std::memory_order_relaxed); }
    bool arpActive() const { return arpActiveValue.load(std::memory_order_relaxed); }

    // Control side: the engine is running (getAnalysis returns zeros otherwise).
    void setActive(bool on) { active.store(on, std::memory_order_release); }
    bool isActive() const { return active.load(std::memory_order_acquire); }

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

    // One reader (getAnalysis): max |x| and RMS (all channels) of everything written since the
    // previous call; 0 if nothing was written since.
    struct Levels { float peak; float rms; };
    Levels readLevels() {
        const float peak = peakHold.exchange(0.0f, std::memory_order_relaxed);
        const uint64_t v = values.load(std::memory_order_acquire);
        const double e = energy.load(std::memory_order_relaxed);
        float rms = 0.0f;
        if (v > readValues) {
            const double meanSquare = std::max(0.0, (e - readEnergy) / (double)(v - readValues));
            rms = (float)std::sqrt(meanSquare);
        }
        readValues = v;
        readEnergy = e;
        return { peak, rms };
    }

    double beat() const { return transportBeat.load(std::memory_order_relaxed); }
    double bpm() const { return transportBpm.load(std::memory_order_relaxed); }
    bool playing() const { return transportPlaying.load(std::memory_order_relaxed); }
    uint32_t samplesWritten() const { return writeCount.load(std::memory_order_relaxed); }

private:
    std::atomic<float> ring[kSize] = {};
    std::atomic<uint32_t> writeCount{0};
    std::atomic<float> peakHold{0.0f};
    double energyTotal = 0.0;   // audio thread only
    uint64_t valuesTotal = 0;   // audio thread only
    std::atomic<double> energy{0.0};
    std::atomic<uint64_t> values{0};
    double readEnergy = 0.0;    // reader only
    uint64_t readValues = 0;    // reader only
    std::atomic<double> transportBeat{0.0};
    std::atomic<double> transportBpm{120.0};
    std::atomic<bool> transportPlaying{false};
    std::atomic<int> arpStepValue{0};
    std::atomic<int> arpStepsValue{16};
    std::atomic<int> arpNoteValue{-1};
    std::atomic<bool> arpActiveValue{false};
    std::atomic<bool> active{false};
};
