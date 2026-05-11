#pragma once

#include <atomic>

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
    std::atomic<double> pendingPosition;
    std::atomic<bool> hasPendingPosition;

    double beatPosition = 0.0;
    int lastBeatInt = -1;

    std::atomic<double> positionSnapshot;
};
