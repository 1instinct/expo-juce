// Offline test of the Android host (android/src/main/cpp/AndroidEngineHost.cpp), its portable
// part: setup / shutdown, device preferences, notes reaching the output, diagnostics and the
// JSON shapes the Kotlin module hands to JS. JUCE's Android device (Oboe) is replaced by a fake
// device type that renders from its own thread in fixed bursts, like an Oboe stream, whatever
// buffer size was asked for. See README.md.
#include "JuceConfig.h"
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include "AndroidEngineHost.h"
#include "FxChainProcessor.h" // kFxCount
#include "DrumMachine.h"     // kDrumVoices

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace host = expojuce::host;

static int gFailures = 0;
static void check(bool ok, const std::string& what) {
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++gFailures;
}

static void pump(int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil(ms); }

// Pumps the message loop until pred() holds or timeoutMs passes.
template <typename Pred>
static bool waitFor(Pred pred, int timeoutMs) {
    for (int waited = 0; waited < timeoutMs; waited += 50) {
        if (pred()) return true;
        pump(50);
    }
    return pred();
}

static juce::var parse(const std::string& json) { return juce::JSON::parse(juce::String(json)); }

//==============================================================================
// Fake Oboe: native rate 44.1 or 48 kHz, bursts of kBurst frames, buffer sizes in whole bursts.
struct FakeStats {
    std::atomic<int> opens{0};
    std::atomic<double> openedRate{0.0};
    std::atomic<int> openedBuffer{0};
    std::atomic<float> peak{0.0f};
    std::atomic<uint64_t> blocks{0};
};
static FakeStats gStats;
static constexpr int kBurst = 96;

class ThreadedFakeDevice final : public juce::AudioIODevice {
public:
    ThreadedFakeDevice() : AudioIODevice("Fake Out", "Fake Oboe") {}
    ~ThreadedFakeDevice() override { close(); }

    juce::StringArray getOutputChannelNames() override { return {"L", "R"}; }
    juce::StringArray getInputChannelNames() override { return {}; }
    juce::Array<double> getAvailableSampleRates() override { return {44100.0, 48000.0}; }
    juce::Array<int> getAvailableBufferSizes() override { return {kBurst, 2 * kBurst, 3 * kBurst, 4 * kBurst, 8 * kBurst}; }
    int getDefaultBufferSize() override { return 4 * kBurst; }

    juce::String open(const juce::BigInteger&, const juce::BigInteger& outputs, double rate, int buffer) override {
        close();
        sampleRate = rate > 0.0 ? rate : 48000.0;
        bufferSize = buffer > 0 ? buffer : getDefaultBufferSize();
        outputChannels = outputs;
        gStats.opens.fetch_add(1);
        gStats.openedRate.store(sampleRate);
        gStats.openedBuffer.store(bufferSize);
        opened = true;
        running = true;
        thread = std::thread([this] { run(); });
        return {};
    }

    void close() override {
        stop();
        running = false;
        if (thread.joinable()) thread.join();
        opened = false;
    }

    bool isOpen() override { return opened; }

    void start(juce::AudioIODeviceCallback* newCallback) override {
        if (newCallback == nullptr) return;
        newCallback->audioDeviceAboutToStart(this);
        std::lock_guard<std::mutex> lock(callbackLock);
        callback = newCallback;
    }

    void stop() override {
        juce::AudioIODeviceCallback* old = nullptr;
        {
            std::lock_guard<std::mutex> lock(callbackLock);
            old = callback;
            callback = nullptr;
        }
        if (old != nullptr) old->audioDeviceStopped();
    }

    bool isPlaying() override { std::lock_guard<std::mutex> lock(callbackLock); return callback != nullptr; }
    juce::String getLastError() override { return {}; }
    int getCurrentBufferSizeSamples() override { return bufferSize; }
    double getCurrentSampleRate() override { return sampleRate; }
    int getCurrentBitDepth() override { return 32; }
    juce::BigInteger getActiveOutputChannels() const override { return outputChannels; }
    juce::BigInteger getActiveInputChannels() const override { return {}; }
    int getOutputLatencyInSamples() override { return bufferSize; }
    int getInputLatencyInSamples() override { return 0; }
    int getXRunCount() const noexcept override { return 0; }

private:
    void run() {
        std::vector<float> left(kBurst), right(kBurst);
        float* channels[2] = {left.data(), right.data()};
        const auto period = std::chrono::microseconds((int64_t)(1e6 * kBurst / sampleRate));
        auto next = std::chrono::steady_clock::now();
        while (running) {
            {
                std::lock_guard<std::mutex> lock(callbackLock);
                if (callback != nullptr) {
                    callback->audioDeviceIOCallbackWithContext(nullptr, 0, channels, 2, kBurst, {});
                    float peak = 0.0f;
                    for (int i = 0; i < kBurst; ++i) peak = std::max(peak, std::max(std::abs(left[(size_t)i]), std::abs(right[(size_t)i])));
                    if (peak > gStats.peak.load()) gStats.peak.store(peak);
                    gStats.blocks.fetch_add(1);
                }
            }
            next += period;
            std::this_thread::sleep_until(next);
        }
    }

    double sampleRate = 48000.0;
    int bufferSize = 4 * kBurst;
    juce::BigInteger outputChannels;
    bool opened = false;
    std::atomic<bool> running{false};
    std::thread thread;
    std::mutex callbackLock;
    juce::AudioIODeviceCallback* callback = nullptr;
};

class FakeDeviceType final : public juce::AudioIODeviceType {
public:
    FakeDeviceType() : AudioIODeviceType("Fake Oboe") {}
    void scanForDevices() override {}
    juce::StringArray getDeviceNames(bool wantInputNames) const override {
        return wantInputNames ? juce::StringArray() : juce::StringArray("Fake Out");
    }
    int getDefaultDeviceIndex(bool) const override { return 0; }
    int getIndexOfDevice(juce::AudioIODevice* device, bool asInput) const override {
        return (device != nullptr && !asInput) ? 0 : -1;
    }
    bool hasSeparateInputsAndOutputs() const override { return true; }
    juce::AudioIODevice* createDevice(const juce::String& outputName, const juce::String&) override {
        return outputName.isEmpty() || outputName == "Fake Out" ? new ThreadedFakeDevice() : nullptr;
    }
};

//==============================================================================
// Every key of the JS AudioDiagnostics type (src/ExpoJuce.types.ts), required and optional,
// except the iOS-only 0.9.0-rc.1 / rc.2 keys (touchLatency, touchPath, lastTouch, mainThread*, midiInput,
// hardwareSampleRate, requested* / granted*, availableBufferSizes, deviceOpens).
static const char* const kDiagnosticsKeys[] = {
    "isReady", "setupStep", "lastError", "deviceName", "deviceType", "sampleRate", "bufferSize",
    "outputLatencySamples", "callbackCount", "graphProcessCount", "synthProcessCount", "outputPeak",
    "synthPeak", "nanCount", "toneSamplesMixed", "notesQueued", "notesConsumed", "notesDropped",
    "activeVoices", "lastNoteToRenderMs", "lastTouchToRenderMs", "preferredBufferSize",
    "preferredSampleRate", "sessionReassertCount", "interruptionCount", "mediaResetCount",
    "lastRecoveryError", "engineState", "lockBusyDrops", "deadDeviceRestarts", "fxProcessCount",
    "fxLimiterGain", "fxNonFiniteResets", "metronomeClicks", "backingTrackLoaded",
    "backingTrackPositionSec", "backingTrackTempoRatio", "drumHits", "drumPeak", "sessionCategory",
    "sessionSampleRate", "sessionIOBufferMs", "sessionOutputLatencyMs", "otherAudioPlaying",
    "routeOutputs", "platform", "nativeSampleRate", "xrunCount", "backgrounded", "backgroundStops",
    "foregroundRestarts",
};

static juce::var diagnostics() { return parse(host::diagnosticsJson()); }

static bool hasEveryKey(const juce::var& d, std::string& missing) {
    for (const char* key : kDiagnosticsKeys)
        if (!d.hasProperty(key)) missing += std::string(missing.empty() ? "" : ", ") + key;
    return missing.empty();
}

//==============================================================================
// android/src/main/java/expo/modules/juce/DefaultShapes.kt: the native JSON before setup, which
// the Kotlin side returns when the library cannot be loaded. Generated from the host itself
// (host_test --write-defaults <path>); the normal run fails if the checked-in file differs.
static std::string defaultShapesKotlin() {
    struct Shape { const char* name; const char* getter; std::string json; };
    const Shape shapes[] = {
        { "EFFECTS", "getEffectsState()", host::effectsStateJson() },
        { "ARPEGGIATOR", "getArpeggiatorState()", host::arpeggiatorStateJson() },
        { "DRUMS", "getDrumState()", host::drumStateJson() },
        { "ANALYSIS", "getAnalysis()", host::analysisJson() },
        { "DIAGNOSTICS", "getDiagnostics() before setup", host::diagnosticsJson() },
    };
    std::string out =
        "// Generated by test/harness/host_test --write-defaults from android/src/main/cpp/AndroidEngineHost.cpp\n"
        "// (its JSON before setup). host_test fails if this file and the native output differ. Do not edit.\n"
        "package expo.modules.juce\n"
        "\n"
        "/** The native JSON shapes, returned when the native library cannot be loaded (ExpoJuceNative). */\n"
        "internal object DefaultShapes {\n";
    for (const Shape& shape : shapes) {
        // Kotlin raw strings: no '$' (templates) and no closing quotes inside.
        if (shape.json.find('$') != std::string::npos || shape.json.find("\"\"\"") != std::string::npos) return {};
        out += std::string("  /** ") + shape.getter + " */\n";
        out += std::string("  const val ") + shape.name + " = \"\"\"" + shape.json + "\"\"\"\n";
    }
    out += "}\n";
    return out;
}

static std::string withLf(const juce::String& text) { return text.replace("\r\n", "\n").toStdString(); }

static void testDefaultShapesFile() {
    std::printf("DefaultShapes.kt\n");
    const std::string expected = defaultShapesKotlin();
    const juce::File file(EXPOJUCE_DEFAULT_SHAPES_KT);
    check(!expected.empty(), "the native JSON fits in Kotlin raw strings");
    check(file.existsAsFile() && withLf(file.loadFileAsString()) == expected,
          "DefaultShapes.kt matches the native shapes (else: host_test --write-defaults android/src/main/java/expo/modules/juce/DefaultShapes.kt)");
}

static int writeDefaultShapes(const char* path) {
    const std::string text = defaultShapesKotlin();
    if (text.empty()) { std::printf("the native JSON does not fit in Kotlin raw strings\n"); return 1; }
    const juce::File file = juce::File::getCurrentWorkingDirectory().getChildFile(path);
    if (!file.replaceWithText(juce::String(text).replace("\n", "\r\n"), false, false, nullptr)) {
        std::printf("cannot write %s\n", file.getFullPathName().toRawUTF8());
        return 1;
    }
    std::printf("wrote %s\n", file.getFullPathName().toRawUTF8());
    return 0;
}

static void testShapesBeforeSetup() {
    std::printf("shapes before setup\n");
    const juce::var d = diagnostics();
    std::string missing;
    check(d.isObject() && hasEveryKey(d, missing), "diagnostics has every AudioDiagnostics key" + (missing.empty() ? "" : " (missing " + missing + ")"));
    check(!(bool)d["isReady"] && (int)d["setupStep"] == -1 && d["engineState"].toString() == "uninitialized",
          "not set up: isReady false, setupStep -1, engineState uninitialized");
    check(d["platform"].toString() == "android" && d["routeOutputs"].isArray(), "platform android, routeOutputs []");

    const juce::var fx = parse(host::effectsStateJson());
    const juce::var reverb = fx["reverb"];
    check(fx.isObject() && fx.getDynamicObject()->getProperties().size() == kFxCount && reverb.isObject()
              && !(bool)reverb["enabled"] && reverb["params"].hasProperty("mix"),
          "getEffectsState: every effect { enabled: false, params }");
    const juce::var arp = parse(host::arpeggiatorStateJson());
    check(!(bool)arp["enabled"] && arp["mode"].toString() == "up" && arp["rate"].toString() == "1/16"
              && arp["pattern"].isArray() && arp["pattern"].size() == 16,
          "getArpeggiatorState: defaults (up, 1/16, 16-step pattern)");
    const juce::var drums = parse(host::drumStateJson());
    check(drums["kit"].isObject() && drums["voices"].isArray() && drums["voices"].size() == kDrumVoices
              && drums["voices"][0].hasProperty("tune"),
          "getDrumState: { kit, voices[9] }");
    const juce::var analysis = parse(host::analysisJson());
    check(analysis["scope"].size() == 128 && analysis["bands"].size() == 24 && (int)analysis["lastNote"] == -1
              && (int)analysis["arp"]["steps"] == 16 && (double)analysis["bpm"] == 120.0,
          "getAnalysis: inactive shape (128 scope, 24 bands, arp steps 16)");

    // Note input before setup is dropped (note-ons) or harmless (releases).
    host::noteOn(60, 100);
    host::noteOff(60);
    host::allNotesOff();
    host::setWaveform("saw");
    host::setFilterCutoff(1200.0);
    check((double)diagnostics()["notesQueued"] == 0.0, "noteOn before setup is not queued");
    check(!host::playTestTone(100.0), "playTestTone before setup: false");
    host::enterBackground();
    host::enterForeground();
    const juce::var after = diagnostics();
    check((double)after["backgroundStops"] == 0.0 && (double)after["foregroundRestarts"] == 0.0 && !(bool)after["backgrounded"],
          "background / foreground before setup: no-ops");
}

static void testNoDevice() {
    std::printf("setup without an audio device\n");
    host::setExtraDeviceTypeFactory(nullptr);
    check(!host::setup(128, 48000.0, 0.0), "setup fails");
    const std::string message = host::setupErrorMessage();
    check(message.rfind("step 4: ", 0) == 0, "setupErrorMessage names step 4: \"" + message + "\"");
    const juce::var d = diagnostics();
    check(d["engineState"].toString() == "failed" && !(bool)d["isReady"], "engineState failed");
    host::shutdown();
    check(diagnostics()["engineState"].toString() == "uninitialized", "shutdown after a failed setup: uninitialized");
}

static void testSetupOffMessageThread() {
    std::printf("setup off the message thread\n");
    bool ok = true;
    std::thread([&] { ok = host::setup(128, 48000.0, 48000.0); }).join();
    check(!ok, "setup from another thread fails (Kotlin runs it on the main thread)");
}

static void testPlayback(double nativeRate, int expectedBuffer) {
    std::printf("setup with the fake device, native rate %.0f\n", nativeRate);
    host::setExtraDeviceTypeFactory([] { return std::make_unique<FakeDeviceType>(); });
    const bool ok = host::setup(128, 48000.0, nativeRate);
    check(ok, ok ? "setup succeeds" : "setup succeeds (" + host::setupErrorMessage() + ")");
    if (!ok) return;
    check(host::setup(128, 48000.0, nativeRate), "setup again: true (already set up)");

    juce::var d = diagnostics();
    const double expectedRate = nativeRate > 0.0 ? nativeRate : 48000.0;
    check((bool)d["isReady"] && (int)d["setupStep"] == 11 && d["engineState"].toString() == "ready",
          "ready, setupStep 11, engineState ready");
    check((double)d["sampleRate"] == expectedRate && gStats.openedRate.load() == expectedRate,
          "device runs at " + juce::String(expectedRate).toStdString() + " Hz (native preferred); got "
              + d["sampleRate"].toString().toStdString());
    check((int)d["bufferSize"] == expectedBuffer,
          "buffer: 128 asked, two bursts minimum -> " + std::to_string(expectedBuffer) + "; got " + d["bufferSize"].toString().toStdString());
    check(d["deviceType"].toString() == "Fake Oboe" && d["deviceName"].toString() == "Fake Out", "device type / name reported");
    check((int)d["preferredBufferSize"] == 128 && (double)d["preferredSampleRate"] == 48000.0
              && (double)d["nativeSampleRate"] == nativeRate,
          "preferredBufferSize / preferredSampleRate / nativeSampleRate reported");
    check(d["lastError"].toString().isEmpty(), "lastError empty: \"" + d["lastError"].toString().toStdString() + "\"");

    const double queuedBefore = (double)d["notesQueued"];
    gStats.peak.store(0.0f);
    host::setWaveform("saw");
    host::setAttack(1.0);
    host::setRelease(50.0);
    host::noteOn(60, 127);
    pump(400);
    d = diagnostics();
    const double callbacks1 = (double)d["callbackCount"];
    check((double)d["notesQueued"] == queuedBefore + 1 && (double)d["notesConsumed"] >= 1, "noteOn queued and consumed");
    check(gStats.peak.load() > 0.05f && (double)d["outputPeak"] > 0.01,
          "held note reaches the device output (peak " + std::to_string(gStats.peak.load()) + ")");
    check((int)d["activeVoices"] >= 1 && (double)d["synthProcessCount"] > 0 && (double)d["fxProcessCount"] > 0,
          "synth and effects chain render");
    pump(100);
    check((double)diagnostics()["callbackCount"] > callbacks1, "callbackCount advances");

    host::noteOff(60);
    check(waitFor([] { return (int)diagnostics()["activeVoices"] == 0; }, 3000), "noteOff releases the voice");
    host::noteOn(64, 100);
    host::noteOn(67, 100);
    pump(100);
    host::allNotesOff();
    check(waitFor([] { return (int)diagnostics()["activeVoices"] == 0; }, 3000), "allNotesOff releases every voice");

    const double toneBefore = (double)diagnostics()["toneSamplesMixed"];
    check(host::playTestTone(50.0), "playTestTone: true while playing");
    pump(150);
    check((double)diagnostics()["toneSamplesMixed"] > toneBefore, "test tone mixed into the output");

    // Notes from another thread while the message thread is idle.
    std::thread([] { host::noteOn(72, 90); }).join();
    pump(200);
    std::thread([] { host::noteOff(72); }).join();
    check(waitFor([] { return (int)diagnostics()["activeVoices"] == 0; }, 3000), "note on / off from another thread");

    // Background: the device closes, held notes are released; foreground: it reopens as it was.
    d = diagnostics();
    const double stops = (double)d["backgroundStops"], restarts = (double)d["foregroundRestarts"];
    const int opens = gStats.opens.load();
    host::noteOn(60, 127);
    pump(200);
    host::enterBackground();
    host::enterBackground(); // idempotent
    d = diagnostics();
    check((bool)d["backgrounded"] && (double)d["backgroundStops"] == stops + 1 && (bool)d["isReady"],
          "background: device closed once (backgroundStops +1), engine still set up");
    const uint64_t blocks = gStats.blocks.load();
    pump(200);
    check(gStats.blocks.load() == blocks, "background: nothing renders");
    check(!host::playTestTone(50.0), "background: playTestTone false");
    const double queuedInBackground = (double)d["notesQueued"];
    host::noteOn(62, 100);
    check((double)diagnostics()["notesQueued"] == queuedInBackground, "background: noteOn is not queued");
    host::enterForeground();
    host::enterForeground(); // idempotent
    d = diagnostics();
    check(!(bool)d["backgrounded"] && (double)d["foregroundRestarts"] == restarts + 1 && gStats.opens.load() == opens + 1,
          "foreground: device reopened once (foregroundRestarts +1)");
    check(gStats.openedRate.load() == expectedRate && gStats.openedBuffer.load() == expectedBuffer
              && (double)d["sampleRate"] == expectedRate && (int)d["bufferSize"] == expectedBuffer,
          "foreground: same sample rate and buffer as before");
    check(waitFor([] { return (int)diagnostics()["activeVoices"] == 0; }, 3000),
          "the note held when going to background is released");
    gStats.peak.store(0.0f);
    host::noteOn(65, 127);
    pump(300);
    check(gStats.peak.load() > 0.05f, "foreground: a new note reaches the output (peak " + std::to_string(gStats.peak.load()) + ")");
    host::noteOff(65);
    check(waitFor([] { return (int)diagnostics()["activeVoices"] == 0; }, 3000), "foreground: noteOff releases it");
    // Shutdown while in the background is clean too.
    host::enterBackground();

    host::shutdown();
    d = diagnostics();
    check(!(bool)d["isReady"] && d["engineState"].toString() == "uninitialized" && (double)d["callbackCount"] == 0.0
          && !(bool)d["backgrounded"],
          "shutdown (from the background): not ready, uninitialized, not backgrounded");
    const double queued = (double)d["notesQueued"];
    host::noteOn(60, 100);
    check((double)diagnostics()["notesQueued"] == queued, "noteOn after shutdown is not queued");
    host::shutdown();
    check(true, "second shutdown is a no-op");
}

int main(int argc, char** argv) {
    juce::MessageManager::getInstance();
    juce::initialiseJuce_GUI();

    if (argc == 3 && std::string(argv[1]) == "--write-defaults") {
        const int rc = writeDefaultShapes(argv[2]);
        juce::shutdownJuce_GUI();
        return rc;
    }

    testDefaultShapesFile(); // first: the shapes before anything has run
    testShapesBeforeSetup();
    testNoDevice();
    testSetupOffMessageThread();
    testPlayback(44100.0, 2 * kBurst);
    // Setup again after a shutdown (a new engine), at another native rate; unknown native rate
    // falls back to the requested 48 kHz.
    testPlayback(48000.0, 2 * kBurst);
    testPlayback(0.0, 2 * kBurst);
    host::setExtraDeviceTypeFactory(nullptr);

    std::printf("%d failure(s)\n", gFailures);
    juce::shutdownJuce_GUI();
    return gFailures == 0 ? 0 : 1;
}
