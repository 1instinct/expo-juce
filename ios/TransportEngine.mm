#include "TransportEngine.h"
#include <algorithm>

TransportEngine::TransportEngine()
    : bpm(120.0), playing(false), pendingPosition(-1.0),
      hasPendingPosition(false), positionSnapshot(0.0) {}

void TransportEngine::setTempo(double newBpm) {
    bpm.store(std::clamp(newBpm, 20.0, 400.0));
}

double TransportEngine::getTempo() { return bpm.load(); }

void TransportEngine::setPosition(double beat) {
    pendingPosition.store(beat);
    hasPendingPosition.store(true);
}

double TransportEngine::getPosition() { return positionSnapshot.load(); }

void TransportEngine::start() { playing.store(true); }
void TransportEngine::stop() { playing.store(false); }
bool TransportEngine::isPlaying() { return playing.load(); }

bool TransportEngine::advanceBlock(int numSamples, double sampleRate) {
    if (hasPendingPosition.exchange(false)) {
        beatPosition = pendingPosition.load();
        lastBeatInt = (int)beatPosition;
    }

    if (!playing.load()) {
        positionSnapshot.store(beatPosition);
        return false;
    }

    double currentBpm = bpm.load();
    double beatsPerSample = currentBpm / (60.0 * sampleRate);
    double beatsThisBlock = beatsPerSample * numSamples;

    double newPos = beatPosition + beatsThisBlock;
    int oldBeatInt = lastBeatInt;
    int newBeatInt = (int)newPos;

    beatPosition = newPos;
    lastBeatInt = newBeatInt;
    positionSnapshot.store(newPos);

    return (newBeatInt != oldBeatInt);
}
