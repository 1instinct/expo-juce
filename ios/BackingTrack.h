#pragma once

// Pure C++ (no Objective-C): compiled into the iOS module and by the offline harness.
//
// Backing track: decoding + loop region + resampling (control side, never the audio thread) and
// the hand-over of the playable buffer to the audio thread.
//
// Hand-over: the control side publishes a new BackingTrackBuffer by an atomic pointer swap. The
// single reader (the AuxProcessor rendering at the moment) acknowledges the buffer it uses at the
// start of every block (acquire(): a hazard pointer, re-validated against the published pointer).
// A replaced buffer goes to a retired list and is deleted by collect() on a control thread only
// once the reader has moved past it; the audio thread never allocates or frees.

#include "JuceConfig.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// MP3 gapless information of the source file (frames at the source rate).
struct BackingTrackGapless {
    int64_t loopSamples = 0;
    int64_t leadingSamples = 0;
    int64_t trailingSamples = 0;
};

// Result of a decode (also what loadBackingTrack resolves with).
struct BackingTrackInfo {
    double durationSec = 0.0;   // loop region length / source rate
    double sampleRate = 0.0;    // source rate
    int channels = 0;           // source channels
    int64_t loopStartSample = 0; // region start in decoded source frames
    int64_t loopSamples = 0;     // region length in source frames
    int64_t decodedSamples = 0;  // frames the decoder produced (before the region cut)
    std::string formatName;      // reader used ("CoreAudio", "MP3 file", "WAV file", …)
};

// Loop region rule for MP3 seeds with a LAME/Xing gapless tag. The decoder honoured the tag
// (decoded == loopSamples) → [0, loopSamples); it ignored the tag and decoded the tag frame as
// silence (decoded == 1152 + leading + loopSamples + trailing) → [1152 + leading, +loopSamples);
// it skipped the tag frame but kept the padding (decoded == leading + loopSamples + trailing, e.g.
// JUCE's MP3 decoder) → [leading, +loopSamples); anything else (or no gapless info) → the whole file.
struct BackingTrackRegion { int64_t start; int64_t length; };
BackingTrackRegion backingTrackRegion(int64_t decodedSamples, const BackingTrackGapless* gapless);

// Decoded audio at the source rate, region already applied, always 2 channels.
struct DecodedBackingTrack {
    juce::AudioBuffer<float> audio;
    double sampleRate = 0.0;
    BackingTrackInfo info;
};

// Playable buffer: stereo at the device rate.
struct BackingTrackBuffer {
    BackingTrackBuffer() { live.fetch_add(1, std::memory_order_relaxed); }
    ~BackingTrackBuffer() { live.fetch_sub(1, std::memory_order_relaxed); }
    BackingTrackBuffer(const BackingTrackBuffer&) = delete;
    BackingTrackBuffer& operator=(const BackingTrackBuffer&) = delete;

    juce::AudioBuffer<float> audio;
    double sampleRate = 0.0;
    int length = 0;

    static inline std::atomic<int> live{0}; // instances alive (tests check the deferred frees)
};

class BackingTrack {
public:
    static constexpr double kMaxSeconds = 600.0;

    BackingTrack() = default;
    ~BackingTrack();
    BackingTrack(const BackingTrack&) = delete;
    BackingTrack& operator=(const BackingTrack&) = delete;

    // ── Control side (any non-audio thread) ──

    // Decodes `file` (JUCE AudioFormatManager basic formats: WAV, AIFF, FLAC, MP3 when enabled, plus
    // CoreAudio on Apple), cuts the loop region, converts to stereo. `cancelled` is polled between
    // chunks. Returns nullptr and sets `error` ("ERR_…: message") on failure.
    static std::unique_ptr<DecodedBackingTrack> decode(const juce::File& file, const BackingTrackGapless* gapless,
                                                       const std::function<bool()>& cancelled, std::string& error);
    // Cyclic 4-point (Hermite) resample of the whole loop to `targetRate`; the output length is
    // round(length × target / source) and the loop seam stays continuous.
    static std::unique_ptr<BackingTrackBuffer> resample(const DecodedBackingTrack& source, double targetRate);

    // Load generations: a load takes a generation; any later beginLoad / unload supersedes it.
    uint64_t beginLoad() { return generation.fetch_add(1, std::memory_order_acq_rel) + 1; }
    bool isCurrent(uint64_t gen) const { return generation.load(std::memory_order_acquire) == gen; }

    // Keeps `source` for re-resampling and publishes it resampled to the wanted rate (the
    // reader's rate, or the source rate while none is known). false if `gen` was superseded.
    bool install(std::unique_ptr<DecodedBackingTrack> source, uint64_t gen);
    // Drops the track (supersedes any load in flight).
    void unload();

    // The reader's rate (AuxProcessor::prepareToPlay). If the published buffer has another rate,
    // calls the rate listener, which must schedule resampleToWanted() off the calling thread.
    void requestRate(double sampleRate);
    void setRateListener(std::function<void(double)> listener);
    // Re-resamples the kept source to the wanted rate if the published buffer differs.
    void resampleToWanted();
    double wantedSampleRate() const { return wantedRate.load(std::memory_order_relaxed); }
    bool hasTrack() const; // any thread, lock-free

    // Deletes retired buffers the reader no longer uses. Returns how many are still waiting.
    int collect();

    // ── Reader (the audio thread; one reader at a time) ──

    // The buffer to play in this block (nullptr = none). Acknowledges it: buffers published
    // before it are no longer referenced by the reader once this returns.
    BackingTrackBuffer* acquire() noexcept {
        BackingTrackBuffer* p = published.load(std::memory_order_seq_cst);
        for (;;) {
            inUse.store(p, std::memory_order_seq_cst);
            BackingTrackBuffer* again = published.load(std::memory_order_seq_cst);
            if (again == p) return p;
            p = again;
        }
    }
    // The reader is gone (processor destroyed): it references no buffer any more.
    void releaseReader() noexcept { inUse.store(nullptr, std::memory_order_seq_cst); }

private:
    void publishLocked(std::unique_ptr<BackingTrackBuffer> next);

    std::atomic<BackingTrackBuffer*> published{nullptr};
    std::atomic<BackingTrackBuffer*> inUse{nullptr};
    std::atomic<uint64_t> generation{0};
    std::atomic<double> wantedRate{0.0};
    std::atomic<bool> sourceLoaded{false}; // lock-free hasTrack()
    std::atomic<double> publishedRateAtomic{0.0};

    mutable std::mutex mutex; // control side only
    std::unique_ptr<DecodedBackingTrack> source;
    double publishedRate = 0.0;
    std::vector<BackingTrackBuffer*> retired;
    std::function<void(double)> rateListener; // listenerMutex
    std::mutex listenerMutex; // guards rateListener only (never held while resampling)
};
