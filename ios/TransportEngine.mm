#include "TransportEngine.h"
#include <algorithm>
#include <cmath>

TransportEngine::TransportEngine()
    : bpm(120.0), playing(false),
      pendingPosition(std::nan("")), positionSnapshot(0.0) {}

void TransportEngine::setTempo(double newBpm) {
    bpm.store(std::clamp(newBpm, 20.0, 400.0));
}

double TransportEngine::getTempo() { return bpm.load(); }

void TransportEngine::setPosition(double beat) {
    pendingPosition.store(beat);
}

double TransportEngine::getPosition() { return positionSnapshot.load(); }

void TransportEngine::start() { playing.store(true); }
void TransportEngine::stop() { playing.store(false); }
bool TransportEngine::isPlaying() { return playing.load(); }

bool TransportEngine::advanceBlock(int numSamples, double sampleRate) {
    double pending = pendingPosition.exchange(std::nan(""));
    if (!std::isnan(pending)) {
        beatPosition = pending;
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
