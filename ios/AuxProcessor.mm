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

// Fade-in of the incoming head; 1 - this for the outgoing one (they always sum to 1).
inline float grainWeight(int phase, int hop) {
    const float x = (float)phase / (float)hop;
    return x * x * (3.0f - 2.0f * x);
}

// Where near `target` the track best continues what `outgoing` is about to play: normalised
// cross-correlation of the two channels' sum, over `compare` samples, every `stride` samples and
// then refined to the sample. Of near-equal places the nearest to `target` wins, and with nothing
// to compare (silence, the ends of a track that does not loop) `target` itself.
int64_t bestGrainStart(const BackingTrackBuffer& b, int64_t outgoing, int64_t target, int search, int compare,
                       int stride, bool loop) {
    constexpr int kMaxPoints = 256;
    const int64_t len = b.length;
    const int points = std::min(compare / stride, kMaxPoints);
    const int64_t span = (int64_t)points * stride;
    if (len < 2 * (search + span)) return target;
    if (!loop && (target < search || target + search + span >= len || outgoing < 0 || outgoing + span >= len)) return target;

    const float* left = b.audio.getReadPointer(0);
    const float* right = b.audio.getReadPointer(1);
    float reference[kMaxPoints];
    int64_t at = ((outgoing % len) + len) % len;
    float energy = 0.0f;
    for (int k = 0; k < points; ++k) {
        reference[k] = left[at] + right[at];
        energy += reference[k] * reference[k];
        at += stride;
        if (at >= len) at -= len;
    }
    if (energy < 1e-6f) return target;

    const auto score = [&](int offset) {
        int64_t p = ((target + offset) % len + len) % len;
        float xy = 0.0f, yy = 0.0f;
        for (int k = 0; k < points; ++k) {
            const float y = left[p] + right[p];
            xy += reference[k] * y;
            yy += y * y;
            p += stride;
            if (p >= len) p -= len;
        }
        const float distance = 1.0f - 0.1f * (float)std::abs(offset) / (float)search;
        return xy / std::sqrt(yy + 1e-9f) * distance;
    };
    int best = 0;
    float bestScore = 0.0f; // only a positive match moves it off the target
    for (int offset = -search; offset <= search; offset += stride) {
        const float s = score(offset);
        if (s > bestScore) { bestScore = s; best = offset; }
    }
    const int coarse = best;
    for (int offset = std::max(-search, coarse - stride + 1); offset <= std::min(search, coarse + stride - 1); ++offset) {
        const float s = score(offset);
        if (s > bestScore) { bestScore = s; best = offset; }
    }
    return target + best;
}

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
    grainHop = std::max(1, (int)std::lround(kGrainHopSeconds * sr));
    grainSearch = std::max(1, (int)std::lround(kGrainSearchSeconds * sr));
    grainCompare = std::max(1, (int)std::lround(kGrainCompareSeconds * sr));
    grainStride = std::max(1, (int)std::lround(sr / 12000.0));
    grainsActive = false;
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
    tailGrains = grainsActive;
    tailOutPos = grainOutPos;
    tailPhase = grainPhase;
    grainsActive = false;
}

// The incoming head becomes the outgoing one; a new incoming head starts at `sourcePosition`.
void AuxProcessor::startGrain(const BackingTrackBuffer& b, double sourcePosition, bool search, bool loop) {
    grainOutPos = trackPos;
    const int64_t target = std::llround(sourcePosition);
    trackPos = search ? bestGrainStart(b, grainOutPos, target, grainSearch, grainCompare, grainStride, loop) : target;
    grainPhase = 0;
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
    // The track's own tempo, if it has one, is its beat grid; without one it plays at its own speed.
    const double sourceBpm = playable ? t->sourceBpm : 0.0;
    const double gridBpm = sourceBpm > 0.0 ? sourceBpm : bpm;
    const bool follow = sourceBpm > 0.0 && std::fabs(bpm - sourceBpm) > 1e-6 * sourceBpm;

    if (playable) {
        const int64_t len = t->length;
        const bool sounding = playing && enabled;
        // Beat → track position: sample 0 is beat 0.
        const int64_t mapped = (int64_t)std::llround(beatStart * 60.0 / std::max(gridBpm, 1e-6) * sr);
        if (sounding) {
            if (!trackActive) { // start, resume, or a new buffer
                trackPos = loop ? ((mapped % len) + len) % len : mapped;
                mainFadeLeft = (trackPos == 0 && !tailActive) ? 0 : fadeLen;
                trackActive = true;
                grainsActive = false;
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
    if (trackActive && follow && !grainsActive) { // the first sample below starts a grain
        grainsActive = true;
        grainPhase = grainHop;
    }
    if (trackActive && follow) grainsOnGrid = false;
    diag.trackTempoRatio.store(trackActive && sourceBpm > 0.0 ? bpm / sourceBpm : 1.0, r);
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
    const double beatsPerSample = bpm / (60.0 * sr);
    const double samplesPerBeat = 60.0 / std::max(gridBpm, 1e-6) * sr;
    for (int i = 0; i < n; ++i) {
        float l = 0.0f, rr = 0.0f;
        if (trackActive) {
            float g = 1.0f;
            if (mainFadeLeft > 0) { g = 1.0f - (float)mainFadeLeft / (float)fadeLen; --mainFadeLeft; }
            if (grainsActive && grainPhase >= grainHop) {
                if (grainsOnGrid) {
                    grainsActive = false; // the incoming head is on the grid: the plain path takes it
                } else {
                    // The grain's middle, a hop from now, is where its position should be on the beat.
                    const double beat = beatStart + i * beatsPerSample;
                    const double source = beat * samplesPerBeat + (beatsPerSample * samplesPerBeat - 1.0) * grainHop;
                    startGrain(*t, source, follow, loop);
                    grainsOnGrid = !follow;
                }
            }
            if (grainsActive) {
                const float w = grainWeight(grainPhase, grainHop);
                l += (read(t, grainOutPos, 0) * (1.0f - w) + read(t, trackPos, 0) * w) * g;
                rr += (read(t, grainOutPos, 1) * (1.0f - w) + read(t, trackPos, 1) * w) * g;
                ++grainOutPos;
                ++grainPhase;
            } else {
                l += read(t, trackPos, 0) * g;
                rr += read(t, trackPos, 1) * g;
            }
            ++trackPos;
        }
        if (tailActive) {
            const float g = (float)tailLeft / (float)fadeLen;
            if (tailGrains && tailPhase < grainHop) {
                const float w = grainWeight(tailPhase, grainHop);
                l += (read(tailBuffer, tailOutPos, 0) * (1.0f - w) + read(tailBuffer, tailPos, 0) * w) * g;
                rr += (read(tailBuffer, tailOutPos, 1) * (1.0f - w) + read(tailBuffer, tailPos, 1) * w) * g;
                ++tailOutPos;
                ++tailPhase;
            } else {
                l += read(tailBuffer, tailPos, 0) * g;
                rr += read(tailBuffer, tailPos, 1) * g;
            }
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
