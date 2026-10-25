#include "BackingTrack.h"
#include <algorithm>
#include <cmath>

BackingTrackRegion backingTrackRegion(int64_t decoded, const BackingTrackGapless* g) {
    if (g != nullptr && g->loopSamples > 0 && g->leadingSamples >= 0 && g->trailingSamples >= 0) {
        if (decoded == g->loopSamples) return { 0, g->loopSamples };
        if (decoded == 1152 + g->leadingSamples + g->loopSamples + g->trailingSamples)
            return { 1152 + g->leadingSamples, g->loopSamples };
    }
    return { 0, decoded };
}

BackingTrack::~BackingTrack() {
    delete published.exchange(nullptr);
    for (auto* b : retired) delete b;
    retired.clear();
}

std::unique_ptr<DecodedBackingTrack> BackingTrack::decode(const juce::File& file, const BackingTrackGapless* gapless,
                                                          const std::function<bool()>& cancelled, std::string& error) {
    if (!file.existsAsFile()) {
        error = "ERR_BACKING_TRACK_FILE: file not found: " + file.getFullPathName().toStdString();
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
    if ((double)decoded / rate > kMaxSeconds) {
        error = "ERR_BACKING_TRACK_TOO_LONG: longer than 600 s";
        return nullptr;
    }

    const BackingTrackRegion region = backingTrackRegion(decoded, gapless);
    auto out = std::make_unique<DecodedBackingTrack>();
    out->sampleRate = rate;
    out->audio.setSize(2, (int)region.length);
    out->audio.clear();
    const int readChannels = std::min(channels, 2);
    juce::AudioBuffer<float> chunk(readChannels, 65536);
    for (int64_t done = 0; done < region.length;) {
        if (cancelled && cancelled()) {
            error = "ERR_BACKING_TRACK_SUPERSEDED: a newer load or unload replaced this one";
            return nullptr;
        }
        const int n = (int)std::min<int64_t>(65536, region.length - done);
        reader->read(&chunk, 0, n, region.start + done, true, readChannels > 1);
        out->audio.copyFrom(0, (int)done, chunk, 0, 0, n);
        out->audio.copyFrom(1, (int)done, chunk, readChannels > 1 ? 1 : 0, 0, n);
        done += n;
    }
    out->info.durationSec = (double)region.length / rate;
    out->info.sampleRate = rate;
    out->info.channels = channels;
    out->info.loopStartSample = region.start;
    out->info.loopSamples = region.length;
    out->info.decodedSamples = decoded;
    out->info.formatName = reader->getFormatName().toStdString();
    return out;
}

std::unique_ptr<BackingTrackBuffer> BackingTrack::resample(const DecodedBackingTrack& src, double targetRate) {
    auto out = std::make_unique<BackingTrackBuffer>();
    const int n = src.audio.getNumSamples();
    if (!(targetRate > 0.0)) targetRate = src.sampleRate;
    out->sampleRate = targetRate;
    if (n <= 0) return out;
    if (targetRate == src.sampleRate) {
        out->audio.makeCopyOf(src.audio);
        out->length = n;
        return out;
    }
    const int m = (int)std::max<int64_t>(1, std::llround((double)n * targetRate / src.sampleRate));
    out->audio.setSize(2, m);
    out->length = m;
    // Step n / m (not the exact rate ratio) so m outputs cover the n inputs exactly once: the
    // loop stays seamless (pitch error < 1 / m).
    const double step = (double)n / (double)m;
    for (int ch = 0; ch < 2; ++ch) {
        const float* x = src.audio.getReadPointer(ch);
        float* y = out->audio.getWritePointer(ch);
        auto at = [x, n](int64_t i) { return x[((i % n) + n) % n]; };
        for (int i = 0; i < m; ++i) {
            const double pos = i * step;
            const int64_t k = (int64_t)std::floor(pos);
            const float t = (float)(pos - (double)k);
            const float xm1 = at(k - 1), x0 = at(k), x1 = at(k + 1), x2 = at(k + 2);
            // Catmull-Rom / 4-point Hermite
            const float c1 = 0.5f * (x1 - xm1);
            const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
            const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
            y[i] = ((c3 * t + c2) * t + c1) * t + x0;
        }
    }
    return out;
}

void BackingTrack::publishLocked(std::unique_ptr<BackingTrackBuffer> next) {
    publishedRate = next ? next->sampleRate : 0.0;
    publishedRateAtomic.store(publishedRate, std::memory_order_release);
    BackingTrackBuffer* old = published.exchange(next.release(), std::memory_order_seq_cst);
    if (old != nullptr) retired.push_back(old);
}

bool BackingTrack::install(std::unique_ptr<DecodedBackingTrack> src, uint64_t gen) {
    if (!src) return false;
    const double target = wantedRate.load(std::memory_order_relaxed);
    auto buffer = resample(*src, target > 0.0 ? target : src->sampleRate);
    std::lock_guard<std::mutex> lock(mutex);
    if (!isCurrent(gen)) return false;
    source = std::move(src);
    publishLocked(std::move(buffer));
    sourceLoaded.store(true, std::memory_order_seq_cst);
    // The reader's rate changed while resampling (requestRate saw no source to redo): redo now.
    const double now = wantedRate.load(std::memory_order_seq_cst);
    if (now > 0.0 && now != publishedRate) publishLocked(resample(*source, now));
    return true;
}

void BackingTrack::unload() {
    generation.fetch_add(1, std::memory_order_acq_rel);
    std::lock_guard<std::mutex> lock(mutex);
    source.reset();
    sourceLoaded.store(false, std::memory_order_release);
    publishLocked(nullptr);
}

void BackingTrack::setRateListener(std::function<void(double)> listener) {
    std::lock_guard<std::mutex> lock(listenerMutex);
    rateListener = std::move(listener);
}

void BackingTrack::requestRate(double sampleRate) {
    if (!(sampleRate > 0.0)) return;
    wantedRate.store(sampleRate, std::memory_order_seq_cst);
    // Lock-free check (the main mutex can be held for a whole resample): prepareToPlay must not wait.
    // seq_cst pairs with install(): either it sees this rate, or this sees its source.
    if (!sourceLoaded.load(std::memory_order_seq_cst) || publishedRateAtomic.load(std::memory_order_acquire) == sampleRate) return;
    std::function<void(double)> listener;
    {
        std::lock_guard<std::mutex> lock(listenerMutex);
        listener = rateListener;
    }
    if (listener) listener(sampleRate);
}

void BackingTrack::resampleToWanted() {
    std::lock_guard<std::mutex> lock(mutex);
    const double target = wantedRate.load(std::memory_order_relaxed);
    if (!source || !(target > 0.0) || publishedRate == target) return;
    publishLocked(resample(*source, target));
}

bool BackingTrack::hasTrack() const {
    return sourceLoaded.load(std::memory_order_acquire);
}

int BackingTrack::collect() {
    std::lock_guard<std::mutex> lock(mutex);
    BackingTrackBuffer* const reading = inUse.load(std::memory_order_seq_cst);
    BackingTrackBuffer* const current = published.load(std::memory_order_seq_cst);
    auto it = std::remove_if(retired.begin(), retired.end(), [&](BackingTrackBuffer* b) {
        if (b == reading || b == current) return false;
        delete b;
        return true;
    });
    retired.erase(it, retired.end());
    return (int)retired.size();
}
