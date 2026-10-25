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

    bool advanceBlock(int numSamples, double sampleRate);

private:
    std::atomic<double> bpm;
    std::atomic<bool> playing;

    // Single atomic for pending seek — NaN means no pending seek
    std::atomic<double> pendingPosition;

    double beatPosition = 0.0;
    int lastBeatInt = -1;

    std::atomic<double> positionSnapshot;

    static_assert(std::atomic<double>::is_always_lock_free,
                  "std::atomic<double> must be lock-free for audio thread safety");
};
