#pragma once

// Pure C++ (no Objective-C): compiled into the iOS module and by the offline harness.
//
// Sampler slots: decoding + resampling (a loader thread, never the audio thread) and the hand-off
// of each slot's playable buffer to the audio thread (SamplerEngine reads them through
// SamplerBufferSource at the start of every block).
//
// Hand-off, per slot, like the backing track's: the control side publishes a new SamplerSample by
// an atomic pointer swap. The single reader acknowledges the buffer it uses in acquireSlot() (a
// hazard pointer, re-validated against the published pointer). The engine stops reading a slot's
// previous buffer the moment acquireSlot returns a different one (its voices fade their last
// output instead), so one hazard slot per sample slot is enough. A replaced buffer goes to a
// retired list and is deleted by collect() on a control thread once the reader no longer marks it;
// the audio thread never allocates or frees.
//
// Loads: one generation counter per slot. beginLoad() takes a generation; a later beginLoad or
// unload of that slot supersedes it (checked between decode chunks and before publishing).
// Limits: kMaxSampleSeconds per sample and memoryCapBytes() (256 MB by default) across all slots
// (the slot being replaced does not count), plus the process's available memory.
// Errors are "ERR_SAMPLER_<KIND>: message": _SLOT, _FILE, _FORMAT, _TOO_LONG, _MEMORY, _SUPERSEDED.

#include "JuceConfig.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include "SamplerEngine.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// A playable sample: stereo at `view.rate`. Immutable once published.
struct SamplerSample {
    SamplerSample() { live.fetch_add(1, std::memory_order_relaxed); }
    ~SamplerSample() { live.fetch_sub(1, std::memory_order_relaxed); }
    SamplerSample(const SamplerSample&) = delete;
    SamplerSample& operator=(const SamplerSample&) = delete;

    juce::AudioBuffer<float> audio; // 2 channels
    SampleBuffer view{};            // points into `audio`
    int channels = 0;               // of the source (1 = mono, copied to both channels)
    int64_t bytes() const { return (int64_t)view.frames * 2 * (int64_t)sizeof(float); }

    static inline std::atomic<int> live{0}; // instances alive (tests check the deferred frees)
};

// What a load reports (and loadSamplerSlot resolves with).
struct SamplerLoadInfo {
    int frames = 0;           // of the playable buffer
    double sampleRate = 0.0;  // of the playable buffer (the device rate when it was known)
    int channels = 0;         // of the source
    double durationMs = 0.0;
    double sourceRate = 0.0;
    int64_t sourceFrames = 0;
    std::string formatName;   // reader used ("WAV file", "MP3 file", "CoreAudio", …)
};

// A slot as the control side sees it.
struct SamplerSlotInfo {
    bool loaded = false;
    int frames = 0;
    double sampleRate = 0.0;
    int channels = 0;
    double durationMs = 0.0;
    int64_t bytes = 0;
};

class SamplerBank : public SamplerBufferSource {
public:
    static constexpr int64_t kDefaultMemoryCapBytes = (int64_t)256 * 1024 * 1024;
    static constexpr double kMaxSampleSeconds = 60.0;

    // Bytes the process may still allocate, or -1 if unknown (iOS: os_proc_available_memory;
    // elsewhere unknown). Tests may replace it (nullptr = default).
    static void setAvailableMemoryProvider(std::function<int64_t()> provider);
    static int64_t availableMemory();

    SamplerBank() = default;
    ~SamplerBank() override;
    SamplerBank(const SamplerBank&) = delete;
    SamplerBank& operator=(const SamplerBank&) = delete;

    // ── Loading (any non-audio thread; no lock held while decoding or resampling) ──

    // Decodes `path` (JUCE basic formats: WAV, AIFF, FLAC, MP3 when enabled, plus CoreAudio on
    // Apple), converts to stereo (mono to both channels, the first two of more) and resamples to
    // `targetRate` (<= 0: keep the source rate; non-looping 4-point Hermite). Refuses before
    // allocating: over kMaxSampleSeconds, more than `budgetBytes` (-1 = no budget) or more than
    // the available memory. `cancelled` is polled between chunks.
    static std::unique_ptr<SamplerSample> decode(const std::string& path, double targetRate,
                                                 const std::function<bool()>& cancelled, int64_t budgetBytes,
                                                 SamplerLoadInfo& info, std::string& error);
    // A sample from audio already in memory (copied; R may equal L). Control side.
    static std::unique_ptr<SamplerSample> fromAudio(const float* L, const float* R, int frames, double rate);

    uint64_t beginLoad(int slot);
    bool isCurrent(int slot, uint64_t gen) const;
    // decode() at the wanted rate within what the cap leaves, then install(). Blocking: a loader thread.
    bool load(int slot, uint64_t gen, const std::string& path, SamplerLoadInfo& info, std::string& error);
    // Publishes `sample` in `slot` unless `gen` was superseded or the cap would be exceeded.
    bool install(int slot, std::unique_ptr<SamplerSample> sample, uint64_t gen, std::string& error);
    // Empties the slot (supersedes a load in flight). false: slot out of range.
    bool unload(int slot);
    void unloadAll();

    // Deletes retired samples the reader no longer uses. Returns how many are still waiting, or -1
    // if the bank was busy (try again later). Never blocks.
    int collect();

    int64_t residentBytes() const;       // published samples, all slots
    void setMemoryCapBytes(int64_t bytes);
    int64_t memoryCapBytes() const { return memoryCap.load(std::memory_order_relaxed); }

    // The reader's rate (PolySynthProcessor::prepareToPlay): later loads resample to it. Lock-free.
    // Samples already loaded keep their rate; the engine plays them at their own speed.
    void requestRate(double sampleRate) {
        if (sampleRate > 0.0) wantedRate.store(sampleRate, std::memory_order_relaxed);
    }
    double wantedSampleRate() const { return wantedRate.load(std::memory_order_relaxed); }

    SamplerSlotInfo slotInfo(int slot) const; // control side

    // ── Reader (the audio thread; one reader at a time) ──

    const SampleBuffer* acquireSlot(int slot) noexcept override {
        if (slot < 0 || slot >= kSamplerSlots) return nullptr;
        SamplerSample* p = published[slot].load(std::memory_order_seq_cst);
        for (;;) {
            inUse[slot].store(p, std::memory_order_seq_cst);
            SamplerSample* again = published[slot].load(std::memory_order_seq_cst);
            if (again == p) return p != nullptr ? &p->view : nullptr;
            p = again;
        }
    }
    void releaseReader() noexcept override {
        for (auto& u : inUse) u.store(nullptr, std::memory_order_seq_cst);
    }

private:
    void publishLocked(int slot, std::unique_ptr<SamplerSample> next, int channels);
    int64_t residentBytesLocked(int skipSlot) const;

    std::atomic<SamplerSample*> published[kSamplerSlots] = {};
    std::atomic<SamplerSample*> inUse[kSamplerSlots] = {};
    std::atomic<uint64_t> generation[kSamplerSlots] = {};
    std::atomic<double> wantedRate{0.0};
    std::atomic<int64_t> memoryCap{kDefaultMemoryCapBytes};

    mutable std::mutex mutex; // control side only, held for pointer / list updates (never a decode)
    std::vector<SamplerSample*> retired;
    SamplerSlotInfo infos[kSamplerSlots];
};
