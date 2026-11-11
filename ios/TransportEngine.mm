#include "TransportEngine.h"
#include <algorithm>
#include <cmath>
#include <limits>

TransportEngine::TransportEngine()
    : bpm(120.0), playing(false),
      pendingPosition(std::numeric_limits<double>::quiet_NaN()), positionSnapshot(0.0) {}

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

TransportState TransportEngine::getState() {
    const double pending = pendingPosition.load();
    return { bpm.load(), std::isnan(pending) ? positionSnapshot.load() : pending, playing.load() };
}

void TransportEngine::restoreState(const TransportState& s) {
    setTempo(s.bpm);
    positionSnapshot.store(s.position);
    pendingPosition.store(s.position);
    playing.store(s.playing);
}

bool TransportEngine::advanceBlock(int numSamples, double sampleRate) {
    double pending = pendingPosition.exchange(std::numeric_limits<double>::quiet_NaN());
    blockSeeked = !std::isnan(pending);
    if (blockSeeked) {
        beatPosition = pending;
        lastBeatInt = (int)beatPosition;
    }

    // One load of each per block, shared by the advance below and the block-start info.
    const double currentBpm = bpm.load();
    const bool playingNow = playing.load();
    blockStartBeat = beatPosition;
    blockBpm = currentBpm;
    blockPlaying = playingNow;

    if (!playingNow) {
        positionSnapshot.store(beatPosition);
        return false;
    }

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
