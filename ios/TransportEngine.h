#pragma once

#include <atomic>
#include <cmath>

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
