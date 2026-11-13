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
#include <algorithm>
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

// The rate to run at. The session's current hardware rate when it is a music rate (the route's
// own rate unless a preferred rate is in effect): asking for another rate makes iOS resample, and
// can make it grant a larger IO buffer. Otherwise the hint (the app's rate at setup, the rate
// already in use afterwards) if it is one; else 48 kHz.
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
    juce::String error;          // empty: the device runs with the wanted setup
    juce::String restoreError;   // the wanted setup failed and so did restoring the previous one
    int chosenBufferSize = 0;    // the listed size that was asked for (0: no device)
    bool opened = false;         // the device was stopped and opened again (its xrun count restarted)
    bool reopened = false;       // reopenWithPreferredSetup: a new device object was created
    int targetBufferSize = 0;    // opened: the size passed to the device's open(), which JUCE's iOS
                                 // device keeps asking for in its own route-change restarts (0: unknown)
    int xrunsBeforeOpen = 0;     // xruns the device(s) had counted before being opened again
};

// The size AudioDeviceManager::chooseBestBufferSize passes to the device's open() for `size`.
inline int openSizeFor(juce::AudioIODevice& device, int size) {
    return size > 0 && device.getAvailableBufferSizes().contains(size) ? size : device.getDefaultBufferSize();
}

// Whether the running device still has the setup AudioDeviceManager last stored for it.
inline bool deviceMatchesSetup(juce::AudioIODevice& device, const juce::AudioDeviceManager::AudioDeviceSetup& setup) {
    return device.isPlaying() && device.getCurrentBufferSizeSamples() == setup.bufferSize
        && std::abs(device.getCurrentSampleRate() - setup.sampleRate) <= 0.5;
}

// After a route change: whether the device should be asked for the preferred size again. JUCE's
// iOS device restarts itself on a route change with the size it was opened with (`openedSize`),
// so it comes back right by itself while that size is still the nearest listed one. When the new
// route lists other sizes (e.g. back on the speaker after a Bluetooth route that was opened at
// 1024), it does not. The granted size cannot be compared instead: with JUCE's extra frame it is
// rarely a listed size, and the size granted before the route change says nothing about this one.
inline bool bufferChoiceDrifted(const juce::Array<int>& listed, int preferred, int openedSize) {
    return nearestListedBufferSize(listed, preferred) != openedSize;
}

inline OpenResult reopenWithPreferredSetup(juce::AudioDeviceManager& manager, int preferredBuffer, double sampleRate);

namespace detail {
inline OpenResult applyPreferredSetup(juce::AudioDeviceManager& manager, int preferredBuffer, double sampleRate,
                                      bool reopenIfStale) {
    OpenResult result;
    juce::AudioIODevice* device = manager.getCurrentAudioDevice();
    if (device == nullptr) return result;

    result.chosenBufferSize = nearestListedBufferSize(device->getAvailableBufferSizes(), preferredBuffer);
    const juce::AudioDeviceManager::AudioDeviceSetup original = manager.getAudioDeviceSetup();
    juce::AudioDeviceManager::AudioDeviceSetup wanted = original;
    wanted.sampleRate = sampleRate;
    wanted.bufferSize = result.chosenBufferSize;
    if (wanted == original) {
        // setAudioDeviceSetup would return without touching the device. Right when the device is
        // still running with that setup; if it is not (the stored setup is stale), open a new one.
        if (!reopenIfStale || deviceMatchesSetup(*device, original)) return result;
        return devicefmt::reopenWithPreferredSetup(manager, preferredBuffer, sampleRate);
    }

    result.opened = true;
    result.targetBufferSize = openSizeFor(*device, result.chosenBufferSize);
    result.xrunsBeforeOpen = std::max(0, manager.getXRunCount());
    result.error = manager.setAudioDeviceSetup(wanted, true);
    if (result.error.isEmpty()) return result;

    juce::AudioIODevice* current = manager.getCurrentAudioDevice();
    if (current == nullptr || !current->isPlaying()) {
        result.restoreError = manager.setAudioDeviceSetup(original, true);
        juce::AudioIODevice* restored = manager.getCurrentAudioDevice();
        result.targetBufferSize = result.restoreError.isEmpty() && restored != nullptr
            ? openSizeFor(*restored, original.bufferSize) : 0;
    }
    return result;
}
} // namespace detail

// Asks the open device for `sampleRate` and the listed buffer size nearest to `preferredBuffer`.
// No-op (empty result) without a current device, and when the device already runs with exactly
// that setup. If AudioDeviceManager's stored setup already equals the wanted one but the device
// no longer runs with it, opens a new device (reopenWithPreferredSetup). On error, if the device
// is no longer playing, restores the setup it had.
inline OpenResult applyPreferredSetup(juce::AudioDeviceManager& manager, int preferredBuffer, double sampleRate) {
    return detail::applyPreferredSetup(manager, preferredBuffer, sampleRate, true);
}

// Closes the current device and opens a new one with the preferred setup: JUCE's
// restartLastAudioDevice (which lands on the default buffer size, see the top of this file),
// then applyPreferredSetup on the new device. Needs a device to have been opened before.
inline OpenResult reopenWithPreferredSetup(juce::AudioDeviceManager& manager, int preferredBuffer, double sampleRate) {
    const int lastSize = manager.getAudioDeviceSetup().bufferSize;
    int xruns = 0;
    if (manager.getCurrentAudioDevice() != nullptr) {
        xruns = std::max(0, manager.getXRunCount());
        manager.closeAudioDevice();
    }
    manager.restartLastAudioDevice();
    juce::AudioIODevice* device = manager.getCurrentAudioDevice();
    OpenResult result;
    if (device == nullptr) {
        result.error = "the device did not reopen";
    } else {
        const int restartSize = openSizeFor(*device, lastSize);
        result = detail::applyPreferredSetup(manager, preferredBuffer, sampleRate, false);
        if (!result.opened) result.targetBufferSize = restartSize;
    }
    result.opened = true;
    result.reopened = true;
    result.xrunsBeforeOpen += xruns;
    return result;
}

} // namespace devicefmt
