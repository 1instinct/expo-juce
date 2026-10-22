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
// GraphPlayer — the shipped class, from ios/GraphPlayer.h at the same git ref as the engine.
#include "GraphPlayer.h"

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

struct BlockRec { int phase; int n; float peak; int nans; bool zero; bool tailZero; };

struct Result {
    float peakNote = 0; int nans = 0; int noteBlocks = 0, zeroNoteBlocks = 0;
    int firstSoundBlock = -1; int synthCalls = 0; float synthPeak = 0;
    int zeroTailBlocks = 0;            // note-held blocks whose last quarter is silent (chunking bug)
    uint64_t graphCalls = 0, expectedGraphCalls = 0; // GraphPlayer chunking: ceil(n / capacity) per block
    int capacity = 0, capacityAfter = 0;             // preallocated samples before/after the run
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
    std::atomic<uint64_t> expectedGraphCalls{0};
    std::atomic<int> capacityForThread{0};
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
            if (attached.load()) {
                player->audioDeviceIOCallbackWithContext(nullptr, 0, out.getArrayOfWritePointers(), 2, n, ctx);
                const int cap = capacityForThread.load();
                if (cap > 0) expectedGraphCalls.fetch_add((uint64_t)((n + cap - 1) / cap));
            }
            BlockRec r{phase.load(), n, 0.0f, 0, true, true};
            for (int ch = 0; ch < 2; ++ch) {
                const float* d = out.getReadPointer(ch);
                for (int i = 0; i < n; ++i) {
                    if (!std::isfinite(d[i])) { r.nans++; continue; }
                    r.peak = std::max(r.peak, std::fabs(d[i]));
                    if (d[i] != 0.0f) {
                        r.zero = false;
                        if (i >= n - n / 4) r.tailZero = false;
                    }
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
    const int capacityBefore = player->getCapacitySamples();
    capacityForThread.store(capacityBefore);
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
            if (r.tailZero) res.zeroTailBlocks++;
            if (r.zero) res.zeroNoteBlocks++;
            else if (res.firstSoundBlock < 0) res.firstSoundBlock = idx;
            res.peakNote = std::max(res.peakNote, r.peak);
            idx++;
        }
    }
    res.synthCalls = synth->calls.load();
    res.synthPeak = synth->peak.load();
    res.graphCalls = player->graphProcessCount.load();
    res.expectedGraphCalls = expectedGraphCalls.load();
    res.capacity = capacityBefore;
    res.capacityAfter = player->getCapacitySamples();
    // A held note must sound in every block after the first (allow 1 block for event latency),
    // including the end of every block (a chunked render must fill the whole block), and the
    // callback must render ceil(n / capacity) chunks per block without growing its buffer
    // (the restart case re-runs aboutToStart with the same device, so capacity is unchanged).
    res.pass = res.peakNote > 1e-3f && res.nans == 0 && res.zeroNoteBlocks <= 1
            && res.zeroTailBlocks <= 1
            && res.graphCalls == res.expectedGraphCalls
            && res.capacity >= 1024 && res.capacityAfter == res.capacity;

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
    // Touch-stamped noteOn, as JucePadView sends it: touch 10 ms before the enqueue.
    synth.noteOn(60, 127, juce::Time::getHighResolutionTicks() - juce::Time::secondsToHighResolutionTicks(0.010));
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
    const double touchMs = synth.diag.lastTouchToRenderMs.load();
    const double noteMs = synth.diag.lastNoteToRenderMs.load();
    std::printf("ctl touch->render %.3f ms (expect >= 10), note->render %.3f ms\n", touchMs, noteMs);
    res.pass = res.peakNote > 1e-3f && res.nans == 0 && res.zeroNoteBlocks == 0
            && touchMs >= 10.0 && touchMs < 1000.0 && noteMs >= 0.0 && noteMs < touchMs;
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
        {"j",  "dev 48k/128, deliver 128 (latency target)",          48000, 128, 48000, 128, {128}},
        {"k",  "dev 48k/256 (cap 1024), deliver 4096 (4 chunks)",    48000, 256, 48000, 256, {4096}},
        {"k2", "dev 48k/128 (cap 1024), deliver 1500/128 (chunked)", 48000, 128, 48000, 128, {1500, 128}},
    };

    std::printf("\n%-5s %-52s %-4s %8s %5s %11s %10s %7s %9s %5s %13s\n",
                "case", "description", "res", "peak", "nan", "zero/blocks", "1st-sound", "synth#", "synthPk",
                "cap", "graph#/expect");
    std::printf("%s\n", std::string(138, '-').c_str());
    auto row = [](const std::string& id, const std::string& d, const Result& r) {
        std::printf("%-5s %-52s %-4s %8.4f %5d %5d/%-5d %10d %7d %9.4f %5d %6llu/%-6llu\n", id.c_str(), d.c_str(),
                    r.pass ? "PASS" : "FAIL", r.peakNote, r.nans, r.zeroNoteBlocks, r.noteBlocks,
                    r.firstSoundBlock, r.synthCalls, r.synthPeak, r.capacity,
                    (unsigned long long)r.graphCalls, (unsigned long long)r.expectedGraphCalls);
        std::fflush(stdout);
    };
    int unexpected = 0;
    {
        const Result ctl = runSynthAlone();
        unexpected += ctl.pass ? 0 : 1;
        row("ctl", "synth alone + touch stamp, direct, 48k/512", ctl);
    }
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
