#include "FxChainProcessor.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>

// ── FxSettings / FxParamStore ──

FxSettings FxSettings::defaults() {
    FxSettings s{};
    for (int fx = 0; fx < kFxCount; ++fx) {
        s.enabled[fx] = false;
        for (int p = 0; p < kFxMaxParams; ++p)
            s.values[fx][p] = p < kFxSpecs[fx].numParams ? kFxSpecs[fx].params[p].defaultValue : 0.0f;
    }
    return s;
}

bool FxSettings::operator==(const FxSettings& other) const {
    for (int fx = 0; fx < kFxCount; ++fx) {
        if (enabled[fx] != other.enabled[fx]) return false;
        for (int p = 0; p < kFxSpecs[fx].numParams; ++p)
            if (values[fx][p] != other.values[fx][p]) return false;
    }
    return true;
}

static float clampParam(int fx, int param, double value) {
    const FxParamSpec& spec = kFxSpecs[fx].params[param];
    double v = std::clamp(value, (double)spec.minValue, (double)spec.maxValue);
    if (spec.integer) v = std::round(v);
    return (float)v;
}

int FxParamStore::effectIndex(const char* id) {
    if (id == nullptr) return -1;
    for (int fx = 0; fx < kFxCount; ++fx)
        if (std::strcmp(kFxSpecs[fx].id, id) == 0) return fx;
    return -1;
}

int FxParamStore::paramIndex(int fx, const char* name) {
    if (fx < 0 || fx >= kFxCount || name == nullptr) return -1;
    for (int p = 0; p < kFxSpecs[fx].numParams; ++p)
        if (std::strcmp(kFxSpecs[fx].params[p].name, name) == 0) return p;
    return -1;
}

bool FxParamStore::setEnabled(const char* id, bool on) {
    const int fx = effectIndex(id);
    if (fx < 0) return false;
    setEnabled(fx, on);
    return true;
}

bool FxParamStore::setParam(const char* id, const char* param, double value) {
    const int fx = effectIndex(id);
    const int p = paramIndex(fx, param);
    if (p < 0 || !std::isfinite(value)) return false;
    setParam(fx, p, value);
    return true;
}

void FxParamStore::setEnabled(int fx, bool on) {
    if (fx < 0 || fx >= kFxCount) return;
    enabled[fx].store(on, std::memory_order_relaxed);
}

void FxParamStore::setParam(int fx, int param, double value) {
    if (fx < 0 || fx >= kFxCount || param < 0 || param >= kFxSpecs[fx].numParams) return;
    if (!std::isfinite(value)) return;
    values[fx][param].store(clampParam(fx, param, value), std::memory_order_relaxed);
}

FxSettings FxParamStore::snapshot() const {
    FxSettings s = FxSettings::defaults();
    for (int fx = 0; fx < kFxCount; ++fx) {
        s.enabled[fx] = isEnabled(fx);
        for (int p = 0; p < kFxSpecs[fx].numParams; ++p) s.values[fx][p] = get(fx, p);
    }
    return s;
}

void FxParamStore::apply(const FxSettings& s) {
    for (int fx = 0; fx < kFxCount; ++fx) {
        setEnabled(fx, s.enabled[fx]);
        for (int p = 0; p < kFxSpecs[fx].numParams; ++p) setParam(fx, p, s.values[fx][p]);
    }
}

void FxParamStore::resetToDefaults() {
    for (int fx = 0; fx < kFxCount; ++fx) {
        enabled[fx].store(false, std::memory_order_relaxed);
        for (int p = 0; p < kFxMaxParams; ++p)
            values[fx][p].store(p < kFxSpecs[fx].numParams ? kFxSpecs[fx].params[p].defaultValue : 0.0f,
                                std::memory_order_relaxed);
    }
}

// ── FxChainProcessor ──

namespace {
// One-pole low-pass coefficient for y += a * (x - y).
inline float onePoleCoeff(double cutoffHz, double sampleRate) {
    const double hz = std::min(cutoffHz, 0.45 * sampleRate);
    return (float)(1.0 - std::exp(-juce::MathConstants<double>::twoPi * hz / sampleRate));
}

inline void setSmoothed(juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>& s, float v, bool snap) {
    if (snap) s.setCurrentAndTargetValue(v);
    else s.setTargetValue(v);
}

} // namespace

FxChainProcessor::FxChainProcessor()
    : AudioProcessor(BusesProperties()
        .withInput("Input", juce::AudioChannelSet::stereo(), true)
        .withOutput("Output", juce::AudioChannelSet::stereo(), true)) {
}

void FxChainProcessor::prepareToPlay(double sampleRate, int /*samplesPerBlock*/) {
    // The graph can be prepared before the device rate is known (0 Hz); prepare for a nominal
    // rate then: it is prepared again with the device's rate before it renders for real.
    sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    const juce::dsp::ProcessSpec spec { sr, (juce::uint32)kChunk, 2 };

    for (auto& t : toggles) {
        t.level.reset(sr, kToggleRampSeconds);
        t.level.setCurrentAndTargetValue(0.0f);
        t.running = false;
        t.tailSamples = t.quietSamples = 0;
    }
    for (Smoothed* s : { &drivePre, &drivePost, &driveMix, &crushNoise, &crushMix, &eqLowDb, &eqMidDb,
                         &eqMidFreq, &eqHighDb, &echoFeedback, &echoMix, &pumpDepth, &compMakeup })
        s->reset(sr, kParamRampSeconds);
    echoDelay.reset(sr, 0.08);

    // Biquad coefficients are assigned here so that each filter's state is sized for order 2
    // now (IIR::Filter reallocates its state when the coefficient order changes).
    for (int ch = 0; ch < 2; ++ch) {
        *eqLow[ch].coefficients = juce::dsp::IIR::ArrayCoefficients<float>::makeLowShelf(sr, 120.0f, 0.707f, 1.0f);
        *eqMid[ch].coefficients = juce::dsp::IIR::ArrayCoefficients<float>::makePeakFilter(sr, 1000.0f, 0.9f, 1.0f);
        *eqHigh[ch].coefficients = juce::dsp::IIR::ArrayCoefficients<float>::makeHighShelf(sr, 6000.0f, 0.707f, 1.0f);
        eqLow[ch].reset();
        eqMid[ch].reset();
        eqHigh[ch].reset();
    }
    std::fill(std::begin(eqApplied), std::end(eqApplied), 1e9f);

    chorus.prepare(spec);
    chorus.setCentreDelay(7.0f);
    chorus.setFeedback(0.0f);
    std::fill(std::begin(chorusApplied), std::end(chorusApplied), -1.0f);

    phaser.prepare(spec);
    phaser.setCentreFrequency(1300.0f);
    std::fill(std::begin(phaserApplied), std::end(phaserApplied), -1.0f);

    // 2 s at up to 96 kHz (or the actual rate if higher), allocated once.
    echoMaxDelaySamples = (int)std::ceil(kMaxEchoSeconds * std::max(96000.0, sr)) + 4;
    echoLine.setMaximumDelayInSamples(echoMaxDelaySamples);
    echoLine.prepare(spec);

    reverb.prepare(spec);
    std::fill(std::begin(reverbApplied), std::end(reverbApplied), -1.0f);

    compressor.prepare(spec);
    std::fill(std::begin(compApplied), std::end(compApplied), 1e9f);

    limiterGain = 1.0f;
    limiterRelease = (float)std::exp(-1.0 / (0.050 * sr));
    limiterCeiling = juce::Decibels::decibelsToGain(kLimiterCeilingDb);

    for (int fx = 0; fx < kFxCount; ++fx) resetEffect(fx);
    pumpFreeBeat = 0.0;
    if (sourceRing != nullptr) sourceRing->setSampleRate(sr);
    prepared = true;
}

// Keeps every buffer: prepareToPlay runs again before the next render.
void FxChainProcessor::releaseResources() {}

void FxChainProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& /*midi*/) {
    juce::ScopedNoDenormals noDenormals;
    const int numSamples = buffer.getNumSamples();
    if (!prepared || buffer.getNumChannels() < 2 || numSamples <= 0) return;

    // Beat position (echo sync, pump). The synth runs before this node in the same graph pass
    // and calls TransportEngine::advanceBlock at the end of its block, so getPosition() already
    // reports the END of the block rendered now. advanceBlock also records the position, tempo
    // and play state at the block START (after applying a pending seek); that is read here, so
    // the phase used for these samples is exactly the one the synth's block covered, and later
    // sample offsets are startBeat + offset * beatsPerSample. These are plain fields written and
    // read on the audio thread only (graph nodes render one after another on that thread).
    double beatStart = 0.0, bpm = 120.0;
    bool playing = false;
    if (transport != nullptr) {
        beatStart = transport->getBlockStartBeat();
        bpm = transport->getBlockBpm();
        playing = transport->isBlockPlaying();
    }
    const double beatsPerSample = bpm / (60.0 * sr);

    float* left = buffer.getWritePointer(0);
    float* right = buffer.getWritePointer(1);
    float minGain = 1.0f;
    for (int done = 0; done < numSamples;) {
        const int n = std::min(kChunk, numSamples - done);
        processChunk(left + done, right + done, n, beatStart + done * beatsPerSample, beatsPerSample, playing);
        minGain = std::min(minGain, runLimiter(left + done, right + done, n));
        done += n;
    }

    // Instrument tap: what this chain hands to the output, before the graph sums the aux node in.
    if (sourceRing != nullptr) sourceRing->write(buffer.getArrayOfReadPointers(), 2, numSamples);

    diag.limiterGain.store(minGain, std::memory_order_relaxed);
    diag.processCount.fetch_add(1, std::memory_order_relaxed);
}

void FxChainProcessor::processChunk(float* left, float* right, int n, double beatStart, double beatsPerSample,
                                    bool playing) {
    // Whether the chain's input is finite: a non-finite input (from upstream) is silenced below
    // but does not reset the effects, so a source stuck on NaN cannot reset them every chunk.
    bool inputFinite = true;
    for (int i = 0; i < n && inputFinite; ++i)
        inputFinite = std::isfinite(left[i]) && std::isfinite(right[i]);

    bool anyRunning = false;
    for (int fx = 0; fx < kFxCount; ++fx) {
        Toggle& t = toggles[fx];
        const bool want = store->isEnabled(fx);
        if (want && !t.running) {
            // Turned on: start clean, at the current settings, fading in.
            resetEffect(fx);
            loadTargets(fx, true);
            t.running = true;
            t.level.setCurrentAndTargetValue(0.0f);
            t.level.setTargetValue(1.0f);
            t.tailSamples = t.quietSamples = 0;
        } else if (t.running) {
            t.level.setTargetValue(want ? 1.0f : 0.0f);
            loadTargets(fx, false);
            if (want) t.tailSamples = t.quietSamples = 0;
        }
        if (!t.running) continue; // bypassed: the signal is not touched
        anyRunning = true;

        if (spillsOver(fx)) {
            runSpilling(fx, t, left, right, n, beatStart, beatsPerSample, playing);
            continue;
        }

        const bool fullyIn = !t.level.isSmoothing() && t.level.getCurrentValue() >= 1.0f;
        if (!fullyIn) {
            std::memcpy(dryL, left, sizeof(float) * (size_t)n);
            std::memcpy(dryR, right, sizeof(float) * (size_t)n);
        }
        runEffect(fx, left, right, n, beatStart, beatsPerSample, playing);
        if (!fullyIn) {
            for (int i = 0; i < n; ++i) {
                const float w = t.level.getNextValue();
                left[i] = dryL[i] + w * (left[i] - dryL[i]);
                right[i] = dryR[i] + w * (right[i] - dryR[i]);
            }
            if (!t.level.isSmoothing() && t.level.getCurrentValue() <= 0.0f) t.running = false;
        }
    }

    // A feedback path that blew up must not keep the chain stuck on NaN: silence the chunk and
    // clear the running effects (once: only when the input was finite, so the effects are what
    // went wrong; a non-finite input is just silenced).
    if (anyRunning) {
        bool finite = true;
        for (int i = 0; i < n && finite; ++i)
            finite = std::isfinite(left[i]) && std::isfinite(right[i]);
        if (!finite) {
            std::fill(left, left + n, 0.0f);
            std::fill(right, right + n, 0.0f);
            if (inputFinite) {
                for (int fx = 0; fx < kFxCount; ++fx)
                    if (toggles[fx].running) resetEffect(fx);
                diag.nonFiniteResets.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
}

// Echo / reverb. Their toggle level scales their INPUT: output = (1 - g)·dry + effect(g·dry),
// which equals effect(dry) at g = 1 and dry + tail at g = 0 (both are linear in the dry path,
// and their dry output for a silent input is 0). Turned off (g reached 0): they run on silence
// and add their tail until it has stayed under kTailFloor for one echo period (reverb: 100 ms)
// or kMaxTailSeconds have passed, then they are bypassed.
void FxChainProcessor::runSpilling(int fx, Toggle& t, float* left, float* right, int n, double beatStart,
                                   double beatsPerSample, bool playing) {
    const bool fullyIn = !t.level.isSmoothing() && t.level.getCurrentValue() >= 1.0f;
    if (fullyIn) {
        runEffect(fx, left, right, n, beatStart, beatsPerSample, playing);
        return;
    }
    std::memcpy(dryL, left, sizeof(float) * (size_t)n);
    std::memcpy(dryR, right, sizeof(float) * (size_t)n);
    const bool muted = !t.level.isSmoothing() && t.level.getCurrentValue() <= 0.0f;
    if (muted) {
        std::fill(left, left + n, 0.0f);
        std::fill(right, right + n, 0.0f);
    } else {
        for (int i = 0; i < n; ++i) {
            const float g = t.level.getNextValue();
            fadeGain[i] = g;
            left[i] *= g;
            right[i] *= g;
        }
    }
    runEffect(fx, left, right, n, beatStart, beatsPerSample, playing);
    if (!muted) {
        for (int i = 0; i < n; ++i) {
            left[i] += (1.0f - fadeGain[i]) * dryL[i];
            right[i] += (1.0f - fadeGain[i]) * dryR[i];
        }
        return;
    }
    // Tail: left / right hold the wet output only.
    const int64_t maxTail = (int64_t)(kMaxTailSeconds * sr);
    const int64_t fadeLen = std::max<int64_t>(1, (int64_t)(kTailFadeSeconds * sr));
    float peak = 0.0f;
    for (int i = 0; i < n; ++i) {
        const int64_t remaining = maxTail - (t.tailSamples + i);
        const float cap = remaining >= fadeLen ? 1.0f : (float)std::max<int64_t>(0, remaining) / (float)fadeLen;
        const float wl = left[i] * cap, wr = right[i] * cap;
        peak = std::max(peak, std::max(std::fabs(wl), std::fabs(wr)));
        left[i] = dryL[i] + wl;
        right[i] = dryR[i] + wr;
    }
    t.tailSamples += n;
    t.quietSamples = (peak < kTailFloor) ? t.quietSamples + n : 0;
    const int64_t quietNeeded = fx == kFxEcho ? (int64_t)echoDelay.getTargetValue() + 2 * kChunk
                                              : (int64_t)(0.1 * sr);
    if (t.tailSamples >= maxTail || t.quietSamples >= quietNeeded) {
        t.running = false;
        t.tailSamples = t.quietSamples = 0;
    }
}

// Audio thread (or prepareToPlay). Clears the effect's internal state; no allocation.
void FxChainProcessor::resetEffect(int fx) {
    switch (fx) {
        case kFxDrive: driveLp[0] = driveLp[1] = 0.0f; break;
        case kFxCrush: crushCounter = 0; crushHeld[0] = crushHeld[1] = 0.0f; break;
        case kFxEq:
            for (int ch = 0; ch < 2; ++ch) { eqLow[ch].reset(); eqMid[ch].reset(); eqHigh[ch].reset(); }
            std::fill(std::begin(eqApplied), std::end(eqApplied), 1e9f);
            break;
        case kFxChorus: chorus.reset(); break;
        case kFxPhaser: phaser.reset(); break;
        // No echoLine.reset() (a memset of the whole 2 s line): echoFilled = 0 makes it read as cleared.
        case kFxEcho: echoFilled = 0; echoLp[0] = echoLp[1] = 0.0f; echoSnap = true; break;
        case kFxReverb: reverb.reset(); break;
        case kFxPump: break; // stateless: the phase comes from the transport
        case kFxCompressor: compressor.reset(); break;
        default: break;
    }
}

// Sets the smoothed parameters' targets from the store (snap = jump there, on enable).
// Parameters handed to a juce::dsp object are read in its run function instead.
void FxChainProcessor::loadTargets(int fx, bool snap) {
    const FxParamStore& p = *store;
    switch (fx) {
        case kFxDrive: {
            const float amount = p.get(kFxDrive, kDriveAmount);
            const float pre = std::pow(40.0f, amount);             // 1–40×
            setSmoothed(drivePre, pre, snap);
            setSmoothed(drivePost, std::pow(pre, -0.3f), snap);    // makeup: level roughly kept
            setSmoothed(driveMix, p.get(kFxDrive, kDriveMix), snap);
            break;
        }
        case kFxCrush:
            setSmoothed(crushNoise, p.get(kFxCrush, kCrushNoise), snap);
            setSmoothed(crushMix, p.get(kFxCrush, kCrushMix), snap);
            break;
        case kFxEq:
            setSmoothed(eqLowDb, p.get(kFxEq, kEqLow), snap);
            setSmoothed(eqMidDb, p.get(kFxEq, kEqMid), snap);
            setSmoothed(eqMidFreq, p.get(kFxEq, kEqMidFreq), snap);
            setSmoothed(eqHighDb, p.get(kFxEq, kEqHigh), snap);
            break;
        case kFxEcho:
            setSmoothed(echoFeedback, p.get(kFxEcho, kEchoFeedback), snap);
            setSmoothed(echoMix, p.get(kFxEcho, kEchoMix), snap);
            break;
        case kFxPump:
            setSmoothed(pumpDepth, p.get(kFxPump, kPumpDepth), snap);
            break;
        case kFxCompressor:
            setSmoothed(compMakeup, juce::Decibels::decibelsToGain(p.get(kFxCompressor, kCompMakeup)), snap);
            break;
        default: break;
    }
}

void FxChainProcessor::runEffect(int fx, float* left, float* right, int n, double beatStart,
                                 double beatsPerSample, bool playing) {
    switch (fx) {
        case kFxDrive: runDrive(left, right, n); break;
        case kFxCrush: runCrush(left, right, n); break;
        case kFxEq: runEq(left, right, n); break;
        case kFxChorus: runChorus(left, right, n); break;
        case kFxPhaser: runPhaser(left, right, n); break;
        case kFxEcho: runEcho(left, right, n, beatsPerSample * 60.0 * sr); break;
        case kFxReverb: runReverb(left, right, n); break;
        case kFxPump: runPump(left, right, n, beatStart, beatsPerSample, playing); break;
        case kFxCompressor: runCompressor(left, right, n); break;
        default: break;
    }
}

// pre-gain → tanh → makeup → one-pole tone low-pass (800 Hz – 20 kHz), dry/wet mix.
void FxChainProcessor::runDrive(float* left, float* right, int n) {
    driveToneCoeff = onePoleCoeff(800.0 * std::pow(25.0, (double)store->get(kFxDrive, kDriveTone)), sr);
    float* ch[2] = { left, right };
    for (int i = 0; i < n; ++i) {
        const float pre = drivePre.getNextValue();
        const float post = drivePost.getNextValue();
        const float mix = driveMix.getNextValue();
        for (int c = 0; c < 2; ++c) {
            const float x = ch[c][i];
            const float shaped = std::tanh(x * pre) * post;
            driveLp[c] += driveToneCoeff * (shaped - driveLp[c]);
            ch[c][i] = x + mix * (driveLp[c] - x);
        }
    }
}

// sample-and-hold (downsample) → bit quantise → white noise (xorshift32), dry/wet mix.
void FxChainProcessor::runCrush(float* left, float* right, int n) {
    const float bits = store->get(kFxCrush, kCrushBits);
    const int hold = std::max(1, (int)std::lround(store->get(kFxCrush, kCrushDownsample)));
    const float levels = std::pow(2.0f, bits - 1.0f); // quantisation steps per unit amplitude
    const float step = 1.0f / levels;
    float* ch[2] = { left, right };
    for (int i = 0; i < n; ++i) {
        const float noiseAmount = crushNoise.getNextValue() * 0.02f;
        const float mix = crushMix.getNextValue();
        if (crushCounter == 0) {
            crushHeld[0] = left[i];
            crushHeld[1] = right[i];
        }
        crushCounter = (crushCounter + 1) % hold;
        for (int c = 0; c < 2; ++c) {
            crushRng ^= crushRng << 13;
            crushRng ^= crushRng >> 17;
            crushRng ^= crushRng << 5;
            const float white = (float)crushRng * (2.0f / 4294967296.0f) - 1.0f;
            const float q = std::round(crushHeld[c] * levels) * step + noiseAmount * white;
            const float x = ch[c][i];
            ch[c][i] = x + mix * (q - x);
        }
    }
}

// low shelf 120 Hz, peak (Q 0.9) at midFreq, high shelf 6 kHz. Gains smoothed; coefficients
// recomputed per chunk when a smoothed value moved (assigning keeps the filters' storage).
void FxChainProcessor::runEq(float* left, float* right, int n) {
    using AC = juce::dsp::IIR::ArrayCoefficients<float>;
    const float lowDb = eqLowDb.skip(n);
    const float midDb = eqMidDb.skip(n);
    const float midHz = eqMidFreq.skip(n);
    const float highDb = eqHighDb.skip(n);
    if (lowDb != eqApplied[0]) {
        const auto c = AC::makeLowShelf(sr, 120.0f, 0.707f, juce::Decibels::decibelsToGain(lowDb));
        for (auto& f : eqLow) *f.coefficients = c;
        eqApplied[0] = lowDb;
    }
    if (midDb != eqApplied[1] || midHz != eqApplied[2]) {
        const auto c = AC::makePeakFilter(sr, midHz, 0.9f, juce::Decibels::decibelsToGain(midDb));
        for (auto& f : eqMid) *f.coefficients = c;
        eqApplied[1] = midDb;
        eqApplied[2] = midHz;
    }
    if (highDb != eqApplied[3]) {
        const auto c = AC::makeHighShelf(sr, 6000.0f, 0.707f, juce::Decibels::decibelsToGain(highDb));
        for (auto& f : eqHigh) *f.coefficients = c;
        eqApplied[3] = highDb;
    }
    float* ch[2] = { left, right };
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < n; ++i)
            ch[c][i] = eqHigh[c].processSample(eqMid[c].processSample(eqLow[c].processSample(ch[c][i])));
}

void FxChainProcessor::runChorus(float* left, float* right, int n) {
    const float rate = store->get(kFxChorus, kChorusRate);
    const float depth = store->get(kFxChorus, kChorusDepth);
    const float mix = store->get(kFxChorus, kChorusMix);
    if (rate != chorusApplied[0]) { chorus.setRate(rate); chorusApplied[0] = rate; }
    if (depth != chorusApplied[1]) { chorus.setDepth(depth); chorusApplied[1] = depth; }
    if (mix != chorusApplied[2]) { chorus.setMix(mix); chorusApplied[2] = mix; }
    float* ch[2] = { left, right };
    juce::dsp::AudioBlock<float> block(ch, 2, (size_t)n);
    chorus.process(juce::dsp::ProcessContextReplacing<float>(block));
}

void FxChainProcessor::runPhaser(float* left, float* right, int n) {
    const float rate = store->get(kFxPhaser, kPhaserRate);
    const float depth = store->get(kFxPhaser, kPhaserDepth);
    const float feedback = store->get(kFxPhaser, kPhaserFeedback);
    const float mix = store->get(kFxPhaser, kPhaserMix);
    if (rate != phaserApplied[0]) { phaser.setRate(rate); phaserApplied[0] = rate; }
    if (depth != phaserApplied[1]) { phaser.setDepth(depth); phaserApplied[1] = depth; }
    if (feedback != phaserApplied[2]) { phaser.setFeedback(feedback); phaserApplied[2] = feedback; }
    if (mix != phaserApplied[3]) { phaser.setMix(mix); phaserApplied[3] = mix; }
    float* ch[2] = { left, right };
    juce::dsp::AudioBlock<float> block(ch, 2, (size_t)n);
    phaser.process(juce::dsp::ProcessContextReplacing<float>(block));
}

// Stereo delay (or ping-pong: mono input into the left line, each line fed back from the other),
// one-pole tone low-pass (1–20 kHz) on the repeats and in the feedback path, dry/wet mix.
// Synced time = division (in beats) at the transport tempo of this block, max 2 s.
void FxChainProcessor::runEcho(float* left, float* right, int n, double bpm) {
    const FxParamStore& p = *store;
    double seconds = p.get(kFxEcho, kEchoTime);
    if (p.get(kFxEcho, kEchoSync) >= 0.5f) {
        const int division = std::clamp((int)p.get(kFxEcho, kEchoDivision), 0, 5);
        seconds = kEchoDivisionBeats[division] * 60.0 / std::max(bpm, 1.0);
    }
    seconds = std::min(seconds, kMaxEchoSeconds);
    const float delaySamples = (float)std::clamp(seconds * sr, 1.0, (double)echoMaxDelaySamples - 2.0);
    setSmoothed(echoDelay, delaySamples, echoSnap);
    echoSnap = false;
    echoToneCoeff = onePoleCoeff(1000.0 * std::pow(20.0, (double)p.get(kFxEcho, kEchoTone)), sr);
    const bool pingPong = p.get(kFxEcho, kEchoPingPong) >= 0.5f;

    for (int i = 0; i < n; ++i) {
        const float d = echoDelay.getNextValue();
        const float feedback = echoFeedback.getNextValue();
        const float mix = echoMix.getNextValue();
        // Read before write: with a delay of d >= 1 sample, sample i reads input i - d.
        // Linear interpolation reads the samples pushed floor(d) and floor(d) + 1 pushes ago;
        // anything pushed before the last reset counts as silence.
        const bool filled = (int)d + 1 <= echoFilled;
        float yL = echoLine.popSample(0, d); // always popped: it advances the read position
        float yR = echoLine.popSample(1, d);
        if (!filled) yL = yR = 0.0f;
        if (echoFilled <= echoMaxDelaySamples) ++echoFilled;
        echoLp[0] += echoToneCoeff * (yL - echoLp[0]);
        echoLp[1] += echoToneCoeff * (yR - echoLp[1]);
        const float xL = left[i], xR = right[i];
        if (pingPong) {
            echoLine.pushSample(0, 0.5f * (xL + xR) + feedback * echoLp[1]);
            echoLine.pushSample(1, feedback * echoLp[0]);
        } else {
            echoLine.pushSample(0, xL + feedback * echoLp[0]);
            echoLine.pushSample(1, xR + feedback * echoLp[1]);
        }
        left[i] = xL + mix * (echoLp[0] - xL);
        right[i] = xR + mix * (echoLp[1] - xR);
    }
}

void FxChainProcessor::runReverb(float* left, float* right, int n) {
    const float size = store->get(kFxReverb, kReverbSize);
    const float damping = store->get(kFxReverb, kReverbDamping);
    const float width = store->get(kFxReverb, kReverbWidth);
    const float mix = store->get(kFxReverb, kReverbMix);
    if (size != reverbApplied[0] || damping != reverbApplied[1] || width != reverbApplied[2] || mix != reverbApplied[3]) {
        juce::dsp::Reverb::Parameters params;
        params.roomSize = size;
        params.damping = damping;
        params.width = width;
        // juce::Reverb scales wet by 3 and dry by 2: dry gain 1 - mix, wet gain 1.5 × mix.
        params.wetLevel = 0.5f * mix;
        params.dryLevel = 0.5f * (1.0f - mix);
        params.freezeMode = 0.0f;
        reverb.setParameters(params); // juce::Reverb smooths the level / damping changes itself
        reverbApplied[0] = size; reverbApplied[1] = damping; reverbApplied[2] = width; reverbApplied[3] = mix;
    }
    float* ch[2] = { left, right };
    juce::dsp::AudioBlock<float> block(ch, 2, (size_t)n);
    reverb.process(juce::dsp::ProcessContextReplacing<float>(block));
}

// Side-chain-style ducking locked to the beat: at each division boundary the gain drops by
// `depth` (4 ms attack) and recovers along (1 - phase)^k, k from `shape` (0 = slow, 1 = snappy).
// While the transport is stopped it free-runs at the current tempo from where it was.
void FxChainProcessor::runPump(float* left, float* right, int n, double beatStart, double beatsPerSample,
                               bool playing) {
    const int division = std::clamp((int)store->get(kFxPump, kPumpDivision), 0, 3);
    const double divisionBeats = kPumpDivisionBeats[division];
    const float k = 0.6f * std::pow(12.0f, store->get(kFxPump, kPumpShape)); // 0.6–7.2
    const double attackPhase = std::max(0.004 * sr * beatsPerSample / divisionBeats, 1e-6);
    const double start = playing ? beatStart : pumpFreeBeat;
    for (int i = 0; i < n; ++i) {
        const double units = (start + i * beatsPerSample) / divisionBeats;
        const double phase = units - std::floor(units);
        float env = std::pow((float)(1.0 - phase), k);
        if (phase < attackPhase) env *= (float)(phase / attackPhase);
        const float gain = 1.0f - pumpDepth.getNextValue() * env;
        left[i] *= gain;
        right[i] *= gain;
    }
    // 64 beats is a multiple of every division: wrapping keeps the phase and the precision.
    pumpFreeBeat = std::fmod(start + n * beatsPerSample, 64.0);
    if (pumpFreeBeat < 0.0) pumpFreeBeat += 64.0;
}

void FxChainProcessor::runCompressor(float* left, float* right, int n) {
    const FxParamStore& p = *store;
    const float threshold = p.get(kFxCompressor, kCompThreshold);
    const float ratio = p.get(kFxCompressor, kCompRatio);
    const float attack = p.get(kFxCompressor, kCompAttack);
    const float release = p.get(kFxCompressor, kCompRelease);
    if (threshold != compApplied[0]) { compressor.setThreshold(threshold); compApplied[0] = threshold; }
    if (ratio != compApplied[1]) { compressor.setRatio(ratio); compApplied[1] = ratio; }
    if (attack != compApplied[2]) { compressor.setAttack(attack); compApplied[2] = attack; }
    if (release != compApplied[3]) { compressor.setRelease(release); compApplied[3] = release; }
    float* ch[2] = { left, right };
    juce::dsp::AudioBlock<float> block(ch, 2, (size_t)n);
    compressor.process(juce::dsp::ProcessContextReplacing<float>(block));
    for (int i = 0; i < n; ++i) {
        const float makeup = compMakeup.getNextValue();
        left[i] *= makeup;
        right[i] *= makeup;
    }
}

// Stereo-linked peak limiter: instant attack (the gain never lets a sample past the ceiling),
// 50 ms release, gain snapped to exactly 1 near unity so a signal under the ceiling passes
// unchanged. Returns the lowest gain used.
float FxChainProcessor::runLimiter(float* left, float* right, int n) {
    float minGain = limiterGain;
    for (int i = 0; i < n; ++i) {
        const float peak = std::max(std::fabs(left[i]), std::fabs(right[i]));
        const float target = peak > limiterCeiling ? limiterCeiling / peak : 1.0f;
        float g = 1.0f - (1.0f - limiterGain) * limiterRelease;
        if (g > 0.99999f) g = 1.0f;
        g = std::min(g, target);
        limiterGain = g;
        if (g != 1.0f) {
            left[i] *= g;
            right[i] *= g;
            minGain = std::min(minGain, g);
        }
    }
    return minGain;
}
