// Android audio host (see AndroidEngineHost.h). Mirrors ios/ExpoJuceAudioEngine.mm: the same
// setup steps (numbered as there, so diagnostics read the same on both platforms), the same
// engine lock rules and the same process-lifetime stores. Not here yet (later milestones):
// session-style recovery and rebuilds, audio focus, the beat timer, the aux node (metronome +
// backing track), and the effect / arp / drum / transport / analysis entry points.

#include "JuceConfig.h"
#include "AndroidEngineHost.h"

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include "PolySynthProcessor.h"
#include "FxChainProcessor.h"
#include "AnalysisRing.h"
#include "TransportEngine.h"
#include "Arpeggiator.h"
#include "DrumMachine.h"
#include "SpectrumAnalyser.h"
#include "SourceRing.h"
#include "SourceAnalysis.h"
#include "NoteEventQueue.h"
#include "GraphPlayer.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <thread>

#if defined(__ANDROID__)
#include <android/log.h>
#define HOST_LOG(...) __android_log_print(ANDROID_LOG_INFO, "ExpoJuce", __VA_ARGS__)
#else
#include <cstdio>
#define HOST_LOG(...) (std::fprintf(stderr, "[ExpoJuce] " __VA_ARGS__), std::fputc('\n', stderr))
#endif

namespace expojuce::host {
namespace {

constexpr int kDefaultBufferSize = 128;
constexpr double kDefaultSampleRate = 48000.0;

// Raw pointers, as on iOS: no JUCE smart pointers at file scope (nothing JUCE is destroyed by
// static destruction, after JUCE has shut down).
juce::AudioDeviceManager* sDeviceManager = nullptr;
GraphPlayer* sPlayer = nullptr;
juce::AudioProcessorGraph* sGraph = nullptr;
PolySynthProcessor* sSynth = nullptr; // owned by sGraph
FxChainProcessor* sFx = nullptr;      // owned by sGraph
TransportEngine* sTransport = nullptr;

// ── Engine lifecycle lock ── (the rules of ios/ExpoJuceAudioEngine.mm, "Engine lifecycle lock")
// Lifecycle sections (message thread only) hold it blocking at one level; users hold it for a
// few atomic loads / stores, blocking on the message thread and with a bounded try elsewhere.
std::mutex sEngineLock;
std::atomic<bool> sLifecycleBusy{false};
std::atomic<uint64_t> sLockBusyDrops{0};

// Lifecycle state. Written only on the message thread inside a lifecycle section; read by
// diagnostics on other threads, hence atomics.
std::atomic<bool> sIsReady{false};
std::atomic<bool> sEngineFailed{false};
std::atomic<int> sSetupStep{-1};
std::atomic<int> sRequestedBufferSize{kDefaultBufferSize};
std::atomic<double> sRequestedSampleRate{kDefaultSampleRate};
std::atomic<double> sNativeSampleRate{0.0};

// lastError is a string: guarded by its own small lock (never held while taking another).
std::mutex sErrorLock;
std::string sLastError;
std::string sLastRecoveryError; // also guarded by sErrorLock

// Background: the device is closed while the app is in the background (no stream, no battery
// drain) and reopened with the same setup when it returns. Message thread only.
std::atomic<bool> sBackgrounded{false};
std::atomic<uint64_t> sBackgroundStops{0};
std::atomic<uint64_t> sForegroundRestarts{0};

// ── Process-lifetime stores (as on iOS) ──
// Note input: one queue for the life of the process, so producers never need the engine lock.
NoteEventQueue sNoteQueue;
std::atomic<bool> sAcceptingNotes{false};
FxParamStore sFxParams;
ArpSettingsStore sArpOptions;
DrumParamStore sDrumParams;
AnalysisRing sAnalysis;
// Per-source taps (sourceAnalysisJson): the instrument is written by FxChainProcessor at the end of
// its block. There is no aux node on Android yet, so the backing ring is never written (inactive).
SourceRing sInstrumentRing;
SourceRing sBackingRing;

std::function<std::unique_ptr<juce::AudioIODeviceType>()> sExtraDeviceTypeFactory;

struct LifecycleLock {
    std::lock_guard<std::mutex> guard;
    LifecycleLock() : guard(sEngineLock) { sLifecycleBusy.store(true, std::memory_order_relaxed); }
    ~LifecycleLock() { sLifecycleBusy.store(false, std::memory_order_relaxed); }
    LifecycleLock(const LifecycleLock&) = delete;
    LifecycleLock& operator=(const LifecycleLock&) = delete;
};

bool isMessageThread() {
    return juce::MessageManager::existsAndIsCurrentThread();
}

bool acquireUserLock(std::unique_lock<std::mutex>& lock) {
    if (isMessageThread()) {
        // Lifecycle sections run only on the message thread: busy here means re-entry.
        if (sLifecycleBusy.load(std::memory_order_relaxed)) {
            sLockBusyDrops.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        lock.lock();
        return true;
    }
    if (lock.try_lock()) return true;
    for (int attempt = 0; attempt < 64 && !sLifecycleBusy.load(std::memory_order_relaxed); ++attempt) {
        std::this_thread::yield();
        if (lock.try_lock()) return true;
    }
    sLockBusyDrops.fetch_add(1, std::memory_order_relaxed);
    return false;
}

template <typename Body>
void withSynth(Body&& body) {
    std::unique_lock<std::mutex> lock(sEngineLock, std::defer_lock);
    if (!acquireUserLock(lock)) return;
    if (!sIsReady.load(std::memory_order_acquire) || sSynth == nullptr) return;
    body(*sSynth);
}

std::string lastError() {
    std::lock_guard<std::mutex> guard(sErrorLock);
    return sLastError;
}

std::string lastRecoveryError() {
    std::lock_guard<std::mutex> guard(sErrorLock);
    return sLastRecoveryError;
}

void setLastError(std::string value) {
    std::lock_guard<std::mutex> guard(sErrorLock);
    sLastError = std::move(value);
}

// A problem that does not abort setup: appended, so diagnostics can show it.
void noteNonFatal(const std::string& message) {
    HOST_LOG("%s", message.c_str());
    std::lock_guard<std::mutex> guard(sErrorLock);
    sLastError = sLastError.empty() ? message : sLastError + "; " + message;
}

// The error that aborted setup: placed first, earlier non-fatal notes kept after it.
void noteFatal(const std::string& message) {
    HOST_LOG("%s", message.c_str());
    std::lock_guard<std::mutex> guard(sErrorLock);
    sLastError = sLastError.empty() ? message : message + "; " + sLastError;
}

void setStep(int step, const char* what) {
    sSetupStep.store(step, std::memory_order_relaxed);
    HOST_LOG("Step %d: %s", step, what);
}

// Message thread, lifecycle lock held. Deletes everything doSetup built (any subset).
void teardownEngineLocked() {
    sBackgrounded.store(false, std::memory_order_relaxed);
    sAcceptingNotes.store(false, std::memory_order_release);
    sIsReady.store(false, std::memory_order_release);
    sAnalysis.setActive(false);
    if (sDeviceManager != nullptr && sPlayer != nullptr) sDeviceManager->removeAudioCallback(sPlayer);
    delete sPlayer; sPlayer = nullptr;
    sSynth = nullptr;
    sFx = nullptr;
    delete sGraph; sGraph = nullptr;
    if (sDeviceManager != nullptr) {
        sDeviceManager->closeAudioDevice();
        delete sDeviceManager;
        sDeviceManager = nullptr;
    }
    delete sTransport; sTransport = nullptr;
}

// Nearest size the device lists (ties to the larger), as on iOS. Android: at least two of the
// device's bursts (its smallest listed size), the usual floor for a glitch-free Oboe stream.
int chooseBufferSize(const juce::Array<int>& sizes, int requested) {
    if (sizes.isEmpty()) return requested;
    const int target = std::max(requested, 2 * sizes[0]);
    int best = sizes[0];
    for (int i = 1; i < sizes.size(); ++i) {
        const int candidate = sizes[i];
        const int candidateDistance = std::abs(candidate - target);
        const int bestDistance = std::abs(best - target);
        if (candidateDistance < bestDistance || (candidateDistance == bestDistance && candidate > best))
            best = candidate;
    }
    return best;
}

// Asks the open device for the wanted rate / buffer through JUCE (so JUCE's own restarts keep
// it). Non-fatal: on error the previous setup is restored.
void applyPreferredDeviceSetup(int bufferSize, double sampleRate) {
    juce::AudioIODevice* device = sDeviceManager->getCurrentAudioDevice();
    if (device == nullptr) return;

    const int chosen = chooseBufferSize(device->getAvailableBufferSizes(), bufferSize);
    const auto original = sDeviceManager->getAudioDeviceSetup();
    auto wanted = original;
    wanted.sampleRate = sampleRate;
    wanted.bufferSize = chosen;
    HOST_LOG("Step 4: setAudioDeviceSetup(sr=%.0f, buf=%d; requested %d)", sampleRate, chosen, bufferSize);
    const juce::String error = sDeviceManager->setAudioDeviceSetup(wanted, true);
    if (error.isEmpty()) return;

    noteNonFatal("step 4 (non-fatal): setAudioDeviceSetup(" + std::to_string((int)sampleRate) + " Hz, "
                 + std::to_string(chosen) + "): " + error.toStdString());
    juce::AudioIODevice* current = sDeviceManager->getCurrentAudioDevice();
    if (current == nullptr || !current->isPlaying()) {
        const juce::String restoreError = sDeviceManager->setAudioDeviceSetup(original, true);
        if (restoreError.isNotEmpty())
            noteNonFatal("step 4 (non-fatal): restoring device setup: " + restoreError.toStdString());
    }
}

// Message thread, lifecycle lock held. ios/ExpoJuceAudioEngine.mm -doSetup, steps 0–11.
void doSetupLocked() {
    setStep(0, "initialiseJuce_GUI");
    juce::initialiseJuce_GUI(); // already done by nativeInit on Android; idempotent

    // Step 1 is the AVAudioSession on iOS. Android has no session to configure for output-only
    // playback (no RECORD_AUDIO, nothing to activate); audio focus is a later milestone.
    setStep(1, "audio session (nothing to do on Android)");

    const int preferredBuffer = sRequestedBufferSize.load(std::memory_order_relaxed);
    const double nativeRate = sNativeSampleRate.load(std::memory_order_relaxed);
    const double preferredRate = nativeRate > 0.0 ? nativeRate : sRequestedSampleRate.load(std::memory_order_relaxed);

    setStep(2, "TransportEngine");
    sTransport = new TransportEngine();

    setStep(3, "AudioDeviceManager");
    sDeviceManager = new juce::AudioDeviceManager();
    if (sExtraDeviceTypeFactory) {
        if (auto type = sExtraDeviceTypeFactory()) sDeviceManager->addAudioDeviceType(std::move(type));
    }

    setStep(4, "initialise(0, 2)");
    // No inputs: JUCE asks for RECORD_AUDIO only when an input is opened.
    const juce::String error = sDeviceManager->initialise(0, 2, nullptr, true);
    if (error.isNotEmpty()) {
        noteFatal("AudioDeviceManager::initialise failed: " + error.toStdString());
        teardownEngineLocked();
        return;
    }
    if (sDeviceManager->getCurrentAudioDevice() != nullptr)
        applyPreferredDeviceSetup(preferredBuffer, preferredRate);
    juce::AudioIODevice* device = sDeviceManager->getCurrentAudioDevice();
    if (device == nullptr || !device->isPlaying()) {
        noteFatal("no running audio device after applying/restoring device setup");
        teardownEngineLocked();
        return;
    }

    const double sr = sDeviceManager->getAudioDeviceSetup().sampleRate;
    const int bufSize = sDeviceManager->getAudioDeviceSetup().bufferSize;
    setStep(5, "AudioProcessorGraph");
    HOST_LOG("device %s / %s, sr=%.0f, buf=%d", device->getTypeName().toRawUTF8(), device->getName().toRawUTF8(), sr, bufSize);
    sGraph = new juce::AudioProcessorGraph();
    sGraph->setPlayConfigDetails(0, 2, sr, bufSize);

    setStep(6, "PolySynthProcessor");
    auto synthProcessor = std::make_unique<PolySynthProcessor>();
    sSynth = synthProcessor.get();
    sSynth->setTransport(sTransport);
    // Nothing drains the queue until the player is attached (step 11): drop stale note-ons.
    sNoteQueue.discardPending();
    sSynth->setNoteQueue(&sNoteQueue);
    sSynth->setAnalysis(&sAnalysis);
    sSynth->arp().setSettingsStore(&sArpOptions);
    sSynth->drums().setParamStore(&sDrumParams);

    setStep(7, "addNode (synth, fx chain)");
    auto synthNode = sGraph->addNode(std::move(synthProcessor));
    auto fxProcessor = std::make_unique<FxChainProcessor>();
    sFx = fxProcessor.get();
    sFx->setParamStore(&sFxParams);
    sFx->setTransport(sTransport);
    sFx->setSourceRing(&sInstrumentRing);
    sBackingRing.setSampleRate(sr);
    auto fxNode = sGraph->addNode(std::move(fxProcessor));

    setStep(8, "addNode (output)");
    auto outputNode = sGraph->addNode(std::make_unique<juce::AudioProcessorGraph::AudioGraphIOProcessor>(
        juce::AudioProcessorGraph::AudioGraphIOProcessor::audioOutputNode));

    setStep(9, "connections");
    const bool connectedL = sGraph->addConnection({{synthNode->nodeID, 0}, {fxNode->nodeID, 0}});
    const bool connectedR = sGraph->addConnection({{synthNode->nodeID, 1}, {fxNode->nodeID, 1}});
    if (!connectedL || !connectedR)
        noteNonFatal("step 9 (non-fatal): addConnection synth->fx failed (L=" + std::to_string(connectedL ? 1 : 0)
                     + ", R=" + std::to_string(connectedR ? 1 : 0) + ")");
    const bool fxConnectedL = sGraph->addConnection({{fxNode->nodeID, 0}, {outputNode->nodeID, 0}});
    const bool fxConnectedR = sGraph->addConnection({{fxNode->nodeID, 1}, {outputNode->nodeID, 1}});
    if (!fxConnectedL || !fxConnectedR)
        noteNonFatal("step 9 (non-fatal): addConnection fx->output failed (L=" + std::to_string(fxConnectedL ? 1 : 0)
                     + ", R=" + std::to_string(fxConnectedR ? 1 : 0) + ")");

    setStep(10, "prepareToPlay");
    sGraph->prepareToPlay(sr, bufSize);

    setStep(11, "GraphPlayer + addAudioCallback");
    sPlayer = new GraphPlayer();
    sPlayer->setProcessor(sGraph);
    sPlayer->setAnalysis(&sAnalysis);
    sDeviceManager->addAudioCallback(sPlayer);

    sIsReady.store(true, std::memory_order_release);
    sAnalysis.setActive(true);
    HOST_LOG("DONE: audio stack initialized (sr=%.0f, buf=%d)", sr, bufSize);
}

// ── JSON ──
double finite(double value, double fallback = 0.0) { return std::isfinite(value) ? value : fallback; }
// Float-stored values reported rounded to 6 decimals, as on iOS.
double forJS(float value) { return std::round((double)value * 1e6) / 1e6; }

juce::var number(double value, double fallback = 0.0) { return juce::var(finite(value, fallback)); }

std::string toJson(const juce::var& value) {
    return juce::JSON::toString(value, true).toStdString();
}

juce::var zeros(int count) {
    juce::Array<juce::var> values;
    for (int i = 0; i < count; ++i) values.add(juce::var(0.0));
    return juce::var(values);
}

} // namespace

// ── Lifecycle ──

bool setup(int bufferSize, double sampleRate, double nativeSampleRate) {
    if (!isMessageThread()) {
        noteFatal("setup must run on the main (JUCE message) thread");
        return false;
    }
    if (sIsReady.load(std::memory_order_acquire)) return true;

    sRequestedBufferSize.store(bufferSize > 0 ? std::min(bufferSize, 4096) : kDefaultBufferSize, std::memory_order_relaxed);
    sRequestedSampleRate.store(sampleRate > 0.0 && std::isfinite(sampleRate) ? sampleRate : kDefaultSampleRate,
                               std::memory_order_relaxed);
    sNativeSampleRate.store(nativeSampleRate > 0.0 && std::isfinite(nativeSampleRate) ? nativeSampleRate : 0.0,
                            std::memory_order_relaxed);

    bool ok = false;
    {
        LifecycleLock lifecycle;
        sSetupStep.store(-1, std::memory_order_relaxed);
        setLastError({});
        try {
            doSetupLocked();
        } catch (const std::exception& e) {
            noteFatal(std::string("exception: ") + e.what());
            teardownEngineLocked();
        } catch (...) {
            noteFatal("unknown exception");
            teardownEngineLocked();
        }
        ok = sIsReady.load(std::memory_order_acquire);
        if (!ok && lastError().empty()) noteFatal("setup did not complete");
        if (ok) sAcceptingNotes.store(true, std::memory_order_release);
    }
    sEngineFailed.store(!ok, std::memory_order_relaxed);
    return ok;
}

void shutdown() {
    if (!isMessageThread()) return;
    sEngineFailed.store(false, std::memory_order_relaxed);
    if (!sIsReady.load(std::memory_order_acquire)) return;
    HOST_LOG("Shutting down...");
    LifecycleLock lifecycle;
    sNoteQueue.pushAllNotesOff();
    teardownEngineLocked();
}

std::string setupErrorMessage() {
    const std::string error = lastError();
    return "step " + std::to_string(sSetupStep.load(std::memory_order_relaxed)) + ": "
           + (error.empty() ? std::string("unknown error") : error);
}

void enterBackground() {
    if (!isMessageThread()) return;
    if (!sIsReady.load(std::memory_order_acquire) || sBackgrounded.load(std::memory_order_relaxed)) return;
    LifecycleLock lifecycle;
    if (sDeviceManager == nullptr) return;
    // Held notes are released: the first block after the device reopens applies this mark.
    sAcceptingNotes.store(false, std::memory_order_release);
    sNoteQueue.pushAllNotesOff();
    sDeviceManager->closeAudioDevice(); // stops the callback, closes the stream
    sBackgrounded.store(true, std::memory_order_relaxed);
    sBackgroundStops.fetch_add(1, std::memory_order_relaxed);
    HOST_LOG("background: audio device closed");
}

void enterForeground() {
    if (!isMessageThread()) return;
    if (!sBackgrounded.load(std::memory_order_relaxed)) return;
    LifecycleLock lifecycle;
    if (!sIsReady.load(std::memory_order_acquire) || sDeviceManager == nullptr) {
        sBackgrounded.store(false, std::memory_order_relaxed);
        return;
    }
    // Nothing drains the queue while the device is closed, so this thread may drop note-ons that
    // arrived meanwhile (none are accepted, but a race at the switch could leave one).
    sNoteQueue.discardPending();
    sDeviceManager->restartLastAudioDevice(); // same device, rate and buffer as before
    juce::AudioIODevice* device = sDeviceManager->getCurrentAudioDevice();
    if (device == nullptr || !device->isPlaying()) {
        // Stays backgrounded: the next foreground event tries again.
        const std::string message = "foreground: the audio device did not reopen";
        HOST_LOG("%s", message.c_str());
        std::lock_guard<std::mutex> guard(sErrorLock);
        sLastRecoveryError = message;
        return;
    }
    sBackgrounded.store(false, std::memory_order_relaxed);
    sForegroundRestarts.fetch_add(1, std::memory_order_relaxed);
    sAcceptingNotes.store(true, std::memory_order_release);
    HOST_LOG("foreground: audio device reopened");
}

void setExtraDeviceTypeFactory(std::function<std::unique_ptr<juce::AudioIODeviceType>()> factory) {
    sExtraDeviceTypeFactory = std::move(factory);
}

// ── Diagnostics ──

std::string diagnosticsJson() {
    bool ready = false;
    juce::String deviceName, deviceType;
    double sampleRate = 0.0;
    int bufferSize = 0, outputLatencySamples = 0, xrunCount = -1;
    double callbackCount = 0.0, graphProcessCount = 0.0, outputPeak = 0.0, nanCount = 0.0, toneSamplesMixed = 0.0;
    double synthProcessCount = 0.0, synthPeak = 0.0, lastNoteToRenderMs = -1.0, lastTouchToRenderMs = -1.0;
    int activeVoices = 0;
    double fxProcessCount = 0.0, fxLimiterGain = 1.0, fxNonFiniteResets = 0.0, drumPeak = 0.0;
    {
        std::unique_lock<std::mutex> lock(sEngineLock, std::defer_lock);
        if (acquireUserLock(lock)) {
            ready = sIsReady.load(std::memory_order_acquire);
            if (ready && sDeviceManager != nullptr) {
                if (juce::AudioIODevice* device = sDeviceManager->getCurrentAudioDevice()) {
                    deviceName = device->getName();
                    deviceType = device->getTypeName();
                    sampleRate = device->getCurrentSampleRate();
                    bufferSize = device->getCurrentBufferSizeSamples();
                    if (device->isOpen()) {
                        outputLatencySamples = device->getOutputLatencyInSamples();
                        xrunCount = device->getXRunCount();
                    }
                }
            }
            const auto relaxed = std::memory_order_relaxed;
            if (GraphPlayer* player = ready ? sPlayer : nullptr) {
                callbackCount = (double)player->callbackCount.load(relaxed);
                graphProcessCount = (double)player->graphProcessCount.load(relaxed);
                outputPeak = (double)player->outputPeak.load(relaxed);
                nanCount = (double)player->nanCount.load(relaxed);
                toneSamplesMixed = (double)player->toneSamplesMixed.load(relaxed);
            }
            if (PolySynthProcessor* synth = ready ? sSynth : nullptr) {
                synthProcessCount = (double)synth->diag.processCount.load(relaxed);
                synthPeak = (double)synth->diag.peak.load(relaxed);
                activeVoices = synth->diag.activeVoices.load(relaxed);
                lastNoteToRenderMs = synth->diag.lastNoteToRenderMs.load(relaxed);
                lastTouchToRenderMs = synth->diag.lastTouchToRenderMs.load(relaxed);
                drumPeak = (double)synth->drums().diag.peak.load(relaxed);
            }
            if (FxChainProcessor* fx = ready ? sFx : nullptr) {
                fxProcessCount = (double)fx->diag.processCount.load(relaxed);
                fxLimiterGain = (double)fx->diag.limiterGain.load(relaxed);
                fxNonFiniteResets = (double)fx->diag.nonFiniteResets.load(relaxed);
            }
        }
    }

    // recovering (a lifecycle section is running) > ready > failed > uninitialized.
    const char* engineState = "uninitialized";
    if (sLifecycleBusy.load(std::memory_order_relaxed)) engineState = "recovering";
    else if (ready) engineState = "ready";
    else if (sEngineFailed.load(std::memory_order_relaxed)) engineState = "failed";

    auto* d = new juce::DynamicObject();
    juce::var result(d);
    d->setProperty("platform", "android");
    d->setProperty("isReady", ready);
    d->setProperty("setupStep", sSetupStep.load(std::memory_order_relaxed));
    d->setProperty("lastError", juce::String(lastError()));
    d->setProperty("deviceName", deviceName);
    d->setProperty("deviceType", deviceType);
    d->setProperty("sampleRate", number(sampleRate));
    d->setProperty("bufferSize", bufferSize);
    d->setProperty("outputLatencySamples", outputLatencySamples);
    d->setProperty("nativeSampleRate", number(sNativeSampleRate.load(std::memory_order_relaxed)));
    d->setProperty("xrunCount", xrunCount);

    d->setProperty("callbackCount", number(callbackCount));
    d->setProperty("graphProcessCount", number(graphProcessCount));
    d->setProperty("outputPeak", number(outputPeak));
    d->setProperty("nanCount", number(nanCount));
    d->setProperty("toneSamplesMixed", number(toneSamplesMixed));

    d->setProperty("synthProcessCount", number(synthProcessCount));
    d->setProperty("synthPeak", number(synthPeak));
    d->setProperty("notesQueued", number((double)sNoteQueue.notesQueued.load(std::memory_order_relaxed)));
    d->setProperty("notesConsumed", number((double)sNoteQueue.notesConsumed.load(std::memory_order_relaxed)));
    d->setProperty("notesDropped", number((double)sNoteQueue.notesDropped.load(std::memory_order_relaxed)));
    d->setProperty("activeVoices", activeVoices);
    d->setProperty("lastNoteToRenderMs", number(lastNoteToRenderMs, -1.0));
    d->setProperty("lastTouchToRenderMs", number(lastTouchToRenderMs, -1.0));

    d->setProperty("fxProcessCount", number(fxProcessCount));
    d->setProperty("fxLimiterGain", number(fxLimiterGain, 1.0));
    d->setProperty("fxNonFiniteResets", number(fxNonFiniteResets));
    d->setProperty("metronomeClicks", 0.0);
    d->setProperty("backingTrackLoaded", false);
    d->setProperty("backingTrackPositionSec", -1.0);
    d->setProperty("backingTrackTempoRatio", 1.0);
    d->setProperty("drumHits", number((double)sNoteQueue.drumHitsConsumed.load(std::memory_order_relaxed)));
    d->setProperty("drumPeak", number(drumPeak));

    d->setProperty("engineState", engineState);
    d->setProperty("lockBusyDrops", number((double)sLockBusyDrops.load(std::memory_order_relaxed)));
    d->setProperty("deadDeviceRestarts", 0.0);
    d->setProperty("backgrounded", sBackgrounded.load(std::memory_order_relaxed));
    d->setProperty("backgroundStops", number((double)sBackgroundStops.load(std::memory_order_relaxed)));
    d->setProperty("foregroundRestarts", number((double)sForegroundRestarts.load(std::memory_order_relaxed)));
    d->setProperty("preferredBufferSize", sRequestedBufferSize.load(std::memory_order_relaxed));
    d->setProperty("preferredSampleRate", number(sRequestedSampleRate.load(std::memory_order_relaxed)));

    // AVAudioSession fields: iOS only; present so the JS shape is the same on both platforms.
    d->setProperty("sessionReassertCount", 0);
    d->setProperty("interruptionCount", 0);
    d->setProperty("mediaResetCount", 0);
    d->setProperty("lastRecoveryError", juce::String(lastRecoveryError()));
    d->setProperty("sessionCategory", "");
    d->setProperty("sessionSampleRate", 0.0);
    d->setProperty("sessionIOBufferMs", 0.0);
    d->setProperty("sessionOutputLatencyMs", 0.0);
    d->setProperty("otherAudioPlaying", false);
    d->setProperty("routeOutputs", juce::var(juce::Array<juce::var>()));
    return toJson(result);
}

bool playTestTone(double durationMs) {
    if (!(durationMs > 0.0)) return false;
    std::unique_lock<std::mutex> lock(sEngineLock, std::defer_lock);
    if (!acquireUserLock(lock)) return false;
    if (!sIsReady.load(std::memory_order_acquire) || sPlayer == nullptr || sDeviceManager == nullptr) return false;
    juce::AudioIODevice* device = sDeviceManager->getCurrentAudioDevice();
    if (device == nullptr || !device->isPlaying()) return false;
    sPlayer->requestTestTone(durationMs);
    return true;
}

// ── Synth ──
// Notes go through the process-lifetime queue without the lock; releases are always recorded
// (ready or not) so they reach a voice that is still sounding.

void noteOn(int note, int velocity) {
    if (!sAcceptingNotes.load(std::memory_order_acquire)) return;
    sNoteQueue.pushNoteOn(note, velocity);
}

void noteOff(int note) { sNoteQueue.pushNoteOff(note); }
int lastNoteStarted() { return sNoteQueue.lastNoteStarted.load(std::memory_order_relaxed); }
void allNotesOff() { sNoteQueue.pushAllNotesOff(); }

void setWaveform(const std::string& waveform) {
    Waveform wf = Waveform::Sine;
    if (waveform == "square") wf = Waveform::Square;
    else if (waveform == "saw") wf = Waveform::Saw;
    else if (waveform == "triangle") wf = Waveform::Triangle;
    withSynth([wf](PolySynthProcessor& synth) { synth.setWaveform(wf); });
}

void setFilterCutoff(double hz) { withSynth([hz](PolySynthProcessor& s) { s.setFilterCutoff(hz); }); }
void setFilterResonance(double q) { withSynth([q](PolySynthProcessor& s) { s.setFilterResonance(q); }); }
void setAttack(double ms) { withSynth([ms](PolySynthProcessor& s) { s.setAttack(ms); }); }
void setDecay(double ms) { withSynth([ms](PolySynthProcessor& s) { s.setDecay(ms); }); }
void setSustain(double level) { withSynth([level](PolySynthProcessor& s) { s.setSustain(level); }); }
void setRelease(double ms) { withSynth([ms](PolySynthProcessor& s) { s.setRelease(ms); }); }
void setDetune(double cents) { withSynth([cents](PolySynthProcessor& s) { s.setDetune(cents); }); }
void setMasterLevel(double level) { withSynth([level](PolySynthProcessor& s) { s.setMasterLevel(level); }); }

// ── Stored state ──

std::string effectsStateJson() {
    const FxSettings settings = sFxParams.snapshot();
    auto* state = new juce::DynamicObject();
    juce::var result(state);
    for (int fx = 0; fx < kFxCount; ++fx) {
        const FxSpec& spec = kFxSpecs[fx];
        auto* params = new juce::DynamicObject();
        juce::var paramsVar(params);
        for (int p = 0; p < spec.numParams; ++p)
            params->setProperty(spec.params[p].name, forJS(settings.values[fx][p]));
        auto* effect = new juce::DynamicObject();
        juce::var effectVar(effect);
        effect->setProperty("enabled", settings.enabled[fx]);
        effect->setProperty("params", paramsVar);
        state->setProperty(spec.id, effectVar);
    }
    return toJson(result);
}

std::string arpeggiatorStateJson() {
    const ArpSettings s = sArpOptions.get();
    juce::Array<juce::var> pattern;
    for (int i = 0; i < s.patternLength; ++i) pattern.add(((s.patternMask >> i) & 1) != 0);
    auto* d = new juce::DynamicObject();
    juce::var result(d);
    d->setProperty("enabled", s.enabled);
    d->setProperty("mode", kArpModeNames[std::clamp((int)s.mode, 0, kArpModeCount - 1)]);
    d->setProperty("rate", kArpRateNames[std::clamp((int)s.rate, 0, kArpRateCount - 1)]);
    d->setProperty("octaves", s.octaves);
    d->setProperty("gate", forJS(s.gate));
    d->setProperty("swing", forJS(s.swing));
    d->setProperty("latch", s.latch);
    d->setProperty("pattern", juce::var(pattern));
    d->setProperty("accent", forJS(s.accent));
    d->setProperty("ratchet", s.ratchet);
    d->setProperty("transposeSteps", s.transposeSteps);
    d->setProperty("transposeDistance", s.transposeDistance);
    return toJson(result);
}

std::string drumStateJson() {
    juce::Array<juce::var> voices;
    for (int v = 0; v < kDrumVoices; ++v) {
        const DrumVoiceParams p = sDrumParams.voice(v);
        auto* voice = new juce::DynamicObject();
        voice->setProperty("tune", forJS(p.tune));
        voice->setProperty("decay", forJS(p.decay));
        voice->setProperty("tone", forJS(p.tone));
        voice->setProperty("level", forJS(p.level));
        voices.add(juce::var(voice));
    }
    const DrumKitParams kit = sDrumParams.kit();
    auto* kitObject = new juce::DynamicObject();
    juce::var kitVar(kitObject);
    kitObject->setProperty("level", forJS(kit.level));
    kitObject->setProperty("mute", kit.mute);
    kitObject->setProperty("drive", forJS(kit.drive));
    auto* d = new juce::DynamicObject();
    juce::var result(d);
    d->setProperty("kit", kitVar);
    d->setProperty("voices", juce::var(voices));
    return toJson(result);
}

// Created on first use (FFT + scratch), never freed: nothing JUCE is destroyed by static destruction.
SourceAnalyser& sourceAnalyser(bool instrument) {
    static SourceAnalyser* const instrumentAnalyser = new SourceAnalyser();
    static SourceAnalyser* const backingAnalyser = new SourceAnalyser();
    return instrument ? *instrumentAnalyser : *backingAnalyser;
}

std::string sourceAnalysisJson(const std::string& source, bool bands, const std::string& scope, double scopePoints,
                               double cycleHz, double windowMs) {
    const bool instrument = source == "instrument";
    if (!instrument && source != "backing") return "{}";
    SourceAnalysisOptions o;
    o.bands = bands;
    o.scope = sourceScopeModeFrom(scope.c_str(), o.scope);
    o.scopePoints = SourceAnalysisOptions::pointsFrom(scopePoints);
    o.cycleHz = cycleHz;
    o.windowMs = windowMs;
    SourceRing& ring = instrument ? sInstrumentRing : sBackingRing;
    const bool running = sAnalysis.isActive();
    // No aux node on Android yet: the backing track never sounds.
    const bool active = running && instrument && ring.loudRecently();
    SourceAnalysisResult r;
    sourceAnalyser(instrument).analyse(ring, running, active, sNoteQueue.lastNoteStarted.load(std::memory_order_relaxed), o, r,
                                       sNoteQueue.synthDetuneCents.load(std::memory_order_relaxed));
    juce::Array<juce::var> bandValues, points;
    for (int i = 0; i < SpectrumAnalyser::kBands; ++i) bandValues.add(forJS(r.bands[i]));
    for (int i = 0; i < r.scopeCount; ++i) points.add(forJS(r.scope[i]));
    auto* d = new juce::DynamicObject();
    juce::var result(d);
    d->setProperty("active", r.active);
    d->setProperty("peak", forJS(r.peak));
    d->setProperty("rms", forJS(r.rms));
    d->setProperty("bands", juce::var(bandValues));
    d->setProperty("scope", juce::var(points));
    d->setProperty("scopeMode", sourceScopeModeName(r.scopeMode));
    d->setProperty("scopePeriod", finite(r.scopePeriod));
    d->setProperty("sampleRate", finite(r.sampleRate));
    return toJson(result);
}

std::string analysisJson() {
    auto* arp = new juce::DynamicObject();
    juce::var arpVar(arp);
    arp->setProperty("active", false);
    arp->setProperty("step", 0);
    arp->setProperty("steps", sArpOptions.get().patternLength);
    arp->setProperty("note", -1);
    auto* d = new juce::DynamicObject();
    juce::var result(d);
    d->setProperty("peak", 0.0);
    d->setProperty("rms", 0.0);
    d->setProperty("scope", zeros(AnalysisRing::kScopePoints));
    d->setProperty("bands", zeros(SpectrumAnalyser::kBands));
    d->setProperty("sampleRate", 0.0);
    d->setProperty("lastNote", sNoteQueue.lastNoteStarted.load(std::memory_order_relaxed));
    d->setProperty("bpm", 120.0);
    d->setProperty("beat", 0.0);
    d->setProperty("playing", false);
    d->setProperty("arp", arpVar);
    return toJson(result);
}

} // namespace expojuce::host
