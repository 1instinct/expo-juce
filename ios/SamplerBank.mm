#include "SamplerBank.h"
#include "Hermite.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <new>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#include <os/proc.h>
#define SAMPLER_BANK_HAS_OS_PROC 1
#endif
#endif

namespace {

std::mutex& providerMutex() { static std::mutex m; return m; }
std::function<int64_t()>& provider() { static std::function<int64_t()> p; return p; }

std::string megabytes(int64_t bytes) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    return text;
}

std::string slotError(int slot) {
    return "ERR_SAMPLER_SLOT: slot " + std::to_string(slot) + " is outside 0.." + std::to_string(kSamplerSlots - 1);
}

const char* const kSuperseded = "ERR_SAMPLER_SUPERSEDED: a newer load or unload of this slot replaced this one";

// Resamples one channel of n frames to m frames, reading the source every `step` frames with
// 4-point Hermite interpolation; reads past either end take the edge sample (a one-shot, not a loop).
void resampleChannel(const float* x, int n, float* y, int m, double step) {
    auto at = [x, n](int64_t i) { return x[i < 0 ? 0 : (i >= n ? n - 1 : i)]; };
    for (int i = 0; i < m; ++i) {
        const double pos = (double)i * step;
        const int64_t k = (int64_t)std::floor(pos);
        const float t = (float)(pos - (double)k);
        y[i] = hermite4(at(k - 1), at(k), at(k + 1), at(k + 2), t);
    }
}

} // namespace

void SamplerBank::setAvailableMemoryProvider(std::function<int64_t()> p) {
    std::lock_guard<std::mutex> lock(providerMutex());
    provider() = std::move(p);
}

int64_t SamplerBank::availableMemory() {
    {
        std::lock_guard<std::mutex> lock(providerMutex());
        if (provider()) return provider()();
    }
#if SAMPLER_BANK_HAS_OS_PROC
    if (__builtin_available(iOS 13.0, *)) return (int64_t)os_proc_available_memory();
#endif
    return -1;
}

SamplerBank::~SamplerBank() {
    for (auto& p : published) delete p.exchange(nullptr);
    for (auto* s : retired) delete s;
    retired.clear();
}

// ── Decode + resample (no lock held) ──

std::unique_ptr<SamplerSample> SamplerBank::decode(const std::string& path, double targetRate,
                                                   const std::function<bool()>& cancelled, int64_t budgetBytes,
                                                   SamplerLoadInfo& info, std::string& error) {
    const juce::File file(juce::String::fromUTF8(path.c_str()));
    if (path.empty() || !file.existsAsFile()) {
        error = "ERR_SAMPLER_FILE: file not found: " + path;
        return nullptr;
    }
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if (reader == nullptr) {
        error = "ERR_SAMPLER_FORMAT: no decoder for " + file.getFileName().toStdString();
        return nullptr;
    }
    const double rate = reader->sampleRate;
    const int64_t frames = reader->lengthInSamples;
    const int channels = (int)reader->numChannels;
    if (!(rate > 0.0) || frames <= 0 || channels <= 0) {
        error = "ERR_SAMPLER_FORMAT: empty or unreadable audio";
        return nullptr;
    }
    const double seconds = (double)frames / rate;
    if (seconds > kMaxSampleSeconds || frames > INT32_MAX) {
        char text[96];
        std::snprintf(text, sizeof(text), "ERR_SAMPLER_TOO_LONG: %.1f s; the limit is %.0f s", seconds, kMaxSampleSeconds);
        error = text;
        return nullptr;
    }
    const double target = targetRate > 0.0 ? targetRate : rate;
    const bool sameRate = target == rate;
    const int64_t outFrames = sameRate ? frames : std::max<int64_t>(1, std::llround((double)frames * target / rate));
    if (outFrames > INT32_MAX) {
        error = "ERR_SAMPLER_TOO_LONG: too many frames at the device rate";
        return nullptr;
    }
    const int64_t frameBytes = 2 * (int64_t)sizeof(float);
    const int64_t outBytes = outFrames * frameBytes;
    if (budgetBytes >= 0 && outBytes > budgetBytes) {
        error = "ERR_SAMPLER_MEMORY: needs " + megabytes(outBytes) + ", " + megabytes(std::max<int64_t>(0, budgetBytes))
              + " left under the sampler's cap";
        return nullptr;
    }
    const int64_t needed = outBytes + (sameRate ? 0 : frames * frameBytes);
    const int64_t available = availableMemory();
    if (available >= 0 && needed > available) {
        error = "ERR_SAMPLER_MEMORY: needs " + megabytes(needed) + ", " + megabytes(available) + " available";
        return nullptr;
    }

    auto sample = std::make_unique<SamplerSample>();
    try {
        // Same rate: decode straight into the playable buffer. Otherwise into a temporary one,
        // freed when this returns.
        juce::AudioBuffer<float> temp;
        juce::AudioBuffer<float>* dest = &sample->audio;
        if (sameRate) sample->audio.setSize(2, (int)frames);
        else { temp.setSize(2, (int)frames); dest = &temp; }
        constexpr int kChunk = 65536;
        for (int64_t done = 0; done < frames;) {
            if (cancelled && cancelled()) {
                error = kSuperseded;
                return nullptr;
            }
            const int n = (int)std::min<int64_t>(kChunk, frames - done);
            if (!reader->read(dest, (int)done, n, done, true, channels > 1)) {
                error = "ERR_SAMPLER_FILE: read failed at frame " + std::to_string(done) + " of " + std::to_string(frames);
                return nullptr;
            }
            if (channels == 1) dest->copyFrom(1, (int)done, *dest, 0, (int)done, n);
            done += n;
        }
        if (!sameRate) {
            if (cancelled && cancelled()) {
                error = kSuperseded;
                return nullptr;
            }
            sample->audio.setSize(2, (int)outFrames);
            const double step = rate / target;
            for (int ch = 0; ch < 2; ++ch)
                resampleChannel(temp.getReadPointer(ch), (int)frames, sample->audio.getWritePointer(ch), (int)outFrames, step);
        }
    } catch (const std::bad_alloc&) {
        error = "ERR_SAMPLER_MEMORY: allocation of " + megabytes(needed) + " failed";
        return nullptr;
    }

    sample->channels = std::min(channels, 2);
    sample->view = { sample->audio.getReadPointer(0), sample->audio.getReadPointer(1), (int)outFrames, target };
    info.frames = (int)outFrames;
    info.sampleRate = target;
    info.channels = channels;
    info.durationMs = seconds * 1000.0;
    info.sourceRate = rate;
    info.sourceFrames = frames;
    info.formatName = reader->getFormatName().toStdString();
    return sample;
}

std::unique_ptr<SamplerSample> SamplerBank::fromAudio(const float* L, const float* R, int frames, double rate) {
    auto sample = std::make_unique<SamplerSample>();
    frames = std::max(0, frames);
    if (R == nullptr) R = L;
    sample->audio.setSize(2, frames);
    if (frames > 0 && L != nullptr) {
        sample->audio.copyFrom(0, 0, L, frames);
        sample->audio.copyFrom(1, 0, R, frames);
    } else {
        sample->audio.clear();
    }
    sample->channels = (R == L) ? 1 : 2;
    sample->view = { sample->audio.getReadPointer(0), sample->audio.getReadPointer(1), frames, rate > 0.0 ? rate : 48000.0 };
    return sample;
}

// ── Holder ──

uint64_t SamplerBank::beginLoad(int slot) {
    if (slot < 0 || slot >= kSamplerSlots) return 0;
    return generation[slot].fetch_add(1, std::memory_order_acq_rel) + 1;
}

bool SamplerBank::isCurrent(int slot, uint64_t gen) const {
    return slot >= 0 && slot < kSamplerSlots && generation[slot].load(std::memory_order_acquire) == gen;
}

int64_t SamplerBank::residentBytesLocked(int skipSlot) const {
    int64_t total = 0;
    for (int s = 0; s < kSamplerSlots; ++s)
        if (s != skipSlot) total += infos[s].bytes;
    return total;
}

int64_t SamplerBank::retiredBytesLocked() const {
    int64_t total = 0;
    for (const SamplerSample* s : retired) total += s->bytes();
    return total;
}

bool SamplerBank::load(int slot, uint64_t gen, const std::string& path, SamplerLoadInfo& info, std::string& error,
                       const BeforePublish& beforePublish) {
    if (slot < 0 || slot >= kSamplerSlots) {
        error = slotError(slot);
        return false;
    }
    if (!isCurrent(slot, gen)) {
        error = kSuperseded;
        return false;
    }
    collect(); // frees what it can now, so stale retired samples do not shrink the budget (never blocks)
    int64_t budget;
    {
        std::lock_guard<std::mutex> lock(mutex);
        budget = std::max<int64_t>(0, memoryCapBytes() - residentBytesLocked(slot) - retiredBytesLocked());
    }
    auto sample = decode(path, wantedSampleRate(), [this, slot, gen] { return !isCurrent(slot, gen); }, budget, info, error);
    if (!sample) return false;
    return install(slot, std::move(sample), gen, error, beforePublish);
}

bool SamplerBank::install(int slot, std::unique_ptr<SamplerSample> sample, uint64_t gen, std::string& error,
                          const BeforePublish& beforePublish) {
    if (slot < 0 || slot >= kSamplerSlots) {
        error = slotError(slot);
        return false;
    }
    if (!sample || sample->view.frames <= 0) {
        error = "ERR_SAMPLER_FORMAT: empty audio";
        return false;
    }
    collect(); // retired samples the reader has let go of no longer count against the cap
    std::lock_guard<std::mutex> lock(mutex);
    if (!isCurrent(slot, gen)) {
        error = kSuperseded;
        return false;
    }
    const int64_t others = residentBytesLocked(slot) + retiredBytesLocked();
    const int64_t cap = memoryCapBytes();
    if (others + sample->bytes() > cap) {
        error = "ERR_SAMPLER_MEMORY: needs " + megabytes(sample->bytes()) + "; the other slots and samples not yet freed hold " + megabytes(others)
              + " of the " + megabytes(cap) + " cap";
        return false;
    }
    const int channels = sample->channels;
    reserveRetiredLocked(); // before beforePublish: if growing throws, nothing has changed
    if (beforePublish) beforePublish();
    publishLocked(slot, std::move(sample), channels);
    return true;
}

bool SamplerBank::unload(int slot) {
    if (slot < 0 || slot >= kSamplerSlots) return false;
    generation[slot].fetch_add(1, std::memory_order_acq_rel);
    std::lock_guard<std::mutex> lock(mutex);
    reserveRetiredLocked();
    publishLocked(slot, nullptr, 0);
    return true;
}

void SamplerBank::unloadAll() {
    for (int s = 0; s < kSamplerSlots; ++s) unload(s);
}

// Room for one more retired sample, so publishLocked's push_back never allocates (or throws) after
// its exchange.
void SamplerBank::reserveRetiredLocked() {
    if (retired.size() == retired.capacity()) retired.reserve(retired.capacity() * 2 + 2 * kSamplerSlots);
}

void SamplerBank::publishLocked(int slot, std::unique_ptr<SamplerSample> next, int channels) {
    SamplerSlotInfo info;
    if (next) {
        info.loaded = true;
        info.frames = next->view.frames;
        info.sampleRate = next->view.rate;
        info.channels = channels;
        info.durationMs = next->view.rate > 0.0 ? (double)next->view.frames * 1000.0 / next->view.rate : 0.0;
        info.bytes = next->bytes();
    }
    infos[slot] = info;
    SamplerSample* old = published[slot].exchange(next.release(), std::memory_order_seq_cst);
    if (old != nullptr) retired.push_back(old); // capacity reserved by the caller
}

int SamplerBank::collect() {
    std::unique_lock<std::mutex> lock(mutex, std::try_to_lock);
    if (!lock.owns_lock()) return -1;
    SamplerSample* reading[kSamplerSlots];
    SamplerSample* current[kSamplerSlots];
    for (int s = 0; s < kSamplerSlots; ++s) {
        reading[s] = inUse[s].load(std::memory_order_seq_cst);
        current[s] = published[s].load(std::memory_order_seq_cst);
    }
    auto it = std::remove_if(retired.begin(), retired.end(), [&](SamplerSample* b) {
        for (int s = 0; s < kSamplerSlots; ++s)
            if (b == reading[s] || b == current[s]) return false;
        delete b;
        return true;
    });
    retired.erase(it, retired.end());
    return (int)retired.size();
}

int64_t SamplerBank::retiredBytes() const {
    std::lock_guard<std::mutex> lock(mutex);
    return retiredBytesLocked();
}

int64_t SamplerBank::residentBytes() const {
    std::lock_guard<std::mutex> lock(mutex);
    return residentBytesLocked(-1);
}

void SamplerBank::setMemoryCapBytes(int64_t bytes) {
    memoryCap.store(std::max<int64_t>(0, bytes), std::memory_order_relaxed);
}

SamplerSlotInfo SamplerBank::slotInfo(int slot) const {
    if (slot < 0 || slot >= kSamplerSlots) return {};
    std::lock_guard<std::mutex> lock(mutex);
    return infos[slot];
}
