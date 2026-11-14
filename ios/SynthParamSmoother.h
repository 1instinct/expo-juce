#pragma once

#include "JuceConfig.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <algorithm>
#include <cmath>

// Targets for the synth's continuous parameters, as stored by the setters (already clamped).
struct SynthParamTargets {
    float cutoffHz;     // 20 … 20000
    float resonance;    // 0 … 1
    float detuneCents;  // -1200 … 1200
    float level;        // master level 0 … 1
    float sustain;      // envelope sustain level 0 … 1
};

// Per-sample smoothing of the synth's continuous parameters. Audio thread only (no allocation,
// no locks); the setters store atomics, the processor copies them in here once per block.
//
// Parameters set from a controller arrive as steps at the controller's rate (often 25–30 Hz,
// i.e. every 33–40 ms). Applied directly, each step is an audible click ("zipper noise") on a
// sweep. Each step here starts a ramp of kRampSeconds from the current value:
//   - cutoff and pitch are ramped multiplicatively (a constant ratio per sample: linear in
//     octaves, which is how a frequency sweep is heard). Detune is ramped as the pitch ratio
//     2^(cents / 1200), which is the same as a linear ramp in cents.
//   - resonance, master level and sustain are ramped linearly.
// 40 ms is just above a 25–30 Hz controller's step interval (33–40 ms), so each new step arrives
// while the previous ramp is still running and a sweep becomes one continuous trajectory with
// no flat plateaus between steps (plateaus would themselves be heard as a 25–30 Hz flutter). It
// is the shortest ramp that does that, so the lag behind the control stays as small as possible,
// and it sits under the effect chain's 50 ms parameter ramp.
class SynthParamSmoother {
public:
    static constexpr double kRampSeconds = 0.040;
    // Samples produced per next() call; the processor renders its block in chunks of this size.
    static constexpr int kChunk = 64;

    static float centsToRatio(float cents) { return std::exp2(cents / 1200.0f); }

    // Not on the audio thread while it renders (prepareToPlay). Sets the ramp length for the
    // sample rate and jumps to the targets.
    void prepare(double sampleRate, const SynthParamTargets& t) {
        for (auto* s : { &cutoffHz, &pitchRatio }) s->reset(sampleRate, kRampSeconds);
        for (auto* s : { &resonance, &levelGain, &sustainLevel }) s->reset(sampleRate, kRampSeconds);
        snap(t);
    }

    // Jump to the targets without ramping (settings restored on a rebuilt engine).
    void snap(const SynthParamTargets& t) {
        cutoffHz.setCurrentAndTargetValue(t.cutoffHz);
        resonance.setCurrentAndTargetValue(t.resonance);
        pitchRatio.setCurrentAndTargetValue(centsToRatio(t.detuneCents));
        levelGain.setCurrentAndTargetValue(t.level);
        sustainLevel.setCurrentAndTargetValue(t.sustain);
    }

    // Ramp from the current values to the targets (a target equal to the current one is a no-op,
    // so this is called every block).
    void setTargets(const SynthParamTargets& t) {
        cutoffHz.setTargetValue(t.cutoffHz);
        resonance.setTargetValue(t.resonance);
        pitchRatio.setTargetValue(centsToRatio(t.detuneCents));
        levelGain.setTargetValue(t.level);
        sustainLevel.setTargetValue(t.sustain);
    }

    // Fills the first n (<= kChunk) entries of the arrays below with the next n smoothed values.
    void next(int n) {
        n = std::clamp(n, 0, kChunk);
        fill(cutoffHz, cutoff, n);
        fill(resonance, reso, n);
        fill(pitchRatio, ratio, n);
        fill(levelGain, level, n);
        fill(sustainLevel, sustain, n);
    }

    bool isSmoothing() const {
        return cutoffHz.isSmoothing() || resonance.isSmoothing() || pitchRatio.isSmoothing()
            || levelGain.isSmoothing() || sustainLevel.isSmoothing();
    }

    // Values as of the last sample produced (what a note started now begins with).
    float currentCutoff() const { return cutoffHz.getCurrentValue(); }
    float currentResonance() const { return resonance.getCurrentValue(); }
    float currentPitchRatio() const { return pitchRatio.getCurrentValue(); }
    float currentLevel() const { return levelGain.getCurrentValue(); }
    float currentSustain() const { return sustainLevel.getCurrentValue(); }

    // Per-sample values from the last next() call.
    float cutoff[kChunk] {};
    float reso[kChunk] {};
    float ratio[kChunk] {};
    float level[kChunk] {};
    float sustain[kChunk] {};

private:
    template <typename S>
    static void fill(S& s, float* out, int n) {
        if (!s.isSmoothing()) {
            std::fill(out, out + n, s.getTargetValue());
            return;
        }
        for (int i = 0; i < n; ++i) out[i] = s.getNextValue();
    }

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> cutoffHz { 20000.0f };
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> pitchRatio { 1.0f };
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> resonance { 0.0f };
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> levelGain { 0.5f };
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> sustainLevel { 1.0f };
};
