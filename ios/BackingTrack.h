#pragma once

// Pure C++ (no Objective-C): compiled into the iOS module and by the offline harness.
//
// Backing track: decoding + loop region + resampling (control side, never the audio thread) and
// the hand-over of the playable buffer to the audio thread.
//
// Hand-over: the control side publishes a new BackingTrackBuffer by an atomic pointer swap. The
// single reader (the AuxProcessor rendering at the moment) acknowledges the buffer it uses at the
// start of every block (acquire(): a hazard pointer, re-validated against the published pointer).
// When it moves to a new buffer, the previous one stays protected as its "tail" buffer (it fades
// the old one out over 5 ms) until it calls releaseTail(). A replaced buffer goes to a retired
// list and is deleted by collect() on a control thread only once the reader references it in
// neither slot; the audio thread never allocates or frees.
//
// Memory: only the playable buffer stays resident. The decoded source is not kept: when the
// device rate changes, the file is decoded again from its path (reload()). Heavy work (decode,
// resample) never runs under the holder's mutex.

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

// Test seam: the offline harness defines BACKING_TRACK_ACQUIRE_HOOK as the name of a function it
// provides; acquire() calls it between its first read of the published pointer and the
// re-validation, so a test can publish at exactly that point. Undefined in the module: no code.
#ifdef BACKING_TRACK_ACQUIRE_HOOK
void BACKING_TRACK_ACQUIRE_HOOK();
#endif

// MP3 gapless information of the source file (frames at the source rate).
struct BackingTrackGapless {
    int64_t loopSamples = 0;
    int64_t leadingSamples = 0;
    int64_t trailingSamples = 0;
};

// Which loop region the rule picked (reported to JS as `regionRule`).
enum class BackingTrackRegionRule : int {
    Tag = 0,          // decoded == loopSamples: the decoder honoured the gapless tag
    Padded,           // decoded == 1152 + leading + loop + trailing: tag frame decoded as silence
    PaddedNoTagFrame, // decoded == leading + loop + trailing: tag frame skipped, padding kept
    Whole,            // anything else, or no gapless info: the whole file
};
const char* backingTrackRegionRuleName(BackingTrackRegionRule rule); // "tag", "padded", "paddedNoTagFrame", "whole"

// Loop region rule for MP3 seeds with a LAME/Xing gapless tag (see BackingTrackRegionRule).
struct BackingTrackRegion { int64_t start; int64_t length; BackingTrackRegionRule rule; };
BackingTrackRegion backingTrackRegion(int64_t decodedSamples, const BackingTrackGapless* gapless);

// What to load: a local file path and its optional gapless info. Kept by the holder so the
// track can be decoded again at a new device rate.
struct BackingTrackSource {
    std::string path;
    bool hasGapless = false;
    BackingTrackGapless gapless;
};

// Result of a load (also what loadBackingTrack resolves with).
struct BackingTrackInfo {
    double durationSec = 0.0;    // loop region length / source rate
    double sampleRate = 0.0;     // source rate
    int channels = 0;            // source channels
    int64_t loopStartSample = 0; // region start in decoded source frames
    int64_t loopSamples = 0;     // region length in source frames
    int64_t decodedSamples = 0;  // frames the decoder produced (before the region cut)
    BackingTrackRegionRule regionRule = BackingTrackRegionRule::Whole;
    std::string formatName;      // reader used ("CoreAudio", "MP3 file", "WAV file", …)
    double playbackRate = 0.0;   // rate of the playable buffer
    int64_t playbackFrames = 0;  // its length
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
    // Length cap of the playable buffer, per channel: 6 min at 48 kHz (17 280 000 frames, about
    // 138 MB as stereo float), scaled with the rate (same duration at any rate).
    static constexpr int64_t kMaxFramesAt48k = 6 * 60 * 48000;
    static int64_t maxFrames(double sampleRate);

    // Bytes the process may still allocate, or -1 if unknown (iOS: os_proc_available_memory;
    // elsewhere unknown). Tests may replace it (nullptr = default).
    static void setAvailableMemoryProvider(std::function<int64_t()> provider);
    static int64_t availableMemory();

    BackingTrack() = default;
    ~BackingTrack();
    BackingTrack(const BackingTrack&) = delete;
    BackingTrack& operator=(const BackingTrack&) = delete;

    // ── Loading (any non-audio thread; no lock held while decoding or resampling) ──

    // Decodes `source` (JUCE AudioFormatManager basic formats: WAV, AIFF, FLAC, MP3 when enabled,
    // plus CoreAudio on Apple), cuts the loop region, converts to stereo and resamples to
    // `targetRate` (<= 0: keep the source rate). With equal rates the decoder writes straight into
    // the playable buffer (no second copy). Checks the frame cap and the available memory (decode
    // buffer + playable buffer) before allocating. `cancelled` is polled between chunks.
    // Returns nullptr and sets `error` ("ERR_…: message") on failure: ERR_BACKING_TRACK_FILE,
    // _FORMAT, _TOO_LONG, _MEMORY, _SUPERSEDED.
    static std::unique_ptr<BackingTrackBuffer> load(const BackingTrackSource& source, double targetRate,
                                                    const std::function<bool()>& cancelled, BackingTrackInfo& info,
                                                    std::string& error);
    // Cyclic 4-point (Hermite) resample of a whole loop to `targetRate`; the output length is
    // round(length × target / source) and the loop seam stays continuous. Throws std::bad_alloc.
    static std::unique_ptr<BackingTrackBuffer> resample(const juce::AudioBuffer<float>& source, double sourceRate,
                                                        double targetRate);

    // Load generations: a load takes a generation; any later beginLoad / unload supersedes it.
    uint64_t beginLoad() { return generation.fetch_add(1, std::memory_order_acq_rel) + 1; }
    bool isCurrent(uint64_t gen) const { return generation.load(std::memory_order_acquire) == gen; }
    // The rate a new load should resample to (the reader's rate; 0 while none is known).
    double wantedSampleRate() const { return wantedRate.load(std::memory_order_seq_cst); }

    // Publishes `buffer` (loaded from `source`; an empty path = cannot be reloaded at another
    // rate). false if `gen` was superseded (the buffer is then dropped). If the reader's rate
    // changed meanwhile, calls the rate listener.
    bool install(std::unique_ptr<BackingTrackBuffer> buffer, const BackingTrackSource& source, uint64_t gen);
    // Drops the track (supersedes any load in flight).
    void unload();

    // The reader's rate (AuxProcessor::prepareToPlay). Lock-free. If the published buffer has
    // another rate, calls the rate listener, which must schedule reload() off the calling thread.
    void requestRate(double sampleRate);
    void setRateListener(std::function<void(double)> listener);
    // Decodes the current source again at the wanted rate and publishes it, unless a load or
    // unload superseded it meanwhile. Heavy; call on a background thread. false = nothing done.
    bool reload(std::string* error = nullptr);
    bool hasTrack() const { return sourceLoaded.load(std::memory_order_seq_cst); } // any thread

    // Deletes retired buffers the reader no longer uses. Returns how many are still waiting, or
    // -1 if the holder was busy (try again later). Never blocks.
    int collect();

    // Control side, for tests / diagnostics: the published buffer (may be replaced at any time).
    const BackingTrackBuffer* publishedBuffer() const { return published.load(std::memory_order_acquire); }

    // ── Reader (the audio thread; one reader at a time) ──

    // The buffer to play in this block (nullptr = none), marked in use: buffers published before
    // it are no longer referenced by the reader once this returns, except the one it used before
    // a change, which stays protected (replacing an earlier one) until releaseTail().
    //
    // The tail slot is set inside the re-validation loop, on whichever pass first moves off
    // `previous` (a publish can land after the first read and only be seen by the re-check), and
    // always before inUse leaves `previous`: collect() reads inUse, then inUseTail, so at every
    // instant `previous` is in at least one of the slots it reads.
    BackingTrackBuffer* acquire() noexcept {
        BackingTrackBuffer* const previous = inUse.load(std::memory_order_relaxed); // written by the reader only
        BackingTrackBuffer* p = published.load(std::memory_order_seq_cst);
#ifdef BACKING_TRACK_ACQUIRE_HOOK
        BACKING_TRACK_ACQUIRE_HOOK();
#endif
        for (;;) {
            if (p != previous && previous != nullptr) inUseTail.store(previous, std::memory_order_seq_cst);
            inUse.store(p, std::memory_order_seq_cst);
            BackingTrackBuffer* again = published.load(std::memory_order_seq_cst);
            if (again == p) return p;
            p = again;
        }
    }
    // The reader no longer reads the previous buffer (its fade-out ended, or it had none).
    void releaseTail() noexcept { inUseTail.store(nullptr, std::memory_order_seq_cst); }
    // The reader is gone (processor destroyed): it references no buffer any more.
    void releaseReader() noexcept {
        inUseTail.store(nullptr, std::memory_order_seq_cst);
        inUse.store(nullptr, std::memory_order_seq_cst);
    }

private:
    void publishLocked(std::unique_ptr<BackingTrackBuffer> next);
    void notifyRateListener(double rate);

    std::atomic<BackingTrackBuffer*> published{nullptr};
    std::atomic<BackingTrackBuffer*> inUse{nullptr};
    std::atomic<BackingTrackBuffer*> inUseTail{nullptr};
    std::atomic<uint64_t> generation{0};
    std::atomic<double> wantedRate{0.0};
    std::atomic<double> publishedRate{0.0};
    std::atomic<bool> sourceLoaded{false};

    std::mutex mutex; // control side only, held for pointer / list updates (never a decode)
    BackingTrackSource source;
    std::vector<BackingTrackBuffer*> retired;

    std::mutex listenerMutex; // guards rateListener only
    std::function<void(double)> rateListener;
};
