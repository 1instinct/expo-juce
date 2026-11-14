#include "BackingTrack.h"
#include "Hermite.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <new>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#include <os/proc.h>
#define BACKING_TRACK_HAS_OS_PROC 1
#endif
#endif

// ── Region rule ──

const char* backingTrackRegionRuleName(BackingTrackRegionRule rule) {
    switch (rule) {
        case BackingTrackRegionRule::Tag: return "tag";
        case BackingTrackRegionRule::Padded: return "padded";
        case BackingTrackRegionRule::PaddedNoTagFrame: return "paddedNoTagFrame";
        case BackingTrackRegionRule::Whole: default: return "whole";
    }
}

BackingTrackRegion backingTrackRegion(int64_t decoded, const BackingTrackGapless* g) {
    if (g != nullptr && g->loopSamples > 0 && g->leadingSamples >= 0 && g->trailingSamples >= 0) {
        if (decoded == g->loopSamples) return { 0, g->loopSamples, BackingTrackRegionRule::Tag };
        if (decoded == 1152 + g->leadingSamples + g->loopSamples + g->trailingSamples)
            return { 1152 + g->leadingSamples, g->loopSamples, BackingTrackRegionRule::Padded };
        if (decoded == g->leadingSamples + g->loopSamples + g->trailingSamples)
            return { g->leadingSamples, g->loopSamples, BackingTrackRegionRule::PaddedNoTagFrame };
    }
    return { 0, decoded, BackingTrackRegionRule::Whole };
}

// ── Limits ──

int64_t BackingTrack::maxFrames(double sampleRate) {
    if (!(sampleRate > 0.0)) sampleRate = 48000.0;
    return (int64_t)std::llround((double)kMaxFramesAt48k * sampleRate / 48000.0);
}

namespace {
std::mutex& providerMutex() { static std::mutex m; return m; }
std::function<int64_t()>& provider() { static std::function<int64_t()> p; return p; }

std::string megabytes(int64_t bytes) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    return text;
}
} // namespace

void BackingTrack::setAvailableMemoryProvider(std::function<int64_t()> p) {
    std::lock_guard<std::mutex> lock(providerMutex());
    provider() = std::move(p);
}

int64_t BackingTrack::availableMemory() {
    {
        std::lock_guard<std::mutex> lock(providerMutex());
        if (provider()) return provider()();
    }
#if BACKING_TRACK_HAS_OS_PROC
    if (__builtin_available(iOS 13.0, *)) return (int64_t)os_proc_available_memory();
#endif
    return -1;
}

// ── Decode + resample (no lock held) ──

std::unique_ptr<BackingTrackBuffer> BackingTrack::resample(const juce::AudioBuffer<float>& src, double sourceRate,
                                                           double targetRate) {
    auto out = std::make_unique<BackingTrackBuffer>();
    const int n = src.getNumSamples();
    if (!(targetRate > 0.0)) targetRate = sourceRate;
    out->sampleRate = targetRate;
    if (n <= 0) return out;
    if (targetRate == sourceRate) {
        out->audio.makeCopyOf(src);
        out->length = n;
        return out;
    }
    const int m = (int)std::max<int64_t>(1, std::llround((double)n * targetRate / sourceRate));
    out->audio.setSize(2, m);
    out->length = m;
    // Step n / m (not the exact rate ratio) so m outputs cover the n inputs exactly once: the
    // loop stays seamless (pitch error < 1 / m).
    const double step = (double)n / (double)m;
    for (int ch = 0; ch < 2; ++ch) {
        const float* x = src.getReadPointer(std::min(ch, src.getNumChannels() - 1));
        float* y = out->audio.getWritePointer(ch);
        auto at = [x, n](int64_t i) { return x[((i % n) + n) % n]; };
        for (int i = 0; i < m; ++i) {
            const double pos = i * step;
            const int64_t k = (int64_t)std::floor(pos);
            const float t = (float)(pos - (double)k);
            y[i] = hermite4(at(k - 1), at(k), at(k + 1), at(k + 2), t); // Catmull-Rom / 4-point Hermite
        }
    }
    return out;
}

std::unique_ptr<BackingTrackBuffer> BackingTrack::load(const BackingTrackSource& src, double targetRate,
                                                       const std::function<bool()>& cancelled, BackingTrackInfo& info,
                                                       std::string& error) {
    const juce::File file(juce::String::fromUTF8(src.path.c_str()));
    if (src.path.empty() || !file.existsAsFile()) {
        error = "ERR_BACKING_TRACK_FILE: file not found: " + src.path;
        return nullptr;
    }
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if (reader == nullptr) {
        error = "ERR_BACKING_TRACK_FORMAT: no decoder for " + file.getFileName().toStdString();
        return nullptr;
    }
    const double rate = reader->sampleRate;
    const int64_t decoded = reader->lengthInSamples;
    const int channels = (int)reader->numChannels;
    if (!(rate > 0.0) || decoded <= 0 || channels <= 0) {
        error = "ERR_BACKING_TRACK_FORMAT: empty or unreadable audio";
        return nullptr;
    }

    const BackingTrackRegion region = backingTrackRegion(decoded, src.hasGapless ? &src.gapless : nullptr);
    const double target = targetRate > 0.0 ? targetRate : rate;
    const bool sameRate = target == rate;
    const int64_t finalFrames = sameRate ? region.length : std::max<int64_t>(1, std::llround((double)region.length * target / rate));
    if (region.length > maxFrames(rate) || finalFrames > maxFrames(target) || region.length > INT32_MAX) {
        char text[160];
        std::snprintf(text, sizeof(text), "ERR_BACKING_TRACK_TOO_LONG: %.1f s; the limit is %.0f s (%lld frames per channel at 48 kHz)",
                      (double)region.length / rate, (double)kMaxFramesAt48k / 48000.0, (long long)kMaxFramesAt48k);
        error = text;
        return nullptr;
    }
    const int64_t frameBytes = 2 * (int64_t)sizeof(float);
    const int64_t needed = finalFrames * frameBytes + (sameRate ? 0 : region.length * frameBytes);
    const int64_t available = availableMemory();
    if (available >= 0 && needed > available) {
        error = "ERR_BACKING_TRACK_MEMORY: needs " + megabytes(needed) + ", " + megabytes(available) + " available";
        return nullptr;
    }

    std::unique_ptr<BackingTrackBuffer> out;
    try {
        // Same rate: decode straight into the playable buffer. Otherwise into a temporary one,
        // freed when this returns.
        std::unique_ptr<BackingTrackBuffer> direct;
        juce::AudioBuffer<float> temp;
        juce::AudioBuffer<float>* dest = nullptr;
        if (sameRate) {
            direct = std::make_unique<BackingTrackBuffer>();
            direct->audio.setSize(2, (int)region.length);
            direct->sampleRate = rate;
            direct->length = (int)region.length;
            dest = &direct->audio;
        } else {
            temp.setSize(2, (int)region.length);
            dest = &temp;
        }
        constexpr int kChunk = 65536;
        for (int64_t done = 0; done < region.length;) {
            if (cancelled && cancelled()) {
                error = "ERR_BACKING_TRACK_SUPERSEDED: a newer load or unload replaced this one";
                return nullptr;
            }
            const int n = (int)std::min<int64_t>(kChunk, region.length - done);
            reader->read(dest, (int)done, n, region.start + done, true, channels > 1);
            if (channels == 1) dest->copyFrom(1, (int)done, *dest, 0, (int)done, n);
            done += n;
        }
        if (sameRate) {
            out = std::move(direct);
        } else {
            if (cancelled && cancelled()) {
                error = "ERR_BACKING_TRACK_SUPERSEDED: a newer load or unload replaced this one";
                return nullptr;
            }
            out = resample(temp, rate, target);
        }
    } catch (const std::bad_alloc&) {
        error = "ERR_BACKING_TRACK_MEMORY: allocation of " + megabytes(needed) + " failed";
        return nullptr;
    }

    const double sourceBpm = std::isfinite(src.sourceBpm) && src.sourceBpm > 0.0 ? src.sourceBpm : 0.0;
    out->sourceBpm = sourceBpm;
    info.sourceBpm = sourceBpm;
    info.durationSec = (double)region.length / rate;
    info.sampleRate = rate;
    info.channels = channels;
    info.loopStartSample = region.start;
    info.loopSamples = region.length;
    info.decodedSamples = decoded;
    info.regionRule = region.rule;
    info.formatName = reader->getFormatName().toStdString();
    info.playbackRate = out->sampleRate;
    info.playbackFrames = out->length;
    return out;
}

// ── Holder ──

BackingTrack::~BackingTrack() {
    delete published.exchange(nullptr);
    for (auto* b : retired) delete b;
    retired.clear();
}

void BackingTrack::publishLocked(std::unique_ptr<BackingTrackBuffer> next) {
    publishedRate.store(next ? next->sampleRate : 0.0, std::memory_order_seq_cst);
    BackingTrackBuffer* old = published.exchange(next.release(), std::memory_order_seq_cst);
    if (old != nullptr) retired.push_back(old);
}

void BackingTrack::notifyRateListener(double rate) {
    std::function<void(double)> listener;
    {
        std::lock_guard<std::mutex> lock(listenerMutex);
        listener = rateListener;
    }
    if (listener) listener(rate);
}

bool BackingTrack::install(std::unique_ptr<BackingTrackBuffer> buffer, const BackingTrackSource& src, uint64_t gen) {
    if (!buffer) return false;
    const double rate = buffer->sampleRate;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!isCurrent(gen)) return false;
        source = src;
        publishLocked(std::move(buffer));
        sourceLoaded.store(true, std::memory_order_seq_cst);
    }
    // The reader's rate differs (it changed while this was loading): load again at that rate.
    // seq_cst with requestRate(): either it sees this track, or this sees its rate.
    const double wanted = wantedRate.load(std::memory_order_seq_cst);
    if (wanted > 0.0 && wanted != rate && !src.path.empty()) notifyRateListener(wanted);
    return true;
}

void BackingTrack::unload() {
    generation.fetch_add(1, std::memory_order_acq_rel);
    std::lock_guard<std::mutex> lock(mutex);
    source = {};
    sourceLoaded.store(false, std::memory_order_seq_cst);
    publishLocked(nullptr);
}

void BackingTrack::setRateListener(std::function<void(double)> listener) {
    std::lock_guard<std::mutex> lock(listenerMutex);
    rateListener = std::move(listener);
}

void BackingTrack::requestRate(double sampleRate) {
    if (!(sampleRate > 0.0)) return;
    wantedRate.store(sampleRate, std::memory_order_seq_cst);
    if (!sourceLoaded.load(std::memory_order_seq_cst)) return;
    if (publishedRate.load(std::memory_order_seq_cst) == sampleRate) return;
    notifyRateListener(sampleRate);
}

bool BackingTrack::reload(std::string* errorOut) {
    BackingTrackSource src;
    uint64_t gen = 0;
    double target = 0.0;
    {
        std::lock_guard<std::mutex> lock(mutex);
        gen = generation.load(std::memory_order_acquire);
        target = wantedRate.load(std::memory_order_seq_cst);
        if (source.path.empty() || !(target > 0.0) || publishedRate.load(std::memory_order_seq_cst) == target) return false;
        src = source;
    }
    // The old buffer stays published (and counted by availableMemory) until the new one replaces
    // it: a rate that flips back meanwhile can still play it.
    BackingTrackInfo info;
    std::string error;
    auto buffer = load(src, target, [this, gen] { return !isCurrent(gen); }, info, error);
    if (!buffer) {
        if (errorOut) *errorOut = error;
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex);
    if (!isCurrent(gen) || wantedRate.load(std::memory_order_seq_cst) != target) return false;
    publishLocked(std::move(buffer));
    return true;
}

int BackingTrack::collect() {
    std::unique_lock<std::mutex> lock(mutex, std::try_to_lock);
    if (!lock.owns_lock()) return -1;
    BackingTrackBuffer* const reading = inUse.load(std::memory_order_seq_cst);
    BackingTrackBuffer* const tail = inUseTail.load(std::memory_order_seq_cst);
    BackingTrackBuffer* const current = published.load(std::memory_order_seq_cst);
    auto it = std::remove_if(retired.begin(), retired.end(), [&](BackingTrackBuffer* b) {
        if (b == reading || b == tail || b == current) return false;
        delete b;
        return true;
    });
    retired.erase(it, retired.end());
    return (int)retired.size();
}
