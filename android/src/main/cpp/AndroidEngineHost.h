#pragma once

// The Android audio host: the portable C++ counterpart of ios/ExpoJuceAudioEngine.mm.
//
// It builds the same engine as the iOS doSetup steps 2–11 (AudioDeviceManager with 0 inputs and
// 2 outputs, the graph synth → effects chain → output played by GraphPlayer) on JUCE's Android
// device (Oboe), around the same process-lifetime stores (note queue, effect / arp / drum
// parameters, analysis ring). No JNI and no Android headers here: ExpoJuceJni.cpp is the JNI
// layer, and the offline harness (test/harness/host_test.cpp) compiles this file on Linux.
//
// Threads:
//  - setup / shutdown run on JUCE's message thread, which on Android is the main thread
//    (ExpoJuceJni.cpp creates the MessageManager there). Called elsewhere they fail / do nothing.
//  - Everything else is safe from any thread (the JS thread in practice). Note input goes through
//    a lock-free queue; other calls hold the engine lock briefly and are dropped (safe default)
//    while a lifecycle section holds it, exactly as on iOS.
//  - The audio thread never takes a lock.

#include <functional>
#include <memory>
#include <string>

namespace juce { class AudioIODeviceType; }

namespace expojuce::host {

// ── Lifecycle (message thread) ───────────────────────────────────
// Builds the engine. bufferSize / sampleRate are the app's preferences (<= 0: 128 / 48000).
// nativeSampleRate is the device's own output rate (Android AudioManager
// PROPERTY_OUTPUT_SAMPLE_RATE; 0 = unknown); when known it is used instead of sampleRate, so the
// output stream runs at the hardware rate without a resampling stage. Returns true on success or
// if already set up; on failure setupErrorMessage() says which step failed and why.
bool setup(int bufferSize, double sampleRate, double nativeSampleRate);
// Tears the engine down (releases every sounding note first). No-op when not set up.
void shutdown();
// "step N: <error>" for the last setup failure.
std::string setupErrorMessage();
// App to background: releases held notes and closes the audio device (the stream stops, so
// nothing renders or holds the output while the app is not visible). No-op if not set up or
// already in the background.
void enterBackground();
// App back in the foreground: reopens the device with the setup it had. No-op unless
// enterBackground closed it; if the device does not reopen, it stays closed (lastRecoveryError)
// and the next call tries again.
void enterForeground();

// ── Diagnostics (any thread) ─────────────────────────────────────
// JSON object with every key of the JS AudioDiagnostics type (iOS-only session fields are empty
// or zero), plus the Android fields platform, nativeSampleRate, xrunCount, backgrounded,
// backgroundStops and foregroundRestarts.
std::string diagnosticsJson();
// Mixes a 440 Hz tone into the device output. false if not ready or the device is not playing.
bool playTestTone(double durationMs);

// ── Synth (any thread) ───────────────────────────────────────────
void noteOn(int note, int velocity);
void noteOff(int note);
void allNotesOff();
// MIDI note of the last note-on the synth started, -1 if none (getAnalysis().lastNote).
int lastNoteStarted();
void setWaveform(const std::string& waveform); // "sine" | "square" | "saw" | "triangle"
void setFilterCutoff(double hz);
void setFilterResonance(double q);
void setAttack(double ms);
void setDecay(double ms);
void setSustain(double level);
void setRelease(double ms);
void setDetune(double cents);
void setMasterLevel(double level);

// ── Stored state (any thread) ────────────────────────────────────
// The same JSON shapes the iOS module returns for getEffectsState, getArpeggiatorState and
// getDrumState (read from the process-lifetime stores), and getAnalysis while nothing is being
// analysed.
std::string effectsStateJson();
std::string arpeggiatorStateJson();
std::string drumStateJson();
std::string analysisJson();

// ── Test seam ────────────────────────────────────────────────────
// When set, every AudioDeviceManager the host creates gets this device type added before it is
// initialised (the offline harness drives the host with a fake device this way). Message thread.
void setExtraDeviceTypeFactory(std::function<std::unique_ptr<juce::AudioIODeviceType>()> factory);

} // namespace expojuce::host
