#pragma once

#include <atomic>

// Tempo, position and play state; carried across an engine rebuild, which replaces the transport.
struct TransportState {
    double bpm;
    double position; // beats
    bool playing;
};

class TransportEngine {
public:
    TransportEngine();

    void setTempo(double newBpm);
    double getTempo();

    void setPosition(double beat);
    double getPosition();

    void start();
    void stop();
    bool isPlaying();

    // Any thread. getState includes a seek the audio thread has not applied yet.
    TransportState getState();
    // Restores tempo, position and play state; getPosition reports the position at once.
    void restoreState(const TransportState& s);

    // Audio thread, once per rendered block (PolySynthProcessor calls it after rendering).
    // Returns true if a beat boundary was crossed.
    bool advanceBlock(int numSamples, double sampleRate);

    // Transport state at the START of the block most recently passed to advanceBlock, i.e. the
    // block the graph is rendering right now. Audio thread only: read by processors that run
    // after the synth in the same graph pass (FxChainProcessor). By then advanceBlock has already
    // moved the position to the block's end, so they read this instead of getPosition().
    double getBlockStartBeat() const { return blockStartBeat; }
    double getBlockBpm() const { return blockBpm; }
    bool isBlockPlaying() const { return blockPlaying; }

private:
    std::atomic<double> bpm;
    std::atomic<bool> playing;

    // Single atomic for pending seek — NaN means no pending seek
    std::atomic<double> pendingPosition;

    double beatPosition = 0.0;
    int lastBeatInt = -1;

    // Audio thread only (see getBlockStartBeat).
    double blockStartBeat = 0.0;
    double blockBpm = 120.0;
    bool blockPlaying = false;

    std::atomic<double> positionSnapshot;

    static_assert(std::atomic<double>::is_always_lock_free,
                  "std::atomic<double> must be lock-free for audio thread safety");
};
