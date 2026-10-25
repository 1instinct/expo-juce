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
void finishClick(float* c, int n, double fs, double peak) {
    float maxAbs = 0.0f;
    for (int i = 0; i < n; ++i) maxAbs = std::max(maxAbs, std::fabs(c[i]));
    const float scale = maxAbs > 0.0f ? (float)(peak / maxAbs) : 0.0f;
    const int fade = std::max(1, std::min(n, (int)(0.001 * fs)));
    for (int i = 0; i < n; ++i) {
        float g = scale;
        if (i >= n - fade) g *= (float)(n - 1 - i) / (float)fade;
        c[i] *= g;
    }
}
} // namespace

void AuxProcessor::renderClicks() {
    const double fs = sr;
    const double loudness[kMetronomeStyleCount] = { 1.0, 1.0, 0.5, 0.8, 0.7 };
    for (int style = 0; style < kMetronomeStyleCount; ++style) {
        for (int accent = 0; accent < 2; ++accent) {
            float* c = clickSlot(style, accent);
            double seconds = 0.03;
            switch (style) {
                case kMetronomeClassic: seconds = 0.030; break;
                case kMetronomeWood: seconds = 0.040; break;
                case kMetronomeBeep: seconds = 0.050; break;
                case kMetronomeCowbell: seconds = 0.080; break;
                case kMetronomeHat: seconds = 0.025; break;
                default: break;
            }
            const int n = std::clamp((int)std::lround(std::min(seconds, kMaxClickSeconds) * fs), 1, kMaxClickSamples);
            std::fill(c, c + n, 0.0f);
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
            finishClick(c, n, fs, loudness[style] * (accent ? 1.0 : 0.75));
            clickLengths[style][accent].store(n, std::memory_order_release);
        }
    }
}

// ── Processor ──

AuxProcessor::AuxProcessor()
    : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      clickStore(new float[(size_t)kMetronomeStyleCount * 2 * kMaxClickSamples]()) {
}

AuxProcessor::~AuxProcessor() {
    // Runs after the node stopped rendering: the holder may free what this reader last used.
    track->releaseReader();
}

std::vector<float> AuxProcessor::clickBuffer(int style, bool accent) const {
    style = std::clamp(style, 0, kMetronomeStyleCount - 1);
    const int a = accent ? 1 : 0;
    const float* c = clickSlot(style, a);
    return std::vector<float>(c, c + clickLengths[style][a].load(std::memory_order_acquire));
}

void AuxProcessor::setBackingTrack(BackingTrack* t) {
    track->releaseReader();
    track = t ? t : &ownTrack;
    lastTrack = nullptr;
    tailActive = trackActive = tailProtected = false;
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
    havePrevBlock = false;
    trackActive = false;
    tailActive = false;
    trackPos = 0;
    lastClickedBeat = 0;
    lastTrack = nullptr;
    if (tailProtected) { track->releaseTail(); tailProtected = false; }
    prepared = true;
    // Ask the holder for a buffer at this rate (loaded again off this thread if needed).
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
    bool playing = false, seekFlag = false;
    if (transport != nullptr) {
        beatStart = transport->getBlockStartBeat();
        bpm = transport->getBlockBpm();
        playing = transport->isBlockPlaying();
        seekFlag = transport->wasBlockSeeked();
    }
    const double beatsPerSample = bpm / (60.0 * sr);
    // A move of the transport: its seek flag, or a block that does not start where the previous
    // one ended (e.g. another node advanced it differently). Tempo changes alone are not moves.
    const double expected = prevBeatStart + prevBeatsAdvanced;
    const bool seeked = seekFlag || (havePrevBlock && wasPlaying
                                     && std::fabs(beatStart - expected) > 1e-7 * std::max(1.0, std::fabs(expected)));
    float* left = buffer.getWritePointer(0);
    float* right = buffer.getWritePointer(1);

    renderMetronome(left, right, n, beatStart, beatsPerSample, playing);
    renderTrack(left, right, n, beatStart, bpm, playing, seeked);

    const double advanced = playing ? beatsPerSample * n : 0.0;
    prevBeatStart = beatStart;
    prevBeatsAdvanced = advanced;
    havePrevBlock = true;
    wasPlaying = playing;
    if (analysis != nullptr) analysis->publishTransport(beatStart + advanced, bpm, playing);
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
        const int a = accent && inBar == 0 ? 1 : 0;
        clickData = clickSlot(style, a);
        clickLength = std::clamp(clickLengths[style][a].load(std::memory_order_acquire), 0, kMaxClickSamples);
        clickPos = 0;
        if (clickLength == 0) clickData = nullptr;
        lastClickedBeat = beat;
        diag.clicks.fetch_add(1, std::memory_order_relaxed);
        pos = eventAt;
    }
}

// The voice at `position` in `buffer` fades out over kFadeSeconds (replacing an earlier tail).
void AuxProcessor::startTail(BackingTrackBuffer* buffer, int64_t position) {
    tailActive = buffer != nullptr;
    tailBuffer = buffer;
    tailPos = position;
    tailLeft = std::max(1, (int)(kFadeSeconds * sr));
}

void AuxProcessor::renderTrack(float* left, float* right, int n, double beatStart, double bpm, bool playing,
                               bool seeked) {
    const auto r = std::memory_order_relaxed;
    BackingTrackBuffer* const previous = lastTrack;
    BackingTrackBuffer* const t = track->acquire(); // keeps `previous` alive for its fade-out
    const bool swapped = t != previous;
    if (swapped) {
        // A tail still on an even older buffer lost its protection inside acquire(): drop it
        // (it is not read again). Then fade the old buffer out where it was playing.
        if (tailActive && tailBuffer != previous) tailActive = false;
        if (trackActive && previous != nullptr) startTail(previous, trackPos);
        tailProtected = previous != nullptr;
        lastTrack = t;
        trackActive = false;
    }
    const bool loop = settings->trackLoop.load(r);
    const bool enabled = settings->trackEnabled.load(r);
    trackGain.setTargetValue(settings->trackLevel.load(r));
    const bool playable = t != nullptr && t->sampleRate == sr && t->length > 0;
    const int fadeLen = std::max(1, (int)(kFadeSeconds * sr));

    if (playable) {
        const int64_t len = t->length;
        const bool sounding = playing && enabled;
        // Beat → track position: sample 0 is beat 0, at the current tempo.
        const int64_t mapped = (int64_t)std::llround(beatStart * 60.0 / std::max(bpm, 1e-6) * sr);
        if (sounding) {
            if (!trackActive) { // start, resume, or a new buffer
                trackPos = loop ? ((mapped % len) + len) % len : mapped;
                mainFadeLeft = (trackPos == 0 && !tailActive) ? 0 : fadeLen;
                trackActive = true;
            } else if (seeked) {
                startTail(t, trackPos);
                trackPos = loop ? ((mapped % len) + len) % len : mapped;
                mainFadeLeft = fadeLen;
            }
            // otherwise continue: the position advances by the samples rendered
        } else if (trackActive) { // stopped / disabled: fade out where it was
            startTail(t, trackPos);
            trackActive = false;
        }
    } else if (trackActive) {
        // Not playable (no buffer, or one at another rate): nothing to continue.
        trackActive = false;
    }
    // A tail on the current buffer no longer needs the protected previous one.
    if (tailActive && tailBuffer == t && tailProtected) { track->releaseTail(); tailProtected = false; }

    if (!trackActive && !tailActive) {
        if (tailProtected) { track->releaseTail(); tailProtected = false; }
        diag.trackPositionSec.store(-1.0, r);
        return;
    }

    const bool tailPlayable = tailActive && tailBuffer->sampleRate == sr && tailBuffer->length > 0;
    if (tailActive && !tailPlayable) tailActive = false;
    auto read = [loop](const BackingTrackBuffer* b, int64_t p, int ch) -> float {
        const int64_t len = b->length;
        const float* src = b->audio.getReadPointer(ch);
        if (loop) return src[((p % len) + len) % len];
        return (p >= 0 && p < len) ? src[p] : 0.0f;
    };
    for (int i = 0; i < n; ++i) {
        float l = 0.0f, rr = 0.0f;
        if (trackActive) {
            float g = 1.0f;
            if (mainFadeLeft > 0) { g = 1.0f - (float)mainFadeLeft / (float)fadeLen; --mainFadeLeft; }
            l += read(t, trackPos, 0) * g;
            rr += read(t, trackPos, 1) * g;
            ++trackPos;
        }
        if (tailActive) {
            const float g = (float)tailLeft / (float)fadeLen;
            l += read(tailBuffer, tailPos, 0) * g;
            rr += read(tailBuffer, tailPos, 1) * g;
            ++tailPos;
            if (--tailLeft <= 0) tailActive = false;
        }
        const float level = trackGain.getNextValue();
        left[i] += l * level;
        right[i] += rr * level;
    }
    if (!tailActive && tailProtected) { track->releaseTail(); tailProtected = false; }
    if (trackActive) {
        const int64_t len = t->length;
        diag.trackPositionSec.store((double)(loop ? ((trackPos % len) + len) % len : trackPos) / sr, r);
    } else {
        diag.trackPositionSec.store(-1.0, r);
    }
}
