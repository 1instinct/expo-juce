#pragma once

// Pure C++ (no Objective-C): how the engine asks the audio device for its sample rate and buffer
// size. Used by ExpoJuceAudioEngine.mm and the offline harness (case "bsel"), which runs these
// against JUCE's real AudioDeviceManager with a fake device that grants sizes the way iOS does.
//
// Why this exists. AudioDeviceManager::chooseBestBufferSize only accepts a size listed in the
// device's getAvailableBufferSizes(); any other size silently becomes getDefaultBufferSize()
// (256 on iOS). JUCE's iOS device lists the sizes it was *granted* for 64, 128, 256 … frames,
// and asks the session for (size + 1) / rate when it opens, so the size it ends up with is
// usually not one of the listed ones. AudioDeviceManager::restartLastAudioDevice() reopens a new
// device object with the last *granted* size, which is therefore not listed, and the device comes
// back at JUCE's default 256 + 1 frames (the route may round that up further, e.g. to 288 at
// 44.1 kHz) instead of the preferred size. Every (re)open therefore goes through
// applyPreferredSetup, which picks from the current device's own list.

#include "JuceConfig.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <cmath>

namespace devicefmt {

static constexpr int kDefaultBufferSize = 128;
static constexpr double kDefaultSampleRate = 48000.0;

// The listed size nearest to `preferred` (ties: the larger one). An empty list: `preferred`.
inline int nearestListedBufferSize(const juce::Array<int>& sizes, int preferred) {
    if (sizes.isEmpty() || sizes.contains(preferred)) return preferred;
    int best = sizes[0];
    for (int i = 1; i < sizes.size(); ++i) {
        const int candidate = sizes[i];
        const int candidateDistance = std::abs(candidate - preferred);
        const int bestDistance = std::abs(best - preferred);
        if (candidateDistance < bestDistance || (candidateDistance == bestDistance && candidate > best))
            best = candidate;
    }
    return best;
}

// Music rates the engine runs at. A session can report a voice rate (16 / 24 kHz) for a moment,
// e.g. right after a phone call ends; that is not the route's music rate.
static constexpr double kMinDeviceSampleRate = 44100.0;
static constexpr double kMaxDeviceSampleRate = 192000.0;

inline bool isMusicSampleRate(double rate) {
    return std::isfinite(rate) && rate >= kMinDeviceSampleRate && rate <= kMaxDeviceSampleRate;
}

// The rate to run at. The route's current hardware rate when it is a music rate: asking for
// another rate makes iOS resample, and can make it grant a larger IO buffer. Otherwise the hint
// (the app's rate at setup, the rate already in use afterwards) if it is one; else 48 kHz.
inline double chooseDeviceSampleRate(double hardwareRate, double hint) {
    if (isMusicSampleRate(hardwareRate)) return hardwareRate;
    if (isMusicSampleRate(hint)) return hint;
    return kDefaultSampleRate;
}

// Preferred IO buffer duration for `frames` at `rate` (seconds); 0 for nonsense input.
inline double ioBufferDurationSeconds(int frames, double rate) {
    if (frames <= 0 || !std::isfinite(rate) || rate <= 0.0) return 0.0;
    return (double)frames / rate;
}

struct OpenResult {
    juce::String error;          // empty: the device opened with the wanted setup
    juce::String restoreError;   // the wanted setup failed and so did restoring the previous one
    int chosenBufferSize = 0;    // the listed size that was asked for (0: no device)
    bool reopened = false;       // reopenWithPreferredSetup: a new device object was created
};

// Asks the open device for `sampleRate` and the listed buffer size nearest to `preferredBuffer`.
// No-op (empty result) without a current device. On error, if the device is no longer playing,
// restores the setup it had.
inline OpenResult applyPreferredSetup(juce::AudioDeviceManager& manager, int preferredBuffer, double sampleRate) {
    OpenResult result;
    juce::AudioIODevice* device = manager.getCurrentAudioDevice();
    if (device == nullptr) return result;

    result.chosenBufferSize = nearestListedBufferSize(device->getAvailableBufferSizes(), preferredBuffer);
    const juce::AudioDeviceManager::AudioDeviceSetup original = manager.getAudioDeviceSetup();
    juce::AudioDeviceManager::AudioDeviceSetup wanted = original;
    wanted.sampleRate = sampleRate;
    wanted.bufferSize = result.chosenBufferSize;
    result.error = manager.setAudioDeviceSetup(wanted, true);
    if (result.error.isEmpty()) return result;

    juce::AudioIODevice* current = manager.getCurrentAudioDevice();
    if (current == nullptr || !current->isPlaying())
        result.restoreError = manager.setAudioDeviceSetup(original, true);
    return result;
}

// Closes the current device and opens a new one with the preferred setup: JUCE's
// restartLastAudioDevice (which lands on the default buffer size, see the top of this file),
// then applyPreferredSetup on the new device. Needs a device to have been opened before.
inline OpenResult reopenWithPreferredSetup(juce::AudioDeviceManager& manager, int preferredBuffer, double sampleRate) {
    if (manager.getCurrentAudioDevice() != nullptr) manager.closeAudioDevice();
    manager.restartLastAudioDevice();
    if (manager.getCurrentAudioDevice() == nullptr) {
        OpenResult result;
        result.error = "the device did not reopen";
        return result;
    }
    OpenResult result = applyPreferredSetup(manager, preferredBuffer, sampleRate);
    result.reopened = true;
    return result;
}

} // namespace devicefmt
