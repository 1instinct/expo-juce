// Offline reproduction of the iOS native audio path (ios/BeeperAudioEngine.mm doSetup +
// AudioDeviceManager::addAudioCallback + the device's audio thread), driven by a fake
// AudioIODevice. See README.md and docs/superpowers/specs/2026-10-21-audio-silence-diagnostics.md.
#include "JuceConfig.h"
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "PolySynthProcessor.h"
#include "TransportEngine.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>
#include <vector>

//==============================================================================
// GraphPlayer — copied VERBATIM from ios/BeeperAudioEngine.mm at commit 69c2252
// (`git show 69c2252:ios/BeeperAudioEngine.mm`), before the diagnostics edits. Keep in sync.
class GraphPlayer : public juce::AudioIODeviceCallback {
public:
    void setProcessor(juce::AudioProcessor* p) { processor = p; }

    void audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                          int numInputChannels,
                                          float* const* outputChannelData,
                                          int numOutputChannels,
                                          int numSamples,
                                          const juce::AudioIODeviceCallbackContext& context) override {
        juce::ignoreUnused(inputChannelData, numInputChannels, context);
        if (!processor) {
            for (int i = 0; i < numOutputChannels; ++i)
                juce::FloatVectorOperations::clear(outputChannelData[i], numSamples);
            return;
        }

        buffer.setSize(numOutputChannels, numSamples, false, false, true);
        buffer.clear();
        juce::MidiBuffer midi;
        processor->processBlock(buffer, midi);
        for (int i = 0; i < numOutputChannels; ++i)
            juce::FloatVectorOperations::copy(outputChannelData[i], buffer.getReadPointer(i), numSamples);
    }

    void audioDeviceAboutToStart(juce::AudioIODevice* device) override {
        if (processor) {
            processor->setPlayConfigDetails(0, device->getActiveOutputChannels().countNumberOfSetBits(),
                                            device->getCurrentSampleRate(),
                                            device->getCurrentBufferSizeSamples());
            processor->prepareToPlay(device->getCurrentSampleRate(),
                                     device->getCurrentBufferSizeSamples());
        }
    }

    void audioDeviceStopped() override {
        if (processor)
            processor->releaseResources();
    }

private:
    juce::AudioProcessor* processor = nullptr;
    juce::AudioBuffer<float> buffer;
};
// ── end of verbatim copy ──

//==============================================================================
// Harness-only probe: identical behaviour to PolySynthProcessor, plus counters so we can
// tell "graph never called the synth" apart from "synth rendered but graph dropped it".
class ProbeSynth : public PolySynthProcessor {
public:
    std::atomic<int> calls{0};
    std::atomic<float> peak{0.0f};
    void processBlock(juce::AudioBuffer<float>& b, juce::MidiBuffer& m) override {
        PolySynthProcessor::processBlock(b, m);
        calls.fetch_add(1);
        float p = b.getMagnitude(0, b.getNumSamples());
        if (p > peak.load()) peak.store(p);
    }
};

//==============================================================================
// Fake device: only reports the state GraphPlayer::audioDeviceAboutToStart reads.
class FakeDevice : public juce::AudioIODevice {
public:
    double sr; int buf;
    FakeDevice(double s, int b) : AudioIODevice("Fake", "Fake"), sr(s), buf(b) {}
    juce::StringArray getOutputChannelNames() override { return {"L", "R"}; }
    juce::StringArray getInputChannelNames() override { return {}; }
    juce::Array<double> getAvailableSampleRates() override { return {sr}; }
    juce::Array<int> getAvailableBufferSizes() override { return {buf}; }
    int getDefaultBufferSize() override { return buf; }
    juce::String open(const juce::BigInteger&, const juce::BigInteger&, double, int) override { return {}; }
    void close() override {}
    bool isOpen() override { return true; }
    void start(juce::AudioIODeviceCallback*) override {}
    void stop() override {}
    bool isPlaying() override { return true; }
    juce::String getLastError() override { return {}; }
    int getCurrentBufferSizeSamples() override { return buf; }
    double getCurrentSampleRate() override { return sr; }
    int getCurrentBitDepth() override { return 32; }
    juce::BigInteger getActiveOutputChannels() const override { juce::BigInteger b; b.setRange(0, 2, true); return b; }
    juce::BigInteger getActiveInputChannels() const override { return {}; }
    int getOutputLatencyInSamples() override { return 0; }
    int getInputLatencyInSamples() override { return 0; }
};

//==============================================================================
struct Case {
    std::string id, desc;
    double setupSr; int setupBuf;      // what doSetup reads from AudioDeviceManager
    double devSr;   int devBuf;        // what the device reports at audioDeviceAboutToStart
    std::vector<int> blocks;           // block sizes the device thread actually delivers (cycled)
    bool messageLoop = true;           // main thread pumps the JUCE message queue
    bool aboutToStartOffThread = false;
    bool restartMidNote = false;       // simulate iOS route-change restart (stopped + aboutToStart)
    bool expectSilence = false;        // negative control: constructed to go silent
};

struct BlockRec { int phase; int n; float peak; int nans; bool zero; };

struct Result {
    float peakNote = 0; int nans = 0; int noteBlocks = 0, zeroNoteBlocks = 0;
    int firstSoundBlock = -1; int synthCalls = 0; float synthPeak = 0;
    bool pass = false;
};

static void pump(bool loop, int ms) {
    if (loop) juce::MessageManager::getInstance()->runDispatchLoopUntil(ms);
    else std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

static Result runCase(const Case& c) {
    // ── doSetup replica (ios/BeeperAudioEngine.mm, HEAD), steps 2 and 5–11 ──
    auto* transport = new TransportEngine();
    auto* graph = new juce::AudioProcessorGraph();
    graph->setPlayConfigDetails(0, 2, c.setupSr, c.setupBuf);
    auto synthProcessor = std::make_unique<ProbeSynth>();
    ProbeSynth* synth = synthProcessor.get();
    synth->setTransport(transport);
    auto synthNode = graph->addNode(std::move(synthProcessor));
    auto outputNode = graph->addNode(std::make_unique<juce::AudioProcessorGraph::AudioGraphIOProcessor>(
        juce::AudioProcessorGraph::AudioGraphIOProcessor::audioOutputNode));
    graph->addConnection({{synthNode->nodeID, 0}, {outputNode->nodeID, 0}});
    graph->addConnection({{synthNode->nodeID, 1}, {outputNode->nodeID, 1}});
    graph->prepareToPlay(c.setupSr, c.setupBuf);
    auto* player = new GraphPlayer();
    player->setProcessor(graph);

    FakeDevice device(c.devSr, c.devBuf);

    // ── device thread: already running when addAudioCallback is called (initialise() opened
    //    and started the device). Until the callback is attached, AudioDeviceManager writes zeros. ──
    std::atomic<bool> attached{false}, paused{false}, stop{false};
    std::atomic<int> phase{0};
    std::vector<BlockRec> recs; recs.reserve(20000);
    std::thread audio([&] {
        juce::AudioBuffer<float> out(2, 8192);
        juce::AudioIODeviceCallbackContext ctx{};
        auto next = std::chrono::steady_clock::now();
        size_t bi = 0;
        while (!stop.load()) {
            int n = c.blocks[bi++ % c.blocks.size()];
            next += std::chrono::microseconds((long long)(1e6 * n / c.devSr));
            std::this_thread::sleep_until(next);
            if (paused.load()) continue;   // audio unit disposed during restart: no callbacks
            out.clear();
            if (attached.load())
                player->audioDeviceIOCallbackWithContext(nullptr, 0, out.getArrayOfWritePointers(), 2, n, ctx);
            BlockRec r{phase.load(), n, 0.0f, 0, true};
            for (int ch = 0; ch < 2; ++ch) {
                const float* d = out.getReadPointer(ch);
                for (int i = 0; i < n; ++i) {
                    if (!std::isfinite(d[i])) { r.nans++; continue; }
                    r.peak = std::max(r.peak, std::fabs(d[i]));
                    if (d[i] != 0.0f) r.zero = false;
                }
            }
            recs.push_back(r);
        }
    });

    // ── addAudioCallback replica: aboutToStart is called on the CALLING thread, then attached ──
    pump(c.messageLoop, 20);
    if (c.aboutToStartOffThread) {
        std::thread t([&] { player->audioDeviceAboutToStart(&device); });
        t.join();
    } else {
        player->audioDeviceAboutToStart(&device);
    }
    attached.store(true);

    pump(c.messageLoop, 50);
    phase.store(1);
    synth->noteOn(60, 127);                // what ExpoJuce.noteOn → sSynth->noteOn does
    if (c.restartMidNote) {
        pump(c.messageLoop, 250);
        // juce_Audio_ios.cpp restart(): dispose unit → callback->audioDeviceStopped(),
        // createAudioUnit, callback->audioDeviceAboutToStart(), AudioOutputUnitStart. Main thread.
        paused.store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        player->audioDeviceStopped();
        player->audioDeviceAboutToStart(&device);
        paused.store(false);
        pump(c.messageLoop, 250);
    } else {
        pump(c.messageLoop, 500);
    }
    phase.store(2);
    synth->noteOff(60);
    pump(c.messageLoop, 300);
    stop.store(true);
    audio.join();

    Result res;
    int idx = 0;
    for (auto& r : recs) {
        res.nans += r.nans;
        if (r.phase == 1) {
            res.noteBlocks++;
            if (r.zero) res.zeroNoteBlocks++;
            else if (res.firstSoundBlock < 0) res.firstSoundBlock = idx;
            res.peakNote = std::max(res.peakNote, r.peak);
            idx++;
        }
    }
    res.synthCalls = synth->calls.load();
    res.synthPeak = synth->peak.load();
    // A held note must sound in every block after the first (allow 1 block for event latency).
    res.pass = res.peakNote > 1e-3f && res.nans == 0 && res.zeroNoteBlocks <= 1;

    delete player;
    delete graph;      // owns synth
    delete transport;
    return res;
}

// Control: the synth alone, no graph, no threads.
static Result runSynthAlone() {
    TransportEngine transport;
    PolySynthProcessor synth;
    synth.setTransport(&transport);
    synth.setRateAndBufferSizeDetails(48000, 512);
    synth.prepareToPlay(48000, 512);
    juce::AudioBuffer<float> buf(2, 512);
    juce::MidiBuffer midi;
    Result res;
    synth.noteOn(60, 127);
    int nBlocks = (int)(0.5 * 48000 / 512);
    for (int b = 0; b < nBlocks; ++b) {
        synth.processBlock(buf, midi);
        res.noteBlocks++;
        float p = buf.getMagnitude(0, 512);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 512; ++i)
                if (!std::isfinite(buf.getSample(ch, i))) res.nans++;
        if (p == 0.0f) res.zeroNoteBlocks++;
        else if (res.firstSoundBlock < 0) res.firstSoundBlock = b;
        res.peakNote = std::max(res.peakNote, p);
        res.synthCalls++;
    }
    res.synthPeak = res.peakNote;
    res.pass = res.peakNote > 1e-3f && res.nans == 0 && res.zeroNoteBlocks == 0;
    return res;
}

int main() {
    // App: MessageManager::getInstance() in a static constructor on the main thread, then
    // juce::initialiseJuce_GUI() at doSetup step 0. Same thread here.
    juce::MessageManager::getInstance();
    juce::initialiseJuce_GUI();

    const std::vector<Case> cases = {
        {"a",  "setup 48k/512, dev 48k/512, deliver 512",            48000, 512, 48000, 512, {512}},
        {"b",  "setup 48k/512, dev 48k/256, deliver 256",            48000, 512, 48000, 256, {256}},
        {"c1", "dev prepared 256, deliver 512",                      48000, 512, 48000, 256, {512}},
        {"c2", "dev prepared 256, deliver 1024",                     48000, 512, 48000, 256, {1024}},
        {"d",  "dev 48k/1024, deliver 471/1024 alternating",         48000, 512, 48000, 1024, {471, 1024}},
        {"e",  "setup 44.1k/512, dev 48k/512",                       44100, 512, 48000, 512, {512}},
        {"f",  "as (b), NO message loop",                            48000, 512, 48000, 256, {256}, false},
        {"g",  "as (b), aboutToStart off message thread",            48000, 512, 48000, 256, {256}, true, true},
        {"g2", "as (g), NO message loop (negative control)",         48000, 512, 48000, 256, {256}, false, true, false, true},
        {"g3", "as (g2) but dev buf == setup buf (512)",             48000, 512, 48000, 512, {512}, false, true},
        {"h","as (b), device restart mid-note (route change)",     48000, 512, 48000, 256, {256}, true, false, true},
        {"i",  "setup reads 0 Hz/0 (device setup not populated)",    0, 0, 48000, 256, {256}},
    };

    std::printf("\n%-5s %-52s %-4s %8s %5s %11s %10s %7s %9s\n",
                "case", "description", "res", "peak", "nan", "zero/blocks", "1st-sound", "synth#", "synthPk");
    std::printf("%s\n", std::string(118, '-').c_str());
    auto row = [](const std::string& id, const std::string& d, const Result& r) {
        std::printf("%-5s %-52s %-4s %8.4f %5d %5d/%-5d %10d %7d %9.4f\n", id.c_str(), d.c_str(),
                    r.pass ? "PASS" : "FAIL", r.peakNote, r.nans, r.zeroNoteBlocks, r.noteBlocks,
                    r.firstSoundBlock, r.synthCalls, r.synthPeak);
        std::fflush(stdout);
    };
    row("ctl", "synth alone, processBlock direct, 48k/512", runSynthAlone());
    int unexpected = 0;
    for (auto& c : cases) {
        auto r = runCase(c);
        unexpected += (r.pass == c.expectSilence) ? 1 : 0;
        row(c.id, c.desc, r);
    }
    std::printf("\npeak/zero/1st-sound are over the 'note held' window; synth# = PolySynth::processBlock calls\n"
                "(whole case), synthPk = peak inside the synth before the graph.\n"
                "Case (i) logs jasserts from Voice::prepare(0 Hz) during doSetup; expected, it is re-prepared.\n");

    juce::shutdownJuce_GUI();
    std::printf("%d unexpected result(s) (g2 is expected to FAIL)\n", unexpected);
    return unexpected == 0 ? 0 : 1;
}
