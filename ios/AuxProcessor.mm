#include "AuxProcessor.h"
#include <algorithm>
#include <cmath>
#include <cstring>

// ── Settings ──

int metronomeStyleIndex(const char* name) {
    if (name == nullptr) return -1;
    for (int i = 0; i < kMetronomeStyleCount; ++i)
        if (std::strcmp(kMetronomeStyleNames[i], name) == 0) return i;
    return -1;
}

AuxSettings AuxSettings::defaults() {
    return { false, kMetronomeClassic, 0.8f, true, 4, 0.8f, true, true };
}

bool AuxSettings::operator==(const AuxSettings& o) const {
    return metronomeEnabled == o.metronomeEnabled && metronomeStyle == o.metronomeStyle
        && metronomeLevel == o.metronomeLevel && metronomeAccent == o.metronomeAccent
        && beatsPerBar == o.beatsPerBar && trackLevel == o.trackLevel && trackLoop == o.trackLoop
        && trackEnabled == o.trackEnabled;
}

bool AuxSettingsStore::setMetronomeStyle(const char* name) {
    const int style = metronomeStyleIndex(name);
    if (style < 0) return false;
    setMetronomeStyle(style);
    return true;
}

void AuxSettingsStore::setMetronomeStyle(int style) {
    if (style < 0 || style >= kMetronomeStyleCount) return;
    metronomeStyle.store(style, std::memory_order_relaxed);
}

void AuxSettingsStore::setMetronomeLevel(double level) {
    if (!std::isfinite(level)) return;
    metronomeLevel.store((float)std::clamp(level, 0.0, 1.0), std::memory_order_relaxed);
}

void AuxSettingsStore::setBeatsPerBar(double beats) {
    if (!std::isfinite(beats)) return;
    beatsPerBar.store((int)std::lround(std::clamp(beats, 1.0, 16.0)), std::memory_order_relaxed);
}

void AuxSettingsStore::setTrackLevel(double level) {
    if (!std::isfinite(level)) return;
    trackLevel.store((float)std::clamp(level, 0.0, 1.0), std::memory_order_relaxed);
}

AuxSettings AuxSettingsStore::snapshot() const {
    const auto r = std::memory_order_relaxed;
    return { metronomeEnabled.load(r), metronomeStyle.load(r), metronomeLevel.load(r), metronomeAccent.load(r),
             beatsPerBar.load(r), trackLevel.load(r), trackLoop.load(r), trackEnabled.load(r) };
}

void AuxSettingsStore::apply(const AuxSettings& s) {
    setMetronomeEnabled(s.metronomeEnabled);
    setMetronomeStyle(s.metronomeStyle);
    setMetronomeLevel(s.metronomeLevel);
    setMetronomeAccent(s.metronomeAccent);
    setBeatsPerBar(s.beatsPerBar);
    setTrackLevel(s.trackLevel);
    setTrackLoop(s.trackLoop);
    setTrackEnabled(s.trackEnabled);
}

// ── Click synthesis (prepareToPlay only) ──

namespace {
struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    double process(double x) {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    // RBJ cookbook
    static Biquad bandpass(double fs, double f0, double q) {
        const double w = juce::MathConstants<double>::twoPi * f0 / fs, alpha = std::sin(w) / (2.0 * q);
        const double a0 = 1.0 + alpha;
        Biquad f;
        f.b0 = alpha / a0; f.b1 = 0.0; f.b2 = -alpha / a0;
        f.a1 = -2.0 * std::cos(w) / a0; f.a2 = (1.0 - alpha) / a0;
        return f;
    }
    static Biquad highpass(double fs, double f0, double q) {
        const double w = juce::MathConstants<double>::twoPi * f0 / fs, alpha = std::sin(w) / (2.0 * q);
        const double c = std::cos(w), a0 = 1.0 + alpha;
        Biquad f;
        f.b0 = (1.0 + c) / 2.0 / a0; f.b1 = -(1.0 + c) / a0; f.b2 = (1.0 + c) / 2.0 / a0;
        f.a1 = -2.0 * c / a0; f.a2 = (1.0 - alpha) / a0;
        return f;
    }
};

struct Noise {
    uint32_t state;
    explicit Noise(uint32_t seed) : state(seed) {}
    double next() {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        return (double)state * (2.0 / 4294967296.0) - 1.0;
    }
};

double square(double phase) { return std::fmod(phase, 1.0) < 0.5 ? 1.0 : -1.0; }

// Peak-normalises to `peak` and fades the last millisecond to exactly 0.
void finishClick(std::vector<float>& c, double fs, double peak) {
    float maxAbs = 0.0f;
    for (float v : c) maxAbs = std::max(maxAbs, std::fabs(v));
    const float scale = maxAbs > 0.0f ? (float)(peak / maxAbs) : 0.0f;
    const int fade = std::max(1, (int)(0.001 * fs));
    const int n = (int)c.size();
    for (int i = 0; i < n; ++i) {
        float g = scale;
        if (i >= n - fade) g *= (float)(n - 1 - i) / (float)fade;
        c[(size_t)i] *= g;
    }
}
} // namespace

void AuxProcessor::renderClicks() {
    const double fs = sr;
    const double loudness[kMetronomeStyleCount] = { 1.0, 1.0, 0.5, 0.8, 0.7 };
    for (int style = 0; style < kMetronomeStyleCount; ++style) {
        for (int accent = 0; accent < 2; ++accent) {
            std::vector<float>& c = clicks[style][accent];
            double seconds = 0.03;
            switch (style) {
                case kMetronomeClassic: seconds = 0.030; break;
                case kMetronomeWood: seconds = 0.040; break;
                case kMetronomeBeep: seconds = 0.050; break;
                case kMetronomeCowbell: seconds = 0.080; break;
                case kMetronomeHat: seconds = 0.025; break;
                default: break;
            }
            const int n = std::max(1, (int)std::lround(std::min(seconds, kMaxClickSeconds) * fs));
            c.assign((size_t)n, 0.0f);
            Noise noise(0x1234567u + (uint32_t)(style * 2 + accent));
            switch (style) {
                case kMetronomeClassic: { // sine blip 1.5 kHz / accent 2.5 kHz
                    const double f = accent ? 2500.0 : 1500.0;
                    for (int i = 0; i < n; ++i) {
                        const double t = i / fs;
                        const double env = std::min(1.0, t / 0.0005) * std::exp(-t / 0.010);
                        c[(size_t)i] = (float)(std::sin(juce::MathConstants<double>::twoPi * f * t) * env);
                    }
                    break;
                }
                case kMetronomeWood: { // resonant band-passed noise burst
                    Biquad bp = Biquad::bandpass(fs, accent ? 2400.0 : 1700.0, 8.0);
                    for (int i = 0; i < n; ++i) {
                        const double t = i / fs;
                        const double excite = t < 0.002 ? noise.next() : 0.0;
                        c[(size_t)i] = (float)(bp.process(excite) * std::exp(-t / 0.012));
                    }
                    break;
                }
                case kMetronomeBeep: { // square 1 kHz (accent 2 kHz), 50 ms, 1 ms edges
                    const double f = accent ? 2000.0 : 1000.0;
                    for (int i = 0; i < n; ++i) {
                        const double t = i / fs;
                        const double edge = std::min(1.0, t / 0.001);
                        c[(size_t)i] = (float)(square(f * t) * edge);
                    }
                    break;
                }
                case kMetronomeCowbell: { // two detuned squares 540 / 800 Hz through a band-pass
                    Biquad bp1 = Biquad::bandpass(fs, 800.0, 1.2);
                    Biquad bp2 = Biquad::bandpass(fs, 800.0, 1.2);
                    for (int i = 0; i < n; ++i) {
                        const double t = i / fs;
                        const double env = std::min(1.0, t / 0.0005) * (0.6 * std::exp(-t / 0.008) + 0.4 * std::exp(-t / 0.035));
                        const double s = 0.5 * (square(540.0 * t) + square(800.0 * t));
                        c[(size_t)i] = (float)(bp2.process(bp1.process(s)) * env);
                    }
                    break;
                }
                case kMetronomeHat: { // high-passed noise, 25 ms
                    Biquad hp1 = Biquad::highpass(fs, std::min(7000.0, 0.4 * fs), 0.707);
                    Biquad hp2 = Biquad::highpass(fs, std::min(7000.0, 0.4 * fs), 0.707);
                    for (int i = 0; i < n; ++i) {
                        const double t = i / fs;
                        c[(size_t)i] = (float)(hp2.process(hp1.process(noise.next())) * std::exp(-t / 0.006));
                    }
                    break;
                }
                default: break;
            }
            finishClick(c, fs, loudness[style] * (accent ? 1.0 : 0.75));
        }
    }
}

// ── Processor ──

AuxProcessor::AuxProcessor()
    : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)) {
}

AuxProcessor::~AuxProcessor() {
    // Runs after the node stopped rendering: the holder may free what this reader last used.
    track->releaseReader();
}

void AuxProcessor::setBackingTrack(BackingTrack* t) {
    track->releaseReader();
    track = t ? t : &ownTrack;
    lastTrack = nullptr;
}

void AuxProcessor::prepareToPlay(double sampleRate, int /*samplesPerBlock*/) {
    sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    renderClicks();
    clickData = nullptr;
    clickLength = clickPos = 0;
    metronomeGain.reset(sr, kLevelRampSeconds);
    metronomeGain.setCurrentAndTargetValue(settings->metronomeLevel.load(std::memory_order_relaxed));
    trackGain.reset(sr, kLevelRampSeconds);
    trackGain.setCurrentAndTargetValue(settings->trackLevel.load(std::memory_order_relaxed));
    wasPlaying = false;
    trackActive = false;
    nextTrackPos = 0;
    lastClickedBeat = 0;
    lastTrack = nullptr;
    prepared = true;
    // Ask the holder for a buffer at this rate (re-resampled off this thread if needed).
    if (sampleRate > 0.0) track->requestRate(sampleRate);
}

void AuxProcessor::releaseResources() {}

void AuxProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& /*midi*/) {
    buffer.clear();
    const int n = buffer.getNumSamples();
    if (!prepared || n <= 0 || buffer.getNumChannels() < 2) {
        diag.processCount.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // Block-start transport snapshot, see TransportEngine::getBlockStartBeat.
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

    renderMetronome(left, right, n, beatStart, beatsPerSample, playing);
    renderTrack(left, right, n, beatStart, bpm, playing);

    wasPlaying = playing;
    diag.processCount.fetch_add(1, std::memory_order_relaxed);
}

void AuxProcessor::renderMetronome(float* left, float* right, int n, double beatStart, double beatsPerSample,
                                   bool playing) {
    const auto r = std::memory_order_relaxed;
    const bool enabled = settings->metronomeEnabled.load(r);
    const int style = std::clamp(settings->metronomeStyle.load(r), 0, kMetronomeStyleCount - 1);
    const bool accent = settings->metronomeAccent.load(r);
    const int64_t bpb = std::max(1, settings->beatsPerBar.load(r));
    metronomeGain.setTargetValue(settings->metronomeLevel.load(r));

    // A beat k sounds on the first sample whose beat position is >= k: offset
    // ceil((k - beatStart) / beatsPerSample - 1e-6) from the block start (the epsilon absorbs the
    // rounding of the accumulated transport position). The beats whose onset lies in this block
    // are k > beatStart - beatsPerSample, so consecutive blocks partition the beats exactly.
    // After a (re)start or a seek the count restarts at the first of them; otherwise it continues
    // after the last clicked beat (never twice, never skipped).
    const int64_t firstHere = (int64_t)std::floor(beatStart - beatsPerSample * (1.0 - 1e-6)) + 1;
    if (!wasPlaying || lastClickedBeat >= firstHere || firstHere > lastClickedBeat + 1)
        lastClickedBeat = firstHere - 1;

    int pos = 0;
    while (pos < n) {
        int eventAt = n;
        int64_t beat = lastClickedBeat + 1;
        if (playing && enabled && beatsPerSample > 0.0) {
            const double offset = std::ceil(((double)beat - beatStart) / beatsPerSample - 1e-6);
            if (offset < (double)n) eventAt = std::max(pos, (int)std::max(0.0, offset));
        }
        // Continue the sounding click up to the event.
        for (int i = pos; i < eventAt && clickData != nullptr; ++i) {
            const float v = clickData[clickPos] * metronomeGain.getNextValue();
            left[i] += v;
            right[i] += v;
            if (++clickPos >= clickLength) { clickData = nullptr; clickLength = clickPos = 0; }
        }
        if (eventAt >= n) break;
        // Click on `beat`; accent on beat 1 of the bar.
        const int64_t inBar = ((beat % bpb) + bpb) % bpb;
        const std::vector<float>& c = clicks[style][accent && inBar == 0 ? 1 : 0];
        clickData = c.data();
        clickLength = (int)c.size();
        clickPos = 0;
        lastClickedBeat = beat;
        diag.clicks.fetch_add(1, std::memory_order_relaxed);
        pos = eventAt;
    }
}

void AuxProcessor::renderTrack(float* left, float* right, int n, double beatStart, double bpm, bool playing) {
    const auto r = std::memory_order_relaxed;
    BackingTrackBuffer* t = track->acquire();
    if (t != lastTrack) {
        // Another buffer (load, unload or resample): the old one may be freed now, so nothing
        // continues from it; the new one starts with a fade.
        lastTrack = t;
        trackActive = false;
        tailActive = false;
    }
    const bool loop = settings->trackLoop.load(r);
    const bool enabled = settings->trackEnabled.load(r);
    trackGain.setTargetValue(settings->trackLevel.load(r));
    const bool playable = t != nullptr && t->sampleRate == sr && t->length > 0;
    if (!playable) {
        trackActive = tailActive = false;
        diag.trackPositionSec.store(-1.0, r);
        return;
    }
    const int64_t len = t->length;
    const float* srcL = t->audio.getReadPointer(0);
    const float* srcR = t->audio.getReadPointer(1);
    const int fadeLen = std::max(1, (int)(kFadeSeconds * sr));

    if (playing && !wasPlaying) {
        const int64_t bpb = std::max(1, settings->beatsPerBar.load(r));
        anchorBeat = std::floor(beatStart / (double)bpb) * (double)bpb;
    }
    const bool sounding = playing && enabled;
    if (sounding) {
        const int64_t pos = (int64_t)std::llround((beatStart - anchorBeat) * 60.0 / bpm * sr);
        const bool continuous = trackActive && std::llabs(pos - nextTrackPos) <= 2;
        if (!continuous) {
            if (trackActive) { // seek / tempo jump: fade the old position out
                tailActive = true; tailPos = nextTrackPos; tailLeft = fadeLen;
            }
            const int64_t wrapped = loop ? ((pos % len) + len) % len : pos;
            mainPos = pos;
            mainFadeLeft = (!trackActive && wrapped == 0) ? 0 : fadeLen; // a start at the top needs no fade
            trackActive = true;
        } else {
            mainPos = nextTrackPos;
        }
    } else if (trackActive) { // stopped / disabled: fade out where it was
        tailActive = true; tailPos = nextTrackPos; tailLeft = fadeLen;
        trackActive = false;
    }
    if (!trackActive && !tailActive) {
        diag.trackPositionSec.store(-1.0, r);
        return;
    }

    auto read = [&](int64_t p, const float* src) -> float {
        if (loop) return src[((p % len) + len) % len];
        return (p >= 0 && p < len) ? src[p] : 0.0f;
    };
    for (int i = 0; i < n; ++i) {
        float l = 0.0f, rr = 0.0f;
        if (trackActive) {
            float g = 1.0f;
            if (mainFadeLeft > 0) { g = 1.0f - (float)mainFadeLeft / (float)fadeLen; --mainFadeLeft; }
            l += read(mainPos, srcL) * g;
            rr += read(mainPos, srcR) * g;
            ++mainPos;
        }
        if (tailActive) {
            const float g = (float)tailLeft / (float)fadeLen;
            l += read(tailPos, srcL) * g;
            rr += read(tailPos, srcR) * g;
            ++tailPos;
            if (--tailLeft <= 0) tailActive = false;
        }
        const float level = trackGain.getNextValue();
        left[i] += l * level;
        right[i] += rr * level;
    }
    if (trackActive) nextTrackPos = mainPos;
    diag.trackPositionSec.store(trackActive ? (double)(loop ? ((mainPos % len) + len) % len : mainPos) / sr : -1.0, r);
}
