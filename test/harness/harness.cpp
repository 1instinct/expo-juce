// Offline reproduction of the iOS native audio path (ios/ExpoJuceAudioEngine.mm doSetup +
// AudioDeviceManager::addAudioCallback + the device's audio thread), driven by a fake
// AudioIODevice. See README.md.
#include "JuceConfig.h"
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "PolySynthProcessor.h"
#include "TransportEngine.h"
#include "FxChainProcessor.h"
#include "AuxProcessor.h"
#include "BackingTrack.h"
#include "AnalysisRing.h"
#include "Arpeggiator.h"
#include "SpectrumAnalyser.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <string>
#include <thread>
#include <vector>

//==============================================================================
// Allocation counter for the render path. glibc's malloc family is interposed (operator new and
// JUCE's HeapBlock both end up here); while tCountAllocs is set on a thread, every allocation it
// makes is counted. The fake device sets it around each GraphPlayer IO callback, so any
// allocation anywhere inside the render (GraphPlayer, AudioProcessorGraph, synth) shows up.
extern "C" {
void* __libc_malloc(size_t);
void* __libc_calloc(size_t, size_t);
void* __libc_realloc(void*, size_t);
void* __libc_memalign(size_t, size_t);
void __libc_free(void*);
}
static thread_local bool tCountAllocs = false;
static std::atomic<uint64_t> gRenderAllocs{0};
static std::atomic<uint64_t> gRenderFrees{0}; // frees inside counted render calls (must be 0 too)
static inline void countAlloc() {
    if (tCountAllocs) gRenderAllocs.fetch_add(1, std::memory_order_relaxed);
}
extern "C" {
void* malloc(size_t n) { countAlloc(); return __libc_malloc(n); }
void* calloc(size_t c, size_t n) { countAlloc(); return __libc_calloc(c, n); }
void* realloc(void* p, size_t n) { countAlloc(); return __libc_realloc(p, n); }
void* memalign(size_t a, size_t n) { countAlloc(); return __libc_memalign(a, n); }
void* aligned_alloc(size_t a, size_t n) { countAlloc(); return __libc_memalign(a, n); }
int posix_memalign(void** out, size_t a, size_t n) {
    countAlloc();
    void* p = __libc_memalign(a, n);
    if (p == nullptr) return 12; // ENOMEM
    *out = p;
    return 0;
}
void free(void* p) {
    if (tCountAllocs && p != nullptr) gRenderFrees.fetch_add(1, std::memory_order_relaxed);
    __libc_free(p);
}
}

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
    bool allFx = false;                // every effect enabled with non-default params
};

// Turns effect `fx` on with non-default parameters (through the string API the engine uses).
static bool enableFxNonDefault(FxParamStore& s, int fx) {
    struct P { const char* name; double value; };
    static const std::vector<std::vector<P>> sets = {
        {{"amount", 0.8}, {"tone", 0.3}, {"mix", 0.9}},
        {{"bits", 5}, {"downsample", 6}, {"noise", 0.3}, {"mix", 0.8}},
        {{"low", 6}, {"mid", -4}, {"midFreq", 2500}, {"high", 3}},
        {{"rate", 1.7}, {"depth", 0.7}, {"mix", 0.6}},
        {{"rate", 1.1}, {"depth", 0.8}, {"feedback", 0.7}, {"mix", 0.7}},
        {{"time", 0.25}, {"sync", 0}, {"division", 1}, {"feedback", 0.6}, {"tone", 0.7}, {"pingPong", 1}, {"mix", 0.5}},
        {{"size", 0.85}, {"damping", 0.3}, {"width", 0.7}, {"mix", 0.45}},
        {{"depth", 0.8}, {"shape", 0.8}, {"division", 1}},
        {{"threshold", -30}, {"ratio", 8}, {"attack", 3}, {"release", 200}, {"makeup", 9}},
    };
    bool ok = true;
    for (const P& p : sets[(size_t)fx]) ok = s.setParam(kFxSpecs[fx].id, p.name, p.value) && ok;
    return s.setEnabled(kFxSpecs[fx].id, true) && ok;
}

struct BlockRec { int phase; int n; float peak; int nans; bool zero; bool tailZero; };

struct Result {
    float peakNote = 0; int nans = 0; int noteBlocks = 0, zeroNoteBlocks = 0;
    int firstSoundBlock = -1; int synthCalls = 0; float synthPeak = 0;
    int zeroTailBlocks = 0;            // note-held blocks whose last quarter is silent (chunking bug)
    uint64_t graphCalls = 0, expectedGraphCalls = 0; // GraphPlayer chunking: ceil(n / capacity) per block
    int capacity = 0, capacityAfter = 0;             // preallocated samples before/after the run
    uint64_t renderAllocs = 0;                       // heap allocations inside IO callbacks (must be 0)
    bool pass = false;
};

static void pump(bool loop, int ms) {
    if (loop) juce::MessageManager::getInstance()->runDispatchLoopUntil(ms);
    else std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

static Result runCase(const Case& c) {
    // ── doSetup replica (ios/ExpoJuceAudioEngine.mm, HEAD), steps 2 and 5–11:
    //    synth → fx chain → output, aux (metronome + backing track, defaults: silent) → output ──
    FxParamStore fxStore;              // the engine's process-lifetime store (outlives the graph)
    AuxSettingsStore auxStore;         // process-lifetime aux settings / track holder (outlive it too)
    BackingTrack auxTrack;
    if (c.allFx)
        for (int fx = 0; fx < kFxCount; ++fx) enableFxNonDefault(fxStore, fx);
    auto* transport = new TransportEngine();
    auto* graph = new juce::AudioProcessorGraph();
    graph->setPlayConfigDetails(0, 2, c.setupSr, c.setupBuf);
    auto synthProcessor = std::make_unique<ProbeSynth>();
    ProbeSynth* synth = synthProcessor.get();
    synth->setTransport(transport);
    auto synthNode = graph->addNode(std::move(synthProcessor));
    auto fxProcessor = std::make_unique<FxChainProcessor>();
    fxProcessor->setParamStore(&fxStore);
    fxProcessor->setTransport(transport);
    auto fxNode = graph->addNode(std::move(fxProcessor));
    auto auxProcessor = std::make_unique<AuxProcessor>();
    auxProcessor->setSettings(&auxStore);
    auxProcessor->setBackingTrack(&auxTrack);
    auxProcessor->setTransport(transport);
    auto auxNode = graph->addNode(std::move(auxProcessor));
    auto outputNode = graph->addNode(std::make_unique<juce::AudioProcessorGraph::AudioGraphIOProcessor>(
        juce::AudioProcessorGraph::AudioGraphIOProcessor::audioOutputNode));
    graph->addConnection({{synthNode->nodeID, 0}, {fxNode->nodeID, 0}});
    graph->addConnection({{synthNode->nodeID, 1}, {fxNode->nodeID, 1}});
    graph->addConnection({{fxNode->nodeID, 0}, {outputNode->nodeID, 0}});
    graph->addConnection({{fxNode->nodeID, 1}, {outputNode->nodeID, 1}});
    graph->addConnection({{auxNode->nodeID, 0}, {outputNode->nodeID, 0}});
    graph->addConnection({{auxNode->nodeID, 1}, {outputNode->nodeID, 1}});
    graph->prepareToPlay(c.setupSr, c.setupBuf);
    auto* player = new GraphPlayer();
    player->setProcessor(graph);

    FakeDevice device(c.devSr, c.devBuf);

    gRenderAllocs.store(0);

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
                tCountAllocs = true;
                player->audioDeviceIOCallbackWithContext(nullptr, 0, out.getArrayOfWritePointers(), 2, n, ctx);
                tCountAllocs = false;
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
    res.renderAllocs = gRenderAllocs.load();
    // A held note must sound in every block after the first (allow 1 block for event latency),
    // including the end of every block (a chunked render must fill the whole block), and the
    // callback must render ceil(n / capacity) chunks per block without growing its buffer
    // (the restart case re-runs aboutToStart with the same device, so capacity is unchanged).
    res.pass = res.peakNote > 1e-3f && res.nans == 0 && res.zeroNoteBlocks <= 1
            && res.zeroTailBlocks <= 1
            && res.graphCalls == res.expectedGraphCalls
            && res.capacity >= 1024 && res.capacityAfter == res.capacity
            && res.renderAllocs == 0;

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

// Multi-producer note queue: two producer threads (as the main thread + JS thread do in the
// app) hammer noteOn/noteOff while an audio thread consumes. Checks the producer spinlock keeps
// the counters consistent (attempted == queued + dropped, queued == consumed after draining)
// and that allNotesOff leaves no stuck voice.
struct MultiProducerResult {
    uint32_t attempted = 0, queued = 0, consumed = 0, dropped = 0;
    int activeAfter = -1, nans = 0;
    bool pass = false;
};

static MultiProducerResult runMultiProducer() {
    constexpr int kPerProducer = 10000;
    constexpr int kBlock = 128;
    TransportEngine transport;
    PolySynthProcessor synth;
    synth.setTransport(&transport);
    synth.setRateAndBufferSizeDetails(48000, kBlock);
    synth.prepareToPlay(48000, kBlock);
    synth.setRelease(10.0);

    std::atomic<bool> stopAudio{false};
    std::atomic<int> nans{0};
    std::thread audio([&] {
        juce::AudioBuffer<float> buf(2, kBlock);
        juce::MidiBuffer midi;
        while (!stopAudio.load()) {
            synth.processBlock(buf, midi);
            for (int i = 0; i < kBlock; ++i)
                if (!std::isfinite(buf.getSample(0, i))) nans.fetch_add(1);
        }
    });

    auto producer = [&](int baseNote) {
        for (int i = 0; i < kPerProducer; ++i) {
            const int note = baseNote + (i % 24);
            synth.noteOn(note, 100);
            synth.noteOff(note);
            // Pace a little so most events are accepted (the queue holds 255) while the two
            // producers still overlap constantly.
            if ((i & 7) == 7) std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    };
    std::thread p1(producer, 36);
    std::thread p2(producer, 72);
    p1.join();
    p2.join();

    // Let the audio thread drain what is left, then silence everything.
    auto drained = [&] {
        return synth.noteQueue().notesConsumed.load() == synth.noteQueue().notesQueued.load();
    };
    for (int i = 0; i < 2000 && !drained(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    synth.allNotesOff();
    std::this_thread::sleep_for(std::chrono::milliseconds(50)); // consumer picks up AllNotesOff
    stopAudio.store(true);
    audio.join();

    // Render ~1 s more on this thread so every release (10 ms) has finished.
    {
        juce::AudioBuffer<float> buf(2, kBlock);
        juce::MidiBuffer midi;
        for (int b = 0; b < 48000 / kBlock; ++b) synth.processBlock(buf, midi);
    }

    MultiProducerResult r;
    r.attempted = 2u * kPerProducer;
    r.queued = synth.noteQueue().notesQueued.load();
    r.consumed = synth.noteQueue().notesConsumed.load();
    r.dropped = synth.noteQueue().notesDropped.load();
    r.activeAfter = synth.diag.activeVoices.load();
    r.nans = nans.load();
    r.pass = r.attempted == r.queued + r.dropped && r.queued == r.consumed && r.activeAfter == 0 && r.nans == 0;
    return r;
}

// Note releases (NoteEventQueue marks). Single-threaded and block by block, so each step is
// deterministic. Scenarios:
//  stopped: a release sent while no block renders (device stopped / being reopened) takes effect
//           on the first block after, and the voice dies out.
//  full:    a release sent while the note-on ring is full (old design: the NoteOff event was
//           dropped and the note sustained forever) still releases the held note.
//  early:   note-off before the note-on was consumed: the note never sustains.
//  order:   note-off / all-notes-off followed by a new note-on of the same note before the next
//           block: the release must not cut the newer note.
struct ReleaseResult {
    int stoppedActive = -1, fullActive = -1, earlyActive = -1, orderActive = -1;
    uint32_t fullDropped = 0;
    bool pass = false;
};

static ReleaseResult runReleases() {
    constexpr int kBlock = 128;
    auto prepare = [](PolySynthProcessor& synth, TransportEngine& transport) {
        synth.setTransport(&transport);
        synth.setRateAndBufferSizeDetails(48000, kBlock);
        synth.prepareToPlay(48000, kBlock);
        synth.setRelease(10.0);
    };
    auto render = [](PolySynthProcessor& synth, double ms) {
        juce::AudioBuffer<float> buf(2, kBlock);
        juce::MidiBuffer midi;
        const int blocks = std::max(1, (int)(ms * 48.0 / kBlock));
        for (int b = 0; b < blocks; ++b) synth.processBlock(buf, midi);
        return synth.diag.activeVoices.load();
    };
    ReleaseResult r;
    {
        TransportEngine t; PolySynthProcessor s; prepare(s, t);
        s.noteOn(60, 127);
        render(s, 20);
        s.noteOff(60);              // no block renders here
        r.stoppedActive = render(s, 200);
    }
    {
        TransportEngine t; PolySynthProcessor s; prepare(s, t);
        s.noteOn(60, 127);
        render(s, 20);
        for (int i = 0; i < NoteEventQueue::kSize + 20; ++i) s.noteOn(61, 127); // ring full
        r.fullDropped = s.noteQueue().notesDropped.load();
        s.noteOff(60);
        s.noteOff(61);
        r.fullActive = render(s, 300);
    }
    {
        TransportEngine t; PolySynthProcessor s; prepare(s, t);
        s.noteOn(62, 127);
        s.noteOff(62);
        r.earlyActive = render(s, 200);
    }
    {
        TransportEngine t; PolySynthProcessor s; prepare(s, t);
        s.noteOn(63, 127);
        s.noteOn(64, 127);
        render(s, 20);
        s.allNotesOff();            // releases 63 and 64 ...
        s.noteOn(64, 127);          // ... but not this newer note-on of 64
        s.noteOff(63);
        s.noteOn(63, 127);          // newer note-on of the released note
        r.orderActive = render(s, 200); // both newer notes still held
    }
    r.pass = r.stoppedActive == 0 && r.fullDropped > 0 && r.fullActive == 0 && r.earlyActive == 0
          && r.orderActive == 2;
    std::printf("rl    note releases (stopped device, full ring, early off, ordering)  %-4s active after: stopped %d,"
                " full %d (ring drops %u), early %d; order %d (want 2)\n",
                r.pass ? "PASS" : "FAIL", r.stoppedActive, r.fullActive, r.fullDropped, r.earlyActive,
                r.orderActive);
    std::fflush(stdout);
    return r;
}

// Rebuild state carry-over: what the engine captures before a rebuild's teardown
// (getSettings / getState) must come back identical on freshly built objects (applySettings /
// restoreState), including a seek the audio thread has not applied yet.
static bool runRebuildState() {
    TransportEngine oldTransport;
    PolySynthProcessor oldSynth;
    oldSynth.setTransport(&oldTransport);
    oldSynth.setRateAndBufferSizeDetails(48000, 256);
    oldSynth.prepareToPlay(48000, 256);
    oldSynth.setWaveform(Waveform::Saw);
    oldSynth.setFilterCutoff(1234.0);
    oldSynth.setFilterResonance(0.7);
    oldSynth.setAttack(15.0);
    oldSynth.setDecay(300.0);
    oldSynth.setSustain(0.4);
    oldSynth.setRelease(900.0);
    oldSynth.setDetune(-25.0);
    oldSynth.setMasterLevel(0.0); // muted: must not come back at the 0.5 default
    ArpSettings arpWant = ArpSettings::defaults();
    arpWant.enabled = true; arpWant.mode = ArpMode::UpDown; arpWant.rate = ArpRate::EighthT; arpWant.octaves = 3;
    arpWant.gate = 0.9f; arpWant.swing = 0.2f; arpWant.latch = true; arpWant.patternMask = 0x5A5A; arpWant.patternLength = 12;
    arpWant.accent = 0.3f; arpWant.ratchet = 2; arpWant.transposeSteps = 3; arpWant.transposeDistance = -7;
    oldSynth.arp().setSettings(arpWant);
    oldTransport.setTempo(90.0);
    oldTransport.start();
    juce::AudioBuffer<float> buf(2, 256);
    juce::MidiBuffer midi;
    for (int b = 0; b < 40; ++b) oldSynth.processBlock(buf, midi); // position advances
    const double played = oldTransport.getPosition();
    oldTransport.setPosition(7.5); // seek not yet applied by the audio thread

    const SynthSettings ss = oldSynth.getSettings();
    const TransportState ts = oldTransport.getState();

    TransportEngine newTransport;
    PolySynthProcessor newSynth;
    newSynth.setTransport(&newTransport);
    newSynth.applySettings(ss);
    newTransport.restoreState(ts);

    const SynthSettings back = newSynth.getSettings();
    const bool synthOk = back.waveform == static_cast<int>(Waveform::Saw) && back.filterCutoff == 1234.0f
        && back.filterResonance == 0.7f && back.attackMs == 15.0f && back.decayMs == 300.0f
        && back.sustainLevel == 0.4f && back.releaseMs == 900.0f && back.detuneCents == -25.0f
        && back.masterLevel == 0.0f && back.arp == arpWant && ss.arp == arpWant && newSynth.arp().getSettings() == arpWant
        && arpWant != ArpSettings::defaults();
    const bool transportOk = played > 0.0 && ts.position == 7.5 && newTransport.getTempo() == 90.0
        && newTransport.isPlaying() && newTransport.getPosition() == 7.5;
    newSynth.setRateAndBufferSizeDetails(48000, 256);
    newSynth.prepareToPlay(48000, 256);
    newSynth.processBlock(buf, midi);
    const bool continuesOk = newTransport.getPosition() > 7.5 && newTransport.getPosition() < 7.6;

    // Effects: the engine keeps one FxParamStore for the process and attaches it to each new
    // FxChainProcessor, so a rebuilt chain renders exactly like the old one did. Checked: the
    // string API (clamping, rounding, unknown id / param / NaN rejected), snapshot / apply
    // round trip, and two chains (old store, store restored from its snapshot) rendering the
    // same input bit-identically.
    FxParamStore oldFx;
    bool apiOk = true;
    for (int fx : {kFxDrive, kFxEq, kFxEcho, kFxReverb, kFxPump, kFxCompressor})
        apiOk = enableFxNonDefault(oldFx, fx) && apiOk;
    apiOk = oldFx.setEnabled("eq", false) && apiOk; // params kept while off
    apiOk = oldFx.setParam("echo", "feedback", 5.0) && oldFx.get(kFxEcho, kEchoFeedback) == 0.95f && apiOk;
    apiOk = oldFx.setParam("pump", "division", 2.6) && oldFx.get(kFxPump, kPumpDivision) == 3.0f && apiOk;
    apiOk = oldFx.setParam("eq", "midFreq", -1.0) && oldFx.get(kFxEq, kEqMidFreq) == 200.0f && apiOk;
    apiOk = !oldFx.setParam("nope", "mix", 0.5) && !oldFx.setParam("echo", "nope", 0.5) && apiOk;
    apiOk = !oldFx.setEnabled("nope", true) && !oldFx.setParam("echo", "time", std::nan("")) && apiOk;
    apiOk = oldFx.get(kFxEcho, kEchoTime) == 0.25f && apiOk; // the NaN left it unchanged
    const FxSettings fxSnap = oldFx.snapshot();
    FxParamStore newFx;
    newFx.apply(fxSnap);
    const bool fxStateOk = apiOk && newFx.snapshot() == fxSnap && !(fxSnap == FxSettings::defaults())
        && fxSnap.enabled[kFxEcho] && !fxSnap.enabled[kFxEq] && fxSnap.values[kFxEq][kEqLow] == 6.0f;
    bool fxRenderOk = true;
    {
        TransportEngine ta, tb;
        FxChainProcessor a, b;
        a.setParamStore(&oldFx); a.setTransport(&ta);
        b.setParamStore(&newFx); b.setTransport(&tb);
        for (FxChainProcessor* p : {&a, &b}) { p->setRateAndBufferSizeDetails(48000, 256); p->prepareToPlay(48000, 256); }
        juce::AudioBuffer<float> ba(2, 256), bb(2, 256);
        double phase = 0.0;
        for (int blk = 0; blk < 200; ++blk) {
            for (int i = 0; i < 256; ++i, phase += 0.0571) {
                const float v = 0.4f * (float)std::sin(phase);
                for (int ch = 0; ch < 2; ++ch) { ba.setSample(ch, i, v); bb.setSample(ch, i, v); }
            }
            ta.advanceBlock(256, 48000); tb.advanceBlock(256, 48000);
            a.processBlock(ba, midi);
            b.processBlock(bb, midi);
            for (int ch = 0; ch < 2; ++ch)
                if (std::memcmp(ba.getReadPointer(ch), bb.getReadPointer(ch), 256 * sizeof(float)) != 0) fxRenderOk = false;
        }
    }
    const bool fxOk = fxStateOk && fxRenderOk;
    const bool ok = synthOk && transportOk && continuesOk && fxOk;
    std::printf("rs    rebuild carry-over (settings + arp, tempo 90, playing, pending seek, fx)  %-4s synth %d transport %d"
                " continues %d (pos %.4f) fx state %d fx render %d\n", ok ? "PASS" : "FAIL",
                synthOk, transportOk, continuesOk, newTransport.getPosition(), fxStateOk, fxRenderOk);
    std::fflush(stdout);
    return ok;
}

//==============================================================================
// Effects chain, standalone (synth → FxChainProcessor on this thread, the graph's order).
// Every render call (synth + fx) runs with the allocation counter on.

struct FxRig {
    TransportEngine transport;
    PolySynthProcessor synth;
    FxParamStore store;
    FxChainProcessor fx;
    explicit FxRig(double sr) {
        synth.setTransport(&transport);
        synth.setRateAndBufferSizeDetails(sr, 512);
        synth.prepareToPlay(sr, 512);
        fx.setParamStore(&store);
        fx.setTransport(&transport);
        fx.setRateAndBufferSizeDetails(sr, 512);
        fx.prepareToPlay(sr, 512);
    }
};

// Block sizes cycled by the fx tests: odd sizes and blocks larger than the chain's chunk.
static const std::vector<int> kFxBlocks = {256, 471, 64, 1024, 128, 333};

// Each effect alone (and all together) with non-default params: finite, non-silent, audibly
// different from the dry synth, never above the limiter ceiling, 0 allocations.
static int runFxEffects() {
    int failures = 0;
    const float ceiling = juce::Decibels::decibelsToGain(FxChainProcessor::kLimiterCeilingDb);
    for (int which = 0; which <= kFxCount; ++which) {
        FxRig rig(48000);
        rig.transport.setTempo(120);
        rig.transport.start();
        bool apiOk = true;
        if (which == kFxCount) {
            for (int fx = 0; fx < kFxCount; ++fx) apiOk = enableFxNonDefault(rig.store, fx) && apiOk;
        } else {
            apiOk = enableFxNonDefault(rig.store, which);
        }
        rig.synth.setWaveform(Waveform::Saw);
        rig.synth.setMasterLevel(0.6);
        rig.synth.noteOn(57, 120);
        rig.synth.noteOn(64, 100);
        juce::AudioBuffer<float> buf(2, 1024), dry(2, 1024);
        juce::MidiBuffer midi;
        const int total = (int)(1.5 * 48000);
        int done = 0, nans = 0;
        size_t bi = 0;
        float peak = 0.0f;
        double diff2 = 0.0;
        bool released = false;
        gRenderAllocs.store(0);
        while (done < total) {
            const int n = kFxBlocks[bi++ % kFxBlocks.size()];
            if (!released && done >= 48000) { rig.synth.noteOff(57); rig.synth.noteOff(64); released = true; }
            buf.setSize(2, n, false, false, true);
            dry.setSize(2, n, false, false, true);
            tCountAllocs = true;
            rig.synth.processBlock(buf, midi);
            tCountAllocs = false;
            for (int ch = 0; ch < 2; ++ch) dry.copyFrom(ch, 0, buf, ch, 0, n);
            tCountAllocs = true;
            rig.fx.processBlock(buf, midi);
            tCountAllocs = false;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < n; ++i) {
                    const float v = buf.getSample(ch, i);
                    if (!std::isfinite(v)) { ++nans; continue; }
                    peak = std::max(peak, std::fabs(v));
                    const double d = (double)v - dry.getSample(ch, i);
                    diff2 += d * d;
                }
            done += n;
        }
        const double diffRms = std::sqrt(diff2 / (2.0 * total));
        const uint64_t allocs = gRenderAllocs.load();
        const bool pass = apiOk && nans == 0 && peak > 1e-3f && peak <= ceiling + 1e-6f && diffRms > 1e-4 && allocs == 0;
        failures += pass ? 0 : 1;
        const std::string id = which == kFxCount ? std::string("all") : std::string(kFxSpecs[which].id);
        std::printf("fx    %-12s non-default params, blocks 256/471/64/1024/128/333  %-4s peak %.4f (ceiling %.4f)"
                    " diff-rms %.5f nan %d allocs %llu limiter-min %.3f\n", id.c_str(), pass ? "PASS" : "FAIL", peak,
                    ceiling, diffRms, nans, (unsigned long long)allocs, rig.fx.diag.limiterGain.load());
        std::fflush(stdout);
    }
    return failures;
}

// Bypass: chain all off == dry synth, bit-exact; on again (reverb + echo) it differs; turned
// off, their tails keep playing (output != dry right after), and once both tails have ended
// (at most kMaxTailSeconds) the output is bit-exact again. 0 allocations throughout.
static bool runFxBypass() {
    FxRig rig(48000);
    rig.synth.setWaveform(Waveform::Saw);
    rig.synth.setMasterLevel(0.4);
    rig.synth.noteOn(60, 100);
    juce::AudioBuffer<float> buf(2, 1024), dry(2, 1024);
    juce::MidiBuffer midi;
    int done = 0;
    size_t bi = 0;
    int mismatchOff = 0, comparedOff = 0, mismatchAfter = 0, comparedAfter = 0;
    double diffOn = 0.0, diffTail = 0.0;
    float dryPeak = 0.0f;
    const int onAt = 24000, offAt = 48000, end = offAt + (int)(FxChainProcessor::kMaxTailSeconds * 48000) + 96000;
    int tailEnd = -1; // first block start at which neither echo nor reverb runs any more
    gRenderAllocs.store(0); gRenderFrees.store(0);
    while (done < end) {
        const int n = kFxBlocks[bi++ % kFxBlocks.size()];
        if (done >= onAt && done < offAt) { rig.store.setEnabled("reverb", true); rig.store.setEnabled("echo", true); }
        if (done >= offAt) { rig.store.setEnabled("reverb", false); rig.store.setEnabled("echo", false); }
        buf.setSize(2, n, false, false, true);
        dry.setSize(2, n, false, false, true);
        tCountAllocs = true;
        rig.synth.processBlock(buf, midi);
        tCountAllocs = false;
        for (int ch = 0; ch < 2; ++ch) dry.copyFrom(ch, 0, buf, ch, 0, n);
        dryPeak = std::max(dryPeak, dry.getMagnitude(0, n));
        const bool tailsOver = done >= offAt && !rig.fx.isEffectRunning(kFxEcho) && !rig.fx.isEffectRunning(kFxReverb);
        if (tailsOver && tailEnd < 0) tailEnd = done;
        tCountAllocs = true;
        rig.fx.processBlock(buf, midi);
        tCountAllocs = false;
        bool same = true;
        for (int ch = 0; ch < 2; ++ch)
            if (std::memcmp(buf.getReadPointer(ch), dry.getReadPointer(ch), (size_t)n * sizeof(float)) != 0) same = false;
        if (done + n <= onAt) { ++comparedOff; mismatchOff += same ? 0 : 1; }
        else if (tailsOver) { ++comparedAfter; mismatchAfter += same ? 0 : 1; }
        else if (done >= onAt && done + n <= offAt) {
            for (int i = 0; i < n; ++i) { const double d = buf.getSample(0, i) - dry.getSample(0, i); diffOn += d * d; }
        } else if (done >= offAt + 960 && done + n <= offAt + 9600) { // 20–200 ms after off: the tail
            for (int i = 0; i < n; ++i) { const double d = buf.getSample(0, i) - dry.getSample(0, i); diffTail += d * d; }
        }
        done += n;
    }
    const uint64_t allocs = gRenderAllocs.load() + gRenderFrees.load();
    const double tailSec = tailEnd < 0 ? -1.0 : (tailEnd - offAt) / 48000.0;
    const bool pass = comparedOff > 10 && mismatchOff == 0 && comparedAfter > 10 && mismatchAfter == 0
                   && diffOn > 1e-3 && diffTail > 1e-4 && tailEnd > 0
                   && tailSec <= FxChainProcessor::kMaxTailSeconds + 0.03 && allocs == 0;
    std::printf("fxb   chain off == dry bit-exact; on (reverb+echo) differs; off: tail spills, then == dry  %-4s off %d/%d"
                " blocks equal, tail %.3f s (diff %.4f), after tail %d/%d equal, on-diff %.4f, dry peak %.4f, allocs %llu\n",
                pass ? "PASS" : "FAIL", comparedOff - mismatchOff, comparedOff, tailSec, diffTail,
                comparedAfter - mismatchAfter, comparedAfter, diffOn, dryPeak, (unsigned long long)allocs);
    std::fflush(stdout);
    return pass;
}

// Renders `fx` alone on `input(i)` (both channels) for `total` samples in 471-sample blocks;
// `control(done)` runs before each block.
static void renderFxAlone(FxChainProcessor& fx, int total, const std::function<float(int)>& input,
                          const std::function<void(int)>& control, std::vector<float>& outL, std::vector<float>& outR) {
    juce::AudioBuffer<float> buf(2, 512);
    juce::MidiBuffer midi;
    outL.assign((size_t)total, 0.0f);
    outR.assign((size_t)total, 0.0f);
    for (int done = 0; done < total;) {
        const int n = std::min(471, total - done);
        control(done);
        buf.setSize(2, n, false, false, true);
        for (int i = 0; i < n; ++i) { const float v = input(done + i); buf.setSample(0, i, v); buf.setSample(1, i, v); }
        tCountAllocs = true;
        fx.processBlock(buf, midi);
        tCountAllocs = false;
        std::memcpy(&outL[(size_t)done], buf.getReadPointer(0), (size_t)n * sizeof(float));
        std::memcpy(&outR[(size_t)done], buf.getReadPointer(1), (size_t)n * sizeof(float));
        done += n;
    }
}

// Echo tail cap + clean restart without clearing the line: echo at feedback 0.95 (a tail far
// longer than 5 s) on a noise burst, turned off: the tail plays on, is faded out and bypassed
// at kMaxTailSeconds; the output is then exactly 0 (silent input). Turned on again with a
// silent input: still exactly 0 (the old line content is never read back, without a memset).
// Then a non-finite input with the reverb on: silenced, and the effects are not reset per chunk.
static bool runFxTails() {
    const double sr = 48000.0;
    FxParamStore store;
    FxChainProcessor fx;
    fx.setParamStore(&store);
    fx.setRateAndBufferSizeDetails(sr, 512);
    fx.prepareToPlay(sr, 512);
    store.setParam("echo", "sync", 0);
    store.setParam("echo", "time", 0.25);
    store.setParam("echo", "feedback", 0.95);
    store.setParam("echo", "tone", 1);
    store.setParam("echo", "mix", 1);
    store.setEnabled("echo", true);
    const int offAt = (int)(0.3 * sr), onAgainAt = (int)(6.0 * sr), total = (int)(7.0 * sr);
    uint32_t rng = 12345u;
    std::vector<float> l, r;
    int endedAt = -1;
    gRenderAllocs.store(0); gRenderFrees.store(0);
    renderFxAlone(fx, total, [&](int i) {
        if (i >= (int)(0.2 * sr)) return 0.0f;
        rng = rng * 1664525u + 1013904223u;
        return 0.3f * ((float)(rng >> 8) / 8388608.0f - 1.0f);
    }, [&](int done) {
        if (done >= offAt && done < onAgainAt) store.setEnabled("echo", false);
        if (done >= onAgainAt) store.setEnabled("echo", true);
        if (done >= offAt && done < onAgainAt && endedAt < 0 && !fx.isEffectRunning(kFxEcho)) endedAt = done;
    }, l, r);
    float tailPeak = 0.0f, lateTail = 0.0f;
    int nonZeroAfter = 0, nonZeroOnAgain = 0;
    for (int i = offAt + 4800; i < offAt + 48000; ++i) tailPeak = std::max(tailPeak, std::fabs(l[(size_t)i]));
    for (int i = offAt + (int)(4.5 * sr); i < offAt + (int)(4.9 * sr); ++i) lateTail = std::max(lateTail, std::fabs(l[(size_t)i]));
    if (endedAt > 0)
        for (int i = endedAt; i < onAgainAt; ++i) nonZeroAfter += (l[(size_t)i] != 0.0f || r[(size_t)i] != 0.0f) ? 1 : 0;
    for (int i = onAgainAt; i < total; ++i) nonZeroOnAgain += (l[(size_t)i] != 0.0f || r[(size_t)i] != 0.0f) ? 1 : 0;
    const double endedSec = endedAt < 0 ? -1.0 : (endedAt - offAt) / sr;
    const bool tailOk = tailPeak > 0.05f && lateTail > 1e-3f && endedAt > 0
                     && std::fabs(endedSec - FxChainProcessor::kMaxTailSeconds) < 0.03 && nonZeroAfter == 0;
    const bool restartOk = nonZeroOnAgain == 0 && fx.isEffectRunning(kFxEcho);

    // Non-finite input with the reverb on.
    FxChainProcessor fx2;
    FxParamStore store2;
    fx2.setParamStore(&store2);
    fx2.setRateAndBufferSizeDetails(sr, 512);
    fx2.prepareToPlay(sr, 512);
    store2.setEnabled("reverb", true);
    std::vector<float> l2, r2;
    renderFxAlone(fx2, 48000, [](int i) {
        if (i >= 12000 && i < 24000) return std::numeric_limits<float>::quiet_NaN();
        return 0.3f * (float)std::sin(i * 0.05);
    }, [](int) {}, l2, r2);
    int nonFiniteOut = 0, nonZeroDuringNaN = 0;
    for (int i = 0; i < 48000; ++i) nonFiniteOut += std::isfinite(l2[(size_t)i]) && std::isfinite(r2[(size_t)i]) ? 0 : 1;
    for (int i = 12256; i < 23552; ++i) nonZeroDuringNaN += l2[(size_t)i] != 0.0f ? 1 : 0; // whole chunks of NaN input
    const uint64_t resets = fx2.diag.nonFiniteResets.load();
    float recovered = 0.0f;
    for (int i = 36000; i < 48000; ++i) recovered = std::max(recovered, std::fabs(l2[(size_t)i]));
    const bool nanOk = nonFiniteOut == 0 && nonZeroDuringNaN == 0 && resets <= 1 && recovered > 0.05f;
    const uint64_t allocs = gRenderAllocs.load() + gRenderFrees.load();
    const bool pass = tailOk && restartOk && nanOk && allocs == 0;
    std::printf("fxt   echo tail (fb 0.95) peak %.4f, capped at %.3f s (want %.1f), then %d non-zero; re-enabled on silence:"
                " %d non-zero; NaN input: %d non-finite out, %llu resets (<= 1), recovers %.3f  %-4s allocs %llu\n",
                tailPeak, endedSec, FxChainProcessor::kMaxTailSeconds, nonZeroAfter, nonZeroOnAgain, nonFiniteOut,
                (unsigned long long)resets, recovered, pass ? "PASS" : "FAIL", (unsigned long long)allocs);
    std::fflush(stdout);
    return pass;
}

// src/effectDefaults.ts (used on web and with older binaries) must list the same effects,
// parameters (same order) and default values as kFxSpecs.
static bool runFxDefaultsMatchJs() {
    const juce::File file(juce::String(HARNESS_JS_SRC) + "/effectDefaults.ts");
    juce::StringArray lines;
    file.readLines(lines);
    int effectsSeen = 0, mismatches = 0;
    std::string detail;
    for (int fx = 0; fx < kFxCount; ++fx) {
        const FxSpec& spec = kFxSpecs[fx];
        const juce::String prefix = juce::String("  ") + spec.id + ": {";
        juce::String body;
        for (const juce::String& line : lines)
            if (line.startsWith(prefix))
                body = line.fromFirstOccurrenceOf("{", false, false).upToLastOccurrenceOf("}", false, false);
        if (body.isEmpty()) { ++mismatches; detail += std::string(" missing ") + spec.id; continue; }
        ++effectsSeen;
        juce::StringArray pairs;
        pairs.addTokens(body, ",", "");
        pairs.trim();
        pairs.removeEmptyStrings();
        if (pairs.size() != spec.numParams) { ++mismatches; detail += std::string(" count:") + spec.id; continue; }
        for (int p = 0; p < spec.numParams; ++p) {
            const juce::String name = pairs[p].upToFirstOccurrenceOf(":", false, false).trim();
            const double value = pairs[p].fromFirstOccurrenceOf(":", false, false).trim().getDoubleValue();
            const double want = (double)spec.params[p].defaultValue;
            if (name != spec.params[p].name || std::fabs(value - want) > 1e-6 * std::max(1.0, std::fabs(want))) {
                ++mismatches;
                detail += std::string(" ") + spec.id + "." + spec.params[p].name;
            }
        }
    }
    const bool pass = file.existsAsFile() && effectsSeen == kFxCount && mismatches == 0;
    std::printf("fxd   src/effectDefaults.ts == kFxSpecs defaults (%d effects)  %-4s %d mismatches%s\n", effectsSeen,
                pass ? "PASS" : "FAIL", mismatches, detail.c_str());
    std::fflush(stdout);
    return pass;
}

// Echo sync: an impulse through echo (sync on, mix 1, feedback 0, tone 1) comes back exactly
// division x 60 / bpm seconds later; nothing before it. The transport is advanced per block as
// the synth does it (block-start beat info). Odd block size (471) so the echo crosses blocks.
static bool runFxEchoSyncOne(double bpm, int division, int expectedDelay) {
    TransportEngine transport;
    transport.setTempo(bpm);
    FxParamStore store;
    FxChainProcessor fx;
    fx.setParamStore(&store);
    fx.setTransport(&transport);
    fx.setRateAndBufferSizeDetails(48000, 512);
    fx.prepareToPlay(48000, 512);
    bool ok = store.setParam("echo", "sync", 1) && store.setParam("echo", "division", division)
           && store.setParam("echo", "feedback", 0) && store.setParam("echo", "tone", 1)
           && store.setParam("echo", "mix", 1) && store.setParam("echo", "pingPong", 0)
           && store.setEnabled("echo", true);
    const int impulseAt = 3000; // after the 10 ms enable crossfade
    const int total = impulseAt + expectedDelay + 4000;
    std::vector<float> outL((size_t)total, 0.0f), outR((size_t)total, 0.0f);
    juce::AudioBuffer<float> buf(2, 512);
    juce::MidiBuffer midi;
    gRenderAllocs.store(0);
    for (int done = 0; done < total;) {
        const int n = std::min(471, total - done);
        buf.setSize(2, n, false, false, true);
        buf.clear();
        if (impulseAt >= done && impulseAt < done + n) {
            buf.setSample(0, impulseAt - done, 0.5f);
            buf.setSample(1, impulseAt - done, 0.5f);
        }
        tCountAllocs = true;
        transport.advanceBlock(n, 48000);
        fx.processBlock(buf, midi);
        tCountAllocs = false;
        std::memcpy(&outL[(size_t)done], buf.getReadPointer(0), (size_t)n * sizeof(float));
        std::memcpy(&outR[(size_t)done], buf.getReadPointer(1), (size_t)n * sizeof(float));
        done += n;
    }
    int first = -1;
    for (int i = 0; i < total && first < 0; ++i)
        if (std::fabs(outL[(size_t)i]) > 1e-7f || std::fabs(outR[(size_t)i]) > 1e-7f) first = i;
    const int got = first < 0 ? -1 : first - impulseAt;
    const float echoPeak = first < 0 ? 0.0f : outL[(size_t)first];
    ok = ok && got == expectedDelay && echoPeak > 0.3f && gRenderAllocs.load() == 0;
    std::printf("fxe   echo sync %.0f bpm division %d: impulse returns after %d samples (expect %d)  %-4s peak %.4f allocs %llu\n",
                bpm, division, got, expectedDelay, ok ? "PASS" : "FAIL", echoPeak,
                (unsigned long long)gRenderAllocs.load());
    std::fflush(stdout);
    return ok;
}

// Pump beat lock + limiter. Pump (depth 1, shape 1, 1/4) on a constant 0.5 input with the
// transport started at beat 0.5 at 120 bpm: the gain dips right after each beat boundary
// (samples 12000, 36000, 60000; 4 ms attack), whatever the block size (471). Then the limiter
// alone: a 2.0-amplitude sine through the all-off chain never exceeds -0.3 dBFS.
static bool runFxPumpAndLimiter() {
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> buf(2, 512);
    gRenderAllocs.store(0);
    bool pumpOk = true;
    int dips[3] = {-1, -1, -1};
    {
        TransportEngine transport;
        transport.setTempo(120);
        transport.setPosition(0.5);
        transport.start();
        FxParamStore store;
        FxChainProcessor fx;
        fx.setParamStore(&store);
        fx.setTransport(&transport);
        fx.setRateAndBufferSizeDetails(48000, 512);
        fx.prepareToPlay(48000, 512);
        pumpOk = store.setParam("pump", "depth", 1) && store.setParam("pump", "shape", 1)
              && store.setParam("pump", "division", 0) && store.setEnabled("pump", true);
        const int total = 66000;
        std::vector<float> out((size_t)total, 0.0f);
        for (int done = 0; done < total;) {
            const int n = std::min(471, total - done);
            buf.setSize(2, n, false, false, true);
            for (int ch = 0; ch < 2; ++ch) juce::FloatVectorOperations::fill(buf.getWritePointer(ch), 0.5f, n);
            tCountAllocs = true;
            transport.advanceBlock(n, 48000);
            fx.processBlock(buf, midi);
            tCountAllocs = false;
            std::memcpy(&out[(size_t)done], buf.getReadPointer(0), (size_t)n * sizeof(float));
            done += n;
        }
        // Lowest gain within each beat (windows centred on the beats, skipping the fade-in).
        const int beats[3] = {12000, 36000, 60000};
        for (int b = 0; b < 3; ++b) {
            int at = -1;
            float lo = 1e9f;
            for (int i = beats[b] - 6000; i < beats[b] + 6000; ++i)
                if (out[(size_t)i] < lo) { lo = out[(size_t)i]; at = i; }
            dips[b] = at - beats[b];
            pumpOk = pumpOk && dips[b] >= 0 && dips[b] <= 400 && lo < 0.05f && out[(size_t)beats[b] - 1] > 0.3f;
        }
    }
    bool limiterOk = true;
    float limPeak = 0.0f;
    {
        FxChainProcessor fx; // own store: all effects off
        fx.setRateAndBufferSizeDetails(48000, 512);
        fx.prepareToPlay(48000, 512);
        double phase = 0.0;
        for (int blk = 0; blk < 100; ++blk) {
            buf.setSize(2, 333, false, false, true);
            for (int i = 0; i < 333; ++i, phase += 0.0571)
                for (int ch = 0; ch < 2; ++ch) buf.setSample(ch, i, 2.0f * (float)std::sin(phase));
            tCountAllocs = true;
            fx.processBlock(buf, midi);
            tCountAllocs = false;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 333; ++i) {
                    const float v = buf.getSample(ch, i);
                    if (!std::isfinite(v)) limiterOk = false;
                    limPeak = std::max(limPeak, std::fabs(v));
                }
        }
        const float ceiling = juce::Decibels::decibelsToGain(FxChainProcessor::kLimiterCeilingDb);
        limiterOk = limiterOk && limPeak <= ceiling * 1.0001f && limPeak > 0.9f;
    }
    const uint64_t allocs = gRenderAllocs.load();
    const bool pass = pumpOk && limiterOk && allocs == 0;
    std::printf("fxp   pump locked to beat (start 0.5, 120 bpm), limiter on a 2.0 sine  %-4s dips +%d/+%d/+%d samples"
                " after the beats (want 0..400), limiter peak %.4f, allocs %llu\n", pass ? "PASS" : "FAIL",
                dips[0], dips[1], dips[2], limPeak, (unsigned long long)allocs);
    std::fflush(stdout);
    return pass;
}

static int runFxEchoSync() {
    int failures = 0;
    failures += runFxEchoSyncOne(120.0, 0, 24000) ? 0 : 1; // 1/4 at 120 bpm = 0.5 s
    failures += runFxEchoSyncOne(90.0, 3, 8000) ? 0 : 1;   // 1/16 at 90 bpm = 1/6 s
    failures += runFxEchoSyncOne(120.0, 2, 18000) ? 0 : 1; // 1/8 dotted at 120 bpm = 0.375 s
    return failures;
}

//==============================================================================
// Aux node: metronome, backing track, analysis.

// Renders `total` samples of `aux` in blocks cycled from `blocks`, advancing `transport` per block
// as the synth does. Allocations / frees counted inside each block. Left channel into `outL`.
static void renderAux(AuxProcessor& aux, TransportEngine& transport, double sr, int total,
                      const std::vector<int>& blocks, std::vector<float>& outL, std::vector<float>* outR = nullptr,
                      const std::function<void(int)>& beforeBlock = {}) {
    juce::AudioBuffer<float> buf(2, 4096);
    juce::MidiBuffer midi;
    outL.assign((size_t)total, 0.0f);
    if (outR) outR->assign((size_t)total, 0.0f);
    size_t bi = 0;
    for (int done = 0; done < total;) {
        const int n = std::min(blocks[bi++ % blocks.size()], total - done);
        if (beforeBlock) beforeBlock(done);
        buf.setSize(2, n, false, false, true);
        tCountAllocs = true;
        transport.advanceBlock(n, sr);
        aux.processBlock(buf, midi);
        tCountAllocs = false;
        std::memcpy(&outL[(size_t)done], buf.getReadPointer(0), (size_t)n * sizeof(float));
        if (outR) std::memcpy(&(*outR)[(size_t)done], buf.getReadPointer(1), (size_t)n * sizeof(float));
        done += n;
    }
}

// Metronome onsets: every beat's click starts exactly on the first sample whose beat position is
// >= the beat (ceil(k × 60 × sr / bpm)), waveform equal to the pre-rendered click (accent on bar
// beat 1), silence between clicks, L == R; odd block sizes so clicks straddle block boundaries.
static int runMetronome() {
    struct Cfg { double bpm, sr, start; std::vector<int> blocks; };
    const std::vector<Cfg> cfgs = {
        {120.0, 48000.0, 0.0, {471, 127, 1024}},
        {137.0, 48000.0, 0.0, {333, 64}},     // non-integer samples per beat
        {100.0, 44100.0, 0.5, {471}},         // starts mid-beat: first click on beat 1
        {173.0, 48000.0, 2.25, {1, 7, 4096, 129}},
    };
    int failures = 0;
    for (const Cfg& cfg : cfgs) {
        int clicksChecked = 0, mismatches = 0, styleRuns = 0;
        uint64_t allocs = 0, frees = 0;
        for (int style = 0; style < kMetronomeStyleCount; ++style, ++styleRuns) {
            TransportEngine transport;
            transport.setTempo(cfg.bpm);
            transport.setPosition(cfg.start);
            transport.start();
            AuxProcessor aux;
            aux.setTransport(&transport);
            aux.auxSettings().setMetronomeEnabled(true);
            aux.auxSettings().setMetronomeStyle(style);
            aux.auxSettings().setMetronomeLevel(1.0);
            aux.auxSettings().setBeatsPerBar(4);
            aux.setRateAndBufferSizeDetails(cfg.sr, 512);
            aux.prepareToPlay(cfg.sr, 512);
            const double spb = 60.0 * cfg.sr / cfg.bpm;
            const int total = (int)(9 * spb);
            std::vector<float> l, r;
            gRenderAllocs.store(0); gRenderFrees.store(0);
            renderAux(aux, transport, cfg.sr, total, cfg.blocks, l, &r);
            allocs += gRenderAllocs.load(); frees += gRenderFrees.load();
            std::vector<char> covered((size_t)total, 0);
            for (int64_t k = (int64_t)std::ceil(cfg.start); ; ++k) {
                const int64_t onset = (int64_t)std::ceil((k - cfg.start) * spb - 1e-6);
                const std::vector<float>& click = aux.clickBuffer(style, k % 4 == 0);
                if (onset + (int64_t)click.size() >= total) break;
                for (size_t j = 0; j < click.size(); ++j) {
                    covered[(size_t)onset + j] = 1;
                    if (l[(size_t)onset + j] != click[j]) { ++mismatches; break; }
                }
                ++clicksChecked;
            }
            for (int i = 0; i < total; ++i) {
                if (!covered[(size_t)i] && l[(size_t)i] != 0.0f) { ++mismatches; break; }
                if (l[(size_t)i] != r[(size_t)i]) { ++mismatches; break; }
            }
        }
        const bool pass = mismatches == 0 && clicksChecked >= 8 * styleRuns && allocs == 0 && frees == 0;
        failures += pass ? 0 : 1;
        std::printf("met   metronome %.0f bpm @ %.0f Hz from beat %.2f, 5 styles, blocks", cfg.bpm, cfg.sr, cfg.start);
        for (int b : cfg.blocks) std::printf(" %d", b);
        std::printf("  %-4s %d clicks on the beat sample, %d mismatches, allocs %llu frees %llu\n", pass ? "PASS" : "FAIL",
                    clicksChecked, mismatches, (unsigned long long)allocs, (unsigned long long)frees);
        std::fflush(stdout);
    }
    return failures;
}

// A synthetic playable track (distinct values per sample; right = -left) at `rate`.
static std::unique_ptr<BackingTrackBuffer> makeTrack(int length, double rate, double salt = 0.0) {
    auto t = std::make_unique<BackingTrackBuffer>();
    t->sampleRate = rate;
    t->length = length;
    t->audio.setSize(2, length);
    for (int i = 0; i < length; ++i) {
        const float v = 0.4f * (float)std::sin(i * 0.0123 + salt) + 0.001f * (float)(i % 97);
        t->audio.setSample(0, i, v);
        t->audio.setSample(1, i, -v);
    }
    return t;
}

static std::unique_ptr<BackingTrackBuffer> constantTrack(int length, double rate, float value) {
    auto t = std::make_unique<BackingTrackBuffer>();
    t->sampleRate = rate;
    t->length = length;
    t->audio.setSize(2, length);
    for (int ch = 0; ch < 2; ++ch) juce::FloatVectorOperations::fill(t->audio.getWritePointer(ch), value, length);
    return t;
}

// Writes `t` (left channel, mono) as a 24-bit WAV; returns its path.
static std::string writeWav(const juce::AudioBuffer<float>& audio, double rate, const char* name, int channels = 2) {
    const juce::File file = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile(name);
    file.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::FileOutputStream> stream(file.createOutputStream());
    std::unique_ptr<juce::AudioFormatWriter> writer(
        wav.createWriterFor(stream.get(), rate, (unsigned)channels, 24, {}, 0));
    if (writer != nullptr) {
        stream.release(); // the writer owns it now
        writer->writeFromAudioSampleBuffer(audio, 0, audio.getNumSamples());
    }
    return file.getFullPathName().toStdString();
}

// Backing track follows the transport. Track sample 0 = beat 0: plays from the top sample-exact,
// loops, seeks with setTransportPosition (5 ms crossfade, then sample-exact at beat × 60 / bpm),
// starts mid-bar at the beat's position, pause + resume continues where it stopped, no loop →
// silent past the end, follows a device-rate change by decoding the file again.
static bool runBackingTrackPlayback() {
    const double sr = 48000.0;
    const int L = 30000;
    const int fade = (int)(AuxProcessor::kFadeSeconds * sr);
    const std::vector<int> blocks = {471, 127, 1024, 333};
    juce::AudioBuffer<float> ref;
    { auto t = makeTrack(L, sr); ref.makeCopyOf(t->audio); }
    const auto expected = [&](int64_t p) { return ref.getSample(0, (int)(((p % L) + L) % L)); };
    auto install = [&](BackingTrack& holder) { holder.install(makeTrack(L, sr), {}, holder.beginLoad()); };
    auto setup = [&](AuxProcessor& aux, BackingTrack& holder, TransportEngine& transport, double rate) {
        aux.setBackingTrack(&holder);
        aux.setTransport(&transport);
        aux.auxSettings().setTrackLevel(1.0);
        aux.setRateAndBufferSizeDetails(rate, 512);
        aux.prepareToPlay(rate, 512);
    };
    int bad = 0;
    uint64_t allocs = 0, frees = 0;
    std::string detail;

    // 1. loop from the top, then seek to beat 10.25 (= 246000 → 6000 in the loop), then stop.
    {
        BackingTrack holder;
        AuxProcessor aux;
        TransportEngine transport;
        transport.setTempo(120);
        setup(aux, holder, transport, sr);
        install(holder);
        transport.start();
        const int seekAt = 80000, stopAt = 140000, total = 150000;
        int seekBlock = -1, stopBlock = -1;
        std::vector<float> l;
        gRenderAllocs.store(0); gRenderFrees.store(0);
        renderAux(aux, transport, sr, total, blocks, l, nullptr, [&](int done) {
            if (seekBlock < 0 && done >= seekAt) { transport.setPosition(10.25); seekBlock = done; }
            if (stopBlock < 0 && done >= stopAt) { transport.stop(); stopBlock = done; }
        });
        allocs += gRenderAllocs.load(); frees += gRenderFrees.load();
        int b1 = 0, b2 = 0, b3 = 0;
        for (int i = 0; i < seekBlock; ++i) b1 += l[(size_t)i] == expected(i) ? 0 : 1;
        for (int i = seekBlock + fade; i < stopBlock; ++i) b2 += l[(size_t)i] == expected(246000 + (i - seekBlock)) ? 0 : 1;
        for (int i = stopBlock + fade; i < total; ++i) b3 += l[(size_t)i] == 0.0f ? 0 : 1;
        bad += b1 + b2 + b3;
        detail += " loop/seek/stop mismatches " + std::to_string(b1) + "/" + std::to_string(b2) + "/" + std::to_string(b3) + ";";
    }
    // 2. start at beat 5.5 (mid-bar): track at 5.5 beats = 132000 → 12000 in the loop.
    {
        BackingTrack holder;
        AuxProcessor aux;
        TransportEngine transport;
        transport.setTempo(120);
        transport.setPosition(5.5);
        setup(aux, holder, transport, sr);
        install(holder);
        transport.start();
        std::vector<float> l;
        gRenderAllocs.store(0); gRenderFrees.store(0);
        renderAux(aux, transport, sr, 40000, blocks, l);
        allocs += gRenderAllocs.load(); frees += gRenderFrees.load();
        int b = 0;
        for (int i = fade; i < 40000; ++i) b += l[(size_t)i] == expected(132000 + i) ? 0 : 1;
        bad += b;
        detail += " mid-bar start " + std::to_string(b) + ";";
    }
    // 3. pause at 20000 samples, resume 10000 samples later: continues at track sample 20000+.
    {
        BackingTrack holder;
        AuxProcessor aux;
        TransportEngine transport;
        transport.setTempo(120);
        setup(aux, holder, transport, sr);
        install(holder);
        transport.start();
        int pauseBlock = -1, resumeBlock = -1;
        std::vector<float> l;
        gRenderAllocs.store(0); gRenderFrees.store(0);
        renderAux(aux, transport, sr, 60000, blocks, l, nullptr, [&](int done) {
            if (pauseBlock < 0 && done >= 20000) { transport.stop(); pauseBlock = done; }
            if (pauseBlock >= 0 && resumeBlock < 0 && done >= 30000) { transport.start(); resumeBlock = done; }
        });
        allocs += gRenderAllocs.load(); frees += gRenderFrees.load();
        int b = 0;
        for (int i = 0; i < pauseBlock; ++i) b += l[(size_t)i] == expected(i) ? 0 : 1;
        for (int i = pauseBlock + fade; i < resumeBlock; ++i) b += l[(size_t)i] == 0.0f ? 0 : 1;
        for (int i = resumeBlock + fade; i < 60000; ++i) b += l[(size_t)i] == expected(pauseBlock + (i - resumeBlock)) ? 0 : 1;
        bad += b;
        detail += " pause/resume " + std::to_string(b) + ";";
    }
    // 4. no loop: the track plays once, then silence.
    {
        BackingTrack holder;
        AuxProcessor aux;
        TransportEngine transport;
        setup(aux, holder, transport, sr);
        aux.auxSettings().setTrackLoop(false);
        install(holder);
        transport.start();
        std::vector<float> l;
        renderAux(aux, transport, sr, 45000, blocks, l);
        int b = 0;
        for (int i = 0; i < 45000; ++i) b += l[(size_t)i] == (i < L ? expected(i) : 0.0f) ? 0 : 1;
        bad += b;
        detail += " no-loop " + std::to_string(b) + ";";
    }
    // 5. device rate change: loaded from a WAV at 48 kHz, prepared at 44.1 kHz → the listener
    //    decodes the file again at 44.1 kHz (the decoded source is not kept) → it plays.
    bool rateOk = false;
    {
        const std::string path = writeWav(ref, sr, "ejfx-bt-rate.wav");
        BackingTrack holder;
        int reloads = 0;
        holder.setRateListener([&](double) { reloads += holder.reload() ? 1 : 0; }); // the app posts this to a queue
        AuxProcessor aux;
        TransportEngine transport;
        setup(aux, holder, transport, sr);
        BackingTrackSource source;
        source.path = path;
        BackingTrackInfo info;
        std::string error;
        auto buffer = BackingTrack::load(source, holder.wantedSampleRate(), {}, info, error);
        const bool loaded = buffer != nullptr && buffer->sampleRate == sr && buffer->length == L;
        holder.install(std::move(buffer), source, holder.beginLoad());
        aux.setRateAndBufferSizeDetails(44100, 512);
        aux.prepareToPlay(44100, 512);
        const BackingTrackBuffer* after = holder.publishedBuffer();
        rateOk = loaded && reloads == 1 && after != nullptr && after->sampleRate == 44100
              && after->length == (int)std::llround(L * 44100.0 / sr);
        transport.start();
        std::vector<float> l;
        renderAux(aux, transport, 44100, 8000, blocks, l);
        float peak = 0.0f;
        for (float v : l) peak = std::max(peak, std::fabs(v));
        rateOk = rateOk && peak > 0.1f;
        detail += std::string(" rate change ") + (rateOk ? "ok" : "FAILED") + (error.empty() ? "" : " " + error) + ";";
        juce::File(juce::String(path)).deleteFile();
    }
    const bool pass = bad == 0 && rateOk && allocs == 0 && frees == 0;
    std::printf("bt    backing track: loop, seek, mid-bar start, pause/resume, stop, no-loop, rate change  %-4s%s allocs %llu frees %llu\n",
                pass ? "PASS" : "FAIL", detail.c_str(), (unsigned long long)allocs, (unsigned long long)frees);
    std::fflush(stdout);
    return pass;
}

// Tempo changes do not move the track: the bpm ramps 120 → 124 over 2 s while playing (one step
// per block); the output is the track read sample after sample, with no crossfade jump.
static bool runBackingTrackTempoRamp() {
    const double sr = 48000.0;
    const int L = 30000, total = 96000;
    BackingTrack holder;
    juce::AudioBuffer<float> ref;
    { auto t = makeTrack(L, sr); ref.makeCopyOf(t->audio); holder.install(std::move(t), {}, holder.beginLoad()); }
    AuxProcessor aux;
    TransportEngine transport;
    transport.setTempo(120);
    aux.setBackingTrack(&holder);
    aux.setTransport(&transport);
    aux.auxSettings().setTrackLevel(1.0);
    aux.setRateAndBufferSizeDetails(sr, 512);
    aux.prepareToPlay(sr, 512);
    transport.start();
    std::vector<float> l;
    gRenderAllocs.store(0); gRenderFrees.store(0);
    renderAux(aux, transport, sr, total, {471, 127, 1024, 333}, l, nullptr, [&](int done) {
        transport.setTempo(120.0 + 4.0 * std::min(1.0, done / 96000.0));
    });
    int mismatches = 0;
    for (int i = 0; i < total; ++i) mismatches += l[(size_t)i] == ref.getSample(0, i % L) ? 0 : 1;
    const uint64_t allocs = gRenderAllocs.load() + gRenderFrees.load();
    const bool pass = mismatches == 0 && transport.getTempo() > 123.9 && allocs == 0;
    std::printf("btt   backing track under a tempo ramp 120 -> 124 bpm while playing  %-4s %d samples off the continuous track"
                " (of %d), allocs %llu\n", pass ? "PASS" : "FAIL", mismatches, total, (unsigned long long)allocs);
    std::fflush(stdout);
    return pass;
}

// Swaps are declicked: while a track of constant +0.5 plays, a track of constant -0.5 replaces
// it (crossfade, never a step larger than one fade increment), then an unload fades it out. The
// old buffer stays alive while it fades out (collect() keeps it), and is freed afterwards.
static bool runBackingTrackSwapFade() {
    const double sr = 48000.0;
    const int fade = (int)(AuxProcessor::kFadeSeconds * sr);
    BackingTrack holder;
    AuxProcessor aux;
    TransportEngine transport;
    aux.setBackingTrack(&holder);
    aux.setTransport(&transport);
    aux.auxSettings().setTrackLevel(1.0);
    aux.setRateAndBufferSizeDetails(sr, 512);
    aux.prepareToPlay(sr, 512);
    holder.install(constantTrack(20000, sr, 0.5f), {}, holder.beginLoad());
    transport.start();
    int pendingDuringFade = -2, pendingAfterFade = -2, liveDuringFade = 0;
    std::vector<float> l;
    const int swapAt = 10000, unloadAt = 20000, total = 26000;
    int swapBlock = -1, unloadBlock = -1;
    gRenderAllocs.store(0); gRenderFrees.store(0);
    uint64_t allocs = 0;
    renderAux(aux, transport, sr, total, {100}, l, nullptr, [&](int done) {
        allocs += gRenderAllocs.exchange(0) + gRenderFrees.exchange(0);
        if (swapBlock < 0 && done >= swapAt) { holder.install(constantTrack(20000, sr, -0.5f), {}, holder.beginLoad()); swapBlock = done; }
        if (swapBlock >= 0 && done == swapBlock + 100) { pendingDuringFade = holder.collect(); liveDuringFade = BackingTrackBuffer::live.load(); }
        if (swapBlock >= 0 && done == swapBlock + 1000) pendingAfterFade = holder.collect();
        if (unloadBlock < 0 && done >= unloadAt) { holder.unload(); unloadBlock = done; }
    });
    allocs += gRenderAllocs.load() + gRenderFrees.load();
    float maxStep = 0.0f;
    for (int i = 1; i < total; ++i) maxStep = std::max(maxStep, std::fabs(l[(size_t)i] - l[(size_t)i - 1]));
    int wrong = 0;
    for (int i = 0; i < swapBlock; ++i) wrong += l[(size_t)i] == 0.5f ? 0 : 1;
    for (int i = swapBlock + fade + 1; i < unloadBlock; ++i) wrong += l[(size_t)i] == -0.5f ? 0 : 1;
    for (int i = unloadBlock + fade + 1; i < total; ++i) wrong += l[(size_t)i] == 0.0f ? 0 : 1;
    const int pendingEnd = holder.collect();
    const int liveEnd = BackingTrackBuffer::live.load();
    const float maxAllowed = 1.0f / (float)fade + 1e-4f; // one crossfade increment (0.5 → -0.5)
    const bool pass = maxStep <= maxAllowed && wrong == 0 && pendingDuringFade == 1 && liveDuringFade == 2
                   && pendingAfterFade == 0 && pendingEnd == 0 && liveEnd == 0 && allocs == 0;
    std::printf("bsf   swap + unload while playing are crossfaded; old buffer kept during its fade  %-4s max step %.5f"
                " (allowed %.5f), %d wrong samples, retired during fade %d (live %d), after %d, end %d (live %d), allocs %llu\n",
                pass ? "PASS" : "FAIL", maxStep, maxAllowed, wrong, pendingDuringFade, liveDuringFade, pendingAfterFade,
                pendingEnd, liveEnd, (unsigned long long)allocs);
    std::fflush(stdout);
    return pass;
}

// Loop region from the MP3 fixture (44.1 kHz stereo, LAME/Xing gapless tag) with the JUCE MP3
// decoder (the harness has no CoreAudio): which length it yields and which rule picks the region,
// then the exact loop resampled to 48 kHz. Also the rule itself on all its cases.
static bool runBackingTrackRegion() {
    const BackingTrackGapless g{682839, 4561, 1496};
    const BackingTrackRegion honoured = backingTrackRegion(682839, &g);
    const BackingTrackRegion ignored = backingTrackRegion(1152 + 4561 + 682839 + 1496, &g);
    const BackingTrackRegion other = backingTrackRegion(4561 + 682839 + 1496, &g);
    const BackingTrackRegion odd = backingTrackRegion(700000, &g);
    const BackingTrackRegion none = backingTrackRegion(5000, nullptr);
    const bool ruleOk = honoured.start == 0 && honoured.length == 682839 && honoured.rule == BackingTrackRegionRule::Tag
        && ignored.start == 1152 + 4561 && ignored.length == 682839 && ignored.rule == BackingTrackRegionRule::Padded
        && other.start == 4561 && other.length == 682839 && other.rule == BackingTrackRegionRule::PaddedNoTagFrame
        && odd.start == 0 && odd.length == 700000 && odd.rule == BackingTrackRegionRule::Whole
        && none.start == 0 && none.length == 5000 && none.rule == BackingTrackRegionRule::Whole;

    BackingTrackSource source;
    source.path = std::string(HARNESS_FIXTURES) + "/loop-124bpm-44k.mp3";
    source.hasGapless = true;
    source.gapless = g;
    std::string error;
    BackingTrackInfo info;
    auto atSource = BackingTrack::load(source, 0.0, {}, info, error);
    bool decodeOk = atSource != nullptr;
    int64_t resampled = 0;
    const int64_t expectedResampled = 743226; // round(682839 × 48000 / 44100 = 743226.12): the exact loop
    if (decodeOk) {
        const int64_t d = info.decodedSamples;
        decodeOk = info.sampleRate == 44100.0 && info.channels == 2 && info.loopStartSample == 4561
                && info.loopSamples == 682839 && atSource->length == 682839 && atSource->sampleRate == 44100.0
                && info.regionRule == BackingTrackRegionRule::PaddedNoTagFrame;
        const float* x = atSource->audio.getReadPointer(0);
        const int n = atSource->length;
        double meanStep = 0.0;
        for (int i = 1; i < n; ++i) meanStep += std::fabs(x[i] - x[i - 1]);
        meanStep /= std::max(1, n - 1);
        std::printf("btr   MP3 fixture via %s: decoded %lld samples (loop %lld + lead %lld + trail %lld) -> rule '%s';"
                    " region [%lld, +%lld); seam |x[end]-x[0]| %.4f vs mean step %.4f\n",
                    info.formatName.c_str(), (long long)d, (long long)g.loopSamples, (long long)g.leadingSamples,
                    (long long)g.trailingSamples, backingTrackRegionRuleName(info.regionRule),
                    (long long)info.loopStartSample, (long long)info.loopSamples, std::fabs(x[n - 1] - x[0]), meanStep);
        atSource.reset();
        BackingTrackInfo info48;
        auto at48 = BackingTrack::load(source, 48000.0, {}, info48, error);
        resampled = at48 != nullptr ? at48->length : -1;
        decodeOk = decodeOk && at48 != nullptr && resampled == expectedResampled && at48->sampleRate == 48000.0
                && at48->audio.getNumSamples() == resampled && info48.playbackFrames == resampled;
    } else {
        std::printf("btr   MP3 fixture decode failed: %s\n", error.c_str());
    }
    const bool pass = ruleOk && decodeOk;
    std::printf("btr   loop-region rule (4 cases) + fixture decode + exact loop resampled 44.1k->48k  %-4s rule %d,"
                " decode %d, resampled %lld (expect %lld)\n", pass ? "PASS" : "FAIL", ruleOk, decodeOk,
                (long long)resampled, (long long)expectedResampled);
    std::fflush(stdout);
    return pass;
}

// Limits: a file longer than the frame cap (6 min at 48 kHz, scaled with the rate) is rejected
// with ERR_BACKING_TRACK_TOO_LONG before any big allocation; with too little memory available
// (the provider reports 1 MB) ERR_BACKING_TRACK_MEMORY; nothing stays allocated.
static bool runBackingTrackLimits() {
    const double rate = 8000.0;
    const int frames = (int)(BackingTrack::maxFrames(rate) + rate); // 1 s over the cap
    juce::AudioBuffer<float> longAudio(1, frames);
    longAudio.clear();
    const std::string longPath = writeWav(longAudio, rate, "ejfx-bt-long.wav", 1);
    BackingTrackSource longSource;
    longSource.path = longPath;
    BackingTrackInfo info;
    std::string tooLong;
    const bool rejectedLong = BackingTrack::load(longSource, 48000.0, {}, info, tooLong) == nullptr
                           && tooLong.rfind("ERR_BACKING_TRACK_TOO_LONG", 0) == 0;
    juce::File(juce::String(longPath)).deleteFile();

    BackingTrackSource mp3;
    mp3.path = std::string(HARNESS_FIXTURES) + "/loop-124bpm-44k.mp3";
    BackingTrack::setAvailableMemoryProvider([] { return (int64_t)1024 * 1024; });
    std::string noMemory;
    const bool rejectedMemory = BackingTrack::load(mp3, 48000.0, {}, info, noMemory) == nullptr
                             && noMemory.rfind("ERR_BACKING_TRACK_MEMORY", 0) == 0;
    BackingTrack::setAvailableMemoryProvider(nullptr);
    const int live = BackingTrackBuffer::live.load();
    const bool pass = rejectedLong && rejectedMemory && live == 0;
    std::printf("btl   frame cap + available-memory check  %-4s too long: \"%s\"; low memory: \"%s\"; live %d\n",
                pass ? "PASS" : "FAIL", tooLong.c_str(), noMemory.c_str(), live);
    std::fflush(stdout);
    return pass;
}

// Swap stress: an audio thread renders the aux node non-stop (transport playing) while this
// thread loads / unloads 300 tracks (44.1 kHz → resampled to 48 kHz) and collects retired buffers.
// The render never allocates or frees; every replaced buffer is freed here; nothing leaks.
static bool runBackingTrackSwapStress() {
    BackingTrack holder;
    TransportEngine transport;
    transport.setTempo(128);
    transport.start();
    std::atomic<bool> stopAudio{false};
    std::atomic<uint64_t> blocksRendered{0};
    std::atomic<int> nonFinite{0};
    uint64_t allocs = 0, frees = 0;
    int pendingMax = 0;
    {
        AuxProcessor aux;
        aux.setBackingTrack(&holder);
        aux.setTransport(&transport);
        aux.auxSettings().setMetronomeEnabled(true);
        aux.setRateAndBufferSizeDetails(48000, 512);
        aux.prepareToPlay(48000, 512);
        gRenderAllocs.store(0); gRenderFrees.store(0);
        std::thread audio([&] {
            juce::AudioBuffer<float> buf(2, 1024);
            juce::MidiBuffer midi;
            const int sizes[3] = {128, 471, 1024};
            for (uint64_t b = 0; !stopAudio.load(); ++b) {
                const int n = sizes[b % 3];
                buf.setSize(2, n, false, false, true);
                tCountAllocs = true;
                transport.advanceBlock(n, 48000);
                aux.processBlock(buf, midi);
                tCountAllocs = false;
                for (int i = 0; i < n; ++i) if (!std::isfinite(buf.getSample(0, i))) nonFinite.fetch_add(1);
                blocksRendered.fetch_add(1);
            }
        });
        for (int i = 0; i < 300; ++i) {
            if (i % 7 == 6) {
                holder.unload();
            } else {
                auto src = makeTrack(2000 + 37 * i, 44100.0, i);
                holder.install(BackingTrack::resample(src->audio, 44100.0, 48000.0), {}, holder.beginLoad());
            }
            if (i % 3 == 0) std::this_thread::sleep_for(std::chrono::microseconds(300));
            pendingMax = std::max(pendingMax, holder.collect());
        }
        // A load superseded before it installs must not publish.
        const uint64_t stale = holder.beginLoad();
        holder.beginLoad();
        const bool staleRejected = !holder.install(makeTrack(1000, 48000.0), {}, stale);
        if (!staleRejected) pendingMax = 1000;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        stopAudio.store(true);
        audio.join();
        allocs = gRenderAllocs.load();
        frees = gRenderFrees.load();
    } // aux destroyed: releases the reader
    holder.collect();
    const int liveWithTrack = BackingTrackBuffer::live.load();
    holder.unload();
    const int pendingAfter = holder.collect();
    const int liveAfter = BackingTrackBuffer::live.load();
    const bool pass = allocs == 0 && frees == 0 && nonFinite.load() == 0 && liveWithTrack <= 1 && liveAfter == 0
                   && pendingAfter == 0 && pendingMax < 1000 && blocksRendered.load() > 50;
    std::printf("bts   swap stress: 300 load/unload while rendering (%llu blocks)  %-4s render allocs %llu frees %llu,"
                " max retired waiting %d, live after unload %d, superseded load rejected %d\n",
                (unsigned long long)blocksRendered.load(), pass ? "PASS" : "FAIL", (unsigned long long)allocs,
                (unsigned long long)frees, pendingMax, liveAfter, pendingMax < 1000);
    std::fflush(stdout);
    return pass;
}

// Aux with nothing to play adds exactly 0: its output is all zeros (transport playing, metronome
// off, track loaded but disabled), and a graph with the aux node renders the same values as one
// without it.
static bool runAuxSilent() {
    bool zeros = true;
    {
        BackingTrack holder;
        TransportEngine transport;
        transport.start();
        AuxProcessor aux;
        aux.setBackingTrack(&holder);
        aux.setTransport(&transport);
        aux.auxSettings().setTrackEnabled(false);
        aux.setRateAndBufferSizeDetails(48000, 512);
        aux.prepareToPlay(48000, 512);
        holder.install(makeTrack(5000, 48000), {}, holder.beginLoad());
        std::vector<float> l, r;
        renderAux(aux, transport, 48000, 20000, {471, 64}, l, &r);
        for (int i = 0; i < 20000; ++i) zeros = zeros && l[(size_t)i] == 0.0f && r[(size_t)i] == 0.0f;
    }
    int differ = 0;
    {
        auto build = [](bool withAux, TransportEngine& t, FxParamStore& fs, AuxSettingsStore& as, BackingTrack& bt,
                        PolySynthProcessor*& synthOut) {
            auto* g = new juce::AudioProcessorGraph();
            g->setPlayConfigDetails(0, 2, 48000, 256);
            auto s = std::make_unique<PolySynthProcessor>();
            synthOut = s.get();
            s->setTransport(&t);
            auto sn = g->addNode(std::move(s));
            auto f = std::make_unique<FxChainProcessor>();
            f->setParamStore(&fs); f->setTransport(&t);
            auto fn = g->addNode(std::move(f));
            auto on = g->addNode(std::make_unique<juce::AudioProcessorGraph::AudioGraphIOProcessor>(
                juce::AudioProcessorGraph::AudioGraphIOProcessor::audioOutputNode));
            for (int ch = 0; ch < 2; ++ch) {
                g->addConnection({{sn->nodeID, ch}, {fn->nodeID, ch}});
                g->addConnection({{fn->nodeID, ch}, {on->nodeID, ch}});
            }
            if (withAux) {
                auto a = std::make_unique<AuxProcessor>();
                a->setSettings(&as); a->setBackingTrack(&bt); a->setTransport(&t);
                auto an = g->addNode(std::move(a));
                for (int ch = 0; ch < 2; ++ch) g->addConnection({{an->nodeID, ch}, {on->nodeID, ch}});
            }
            g->prepareToPlay(48000, 256);
            return g;
        };
        TransportEngine ta, tb;
        FxParamStore fa, fb;
        AuxSettingsStore aa, ab;
        BackingTrack ba, bb;
        PolySynthProcessor *sa = nullptr, *sb = nullptr;
        juce::AudioProcessorGraph* ga = build(true, ta, fa, aa, ba, sa);
        juce::AudioProcessorGraph* gb = build(false, tb, fb, ab, bb, sb);
        ta.start(); tb.start();
        sa->noteOn(60, 110); sb->noteOn(60, 110);
        juce::AudioBuffer<float> xa(2, 256), xb(2, 256);
        juce::MidiBuffer midi;
        float peak = 0.0f;
        for (int blk = 0; blk < 200; ++blk) {
            xa.clear(); xb.clear();
            ga->processBlock(xa, midi);
            gb->processBlock(xb, midi);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 256; ++i) {
                    differ += xa.getSample(ch, i) == xb.getSample(ch, i) ? 0 : 1;
                    peak = std::max(peak, std::fabs(xa.getSample(ch, i)));
                }
        }
        if (peak < 1e-3f) differ += 1000000; // the comparison must be over real signal
        delete ga;
        delete gb;
    }
    const bool pass = zeros && differ == 0;
    std::printf("aux0  aux with nothing to play adds exactly 0  %-4s aux output all zero %d; graph with aux vs without:"
                " %d differing samples\n", pass ? "PASS" : "FAIL", zeros, differ);
    std::fflush(stdout);
    return pass;
}

// Aux settings and the backing track survive a rebuild: settings round-trip through
// snapshot / apply and two processors render the metronome identically; a new processor on
// the same (process-lifetime) holder plays the track the old one played.
static bool runAuxRebuildState() {
    AuxSettingsStore a;
    a.setMetronomeEnabled(true);
    bool apiOk = a.setMetronomeStyle("cowbell") && !a.setMetronomeStyle("nope");
    a.setMetronomeLevel(1.7);   // clamped to 1
    a.setMetronomeAccent(false);
    a.setBeatsPerBar(3.4);      // rounded to 3
    a.setTrackLevel(0.25);
    a.setTrackLoop(false);
    a.setTrackEnabled(false);
    const AuxSettings s = a.snapshot();
    apiOk = apiOk && s.metronomeStyle == kMetronomeCowbell && s.metronomeLevel == 1.0f && s.beatsPerBar == 3
         && !s.metronomeAccent && s.trackLevel == 0.25f && !s.trackLoop && !s.trackEnabled && !(s == AuxSettings::defaults());
    AuxSettingsStore b;
    b.apply(s);
    const bool stateOk = apiOk && b.snapshot() == s;
    bool renderOk = true;
    {
        TransportEngine ta, tb;
        ta.start(); tb.start();
        AuxProcessor pa, pb;
        pa.setSettings(&a); pa.setTransport(&ta);
        pb.setSettings(&b); pb.setTransport(&tb);
        for (AuxProcessor* p : {&pa, &pb}) { p->setRateAndBufferSizeDetails(48000, 512); p->prepareToPlay(48000, 512); }
        std::vector<float> la, lb;
        renderAux(pa, ta, 48000, 60000, {471}, la);
        renderAux(pb, tb, 48000, 60000, {471}, lb);
        float peak = 0.0f;
        for (float v : la) peak = std::max(peak, std::fabs(v));
        renderOk = la == lb && peak > 0.1f;
    }
    bool trackOk = false;
    {
        BackingTrack holder; // process-lifetime in the engine
        TransportEngine t;
        t.start();
        std::vector<float> l1, l2;
        {
            AuxProcessor oldAux;
            oldAux.setBackingTrack(&holder); oldAux.setTransport(&t);
            oldAux.setRateAndBufferSizeDetails(48000, 512); oldAux.prepareToPlay(48000, 512);
            holder.install(makeTrack(20000, 48000), {}, holder.beginLoad());
            renderAux(oldAux, t, 48000, 5000, {471}, l1);
        }
        const int live = BackingTrackBuffer::live.load();
        AuxProcessor newAux;
        newAux.setBackingTrack(&holder); newAux.setTransport(&t);
        newAux.setRateAndBufferSizeDetails(48000, 512); newAux.prepareToPlay(48000, 512);
        renderAux(newAux, t, 48000, 5000, {471}, l2);
        float peak = 0.0f;
        for (float v : l2) peak = std::max(peak, std::fabs(v));
        trackOk = live == 1 && peak > 0.1f && holder.hasTrack();
    }
    const bool pass = stateOk && renderOk && trackOk;
    std::printf("rsa   aux rebuild carry-over (metronome + track options, track holder)  %-4s state %d render %d track %d\n",
                pass ? "PASS" : "FAIL", stateOk, renderOk, trackOk);
    std::fflush(stdout);
    return pass;
}

// getAnalysis: GraphPlayer writes the device output into the AnalysisRing; with a known sine the
// scope points are the sine at the expected sample indices (stride 16, newest last); peak / rms
// cover everything written since the previous read (then 0 with nothing new); a mono write
// counts each sample once; the aux node publishes the transport; the callback does not allocate.
class SineSource : public juce::AudioProcessor {
public:
    SineSource() : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)) {}
    double index = 0.0;
    static float value(double i) { return 0.5f * (float)std::sin(juce::MathConstants<double>::twoPi * i / 400.0); }
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& b, juce::MidiBuffer&) override {
        for (int i = 0; i < b.getNumSamples(); ++i, index += 1.0)
            for (int ch = 0; ch < b.getNumChannels(); ++ch) b.setSample(ch, i, value(index));
    }
    const juce::String getName() const override { return "Sine"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}
};

static bool runAnalysis() {
    SineSource sine;
    AnalysisRing ring;
    GraphPlayer player;
    player.setProcessor(&sine);
    player.setAnalysis(&ring);
    FakeDevice device(48000, 256);
    player.audioDeviceAboutToStart(&device);
    juce::AudioBuffer<float> out(2, 1024);
    juce::AudioIODeviceCallbackContext ctx{};
    gRenderAllocs.store(0); gRenderFrees.store(0);
    int total = 0;
    const int sizes[3] = {256, 471, 100};
    for (int b = 0; b < 30; ++b) {
        const int n = sizes[b % 3];
        out.clear();
        tCountAllocs = true;
        player.audioDeviceIOCallbackWithContext(nullptr, 0, out.getArrayOfWritePointers(), 2, n, ctx);
        tCountAllocs = false;
        total += n;
    }
    float scope[AnalysisRing::kScopePoints];
    ring.readScope(scope);
    double maxErr = 0.0;
    for (int j = 0; j < AnalysisRing::kScopePoints; ++j) {
        const double idx = (double)(total - 1 - (AnalysisRing::kScopePoints - 1 - j) * 16);
        maxErr = std::max(maxErr, (double)std::fabs(scope[j] - SineSource::value(idx)));
    }
    // Everything written so far (one read covers it).
    float wantPeak = 0.0f;
    double sum = 0.0;
    for (int i = 0; i < total; ++i) {
        const float v = SineSource::value(i);
        wantPeak = std::max(wantPeak, std::fabs(v));
        sum += (double)v * v;
    }
    const double wantRms = std::sqrt(sum / total);
    const AnalysisRing::Levels first = ring.readLevels();
    const AnalysisRing::Levels empty = ring.readLevels();
    const bool levelsOk = std::fabs(first.peak - wantPeak) < 1e-6 && std::fabs(first.rms - wantRms) < 1e-5
                       && empty.peak == 0.0f && empty.rms == 0.0f;
    // Mono: a constant 0.5 has an RMS of 0.5 (each sample counted once).
    AnalysisRing mono;
    float half[64];
    std::fill(std::begin(half), std::end(half), 0.5f);
    const float* monoChannels[1] = { half };
    mono.write(monoChannels, 1, 64);
    const AnalysisRing::Levels monoLevels = mono.readLevels();
    const bool monoOk = std::fabs(monoLevels.rms - 0.5f) < 1e-6 && monoLevels.peak == 0.5f;
    // The aux node publishes the transport at the end of each block.
    AnalysisRing published;
    TransportEngine transport;
    transport.setTempo(97);
    transport.start();
    AuxProcessor aux;
    aux.setTransport(&transport);
    aux.setAnalysis(&published);
    aux.setRateAndBufferSizeDetails(48000, 512);
    aux.prepareToPlay(48000, 512);
    std::vector<float> l;
    renderAux(aux, transport, 48000, 4800, {480}, l);
    const bool transportOk = published.playing() && published.bpm() == 97.0
                          && std::fabs(published.beat() - transport.getPosition()) < 1e-9;
    const bool pass = maxErr < 1e-6 && levelsOk && monoOk && transportOk && ring.samplesWritten() == (uint32_t)total
                   && gRenderAllocs.load() == 0 && gRenderFrees.load() == 0;
    std::printf("an    analysis ring: scope of a known sine, peak / rms since the last read, mono rms, transport  %-4s"
                " scope max err %.2g, peak %.4f (want %.4f), rms %.4f (want %.4f), then %.1f/%.1f, mono rms %.4f,"
                " transport %d, allocs %llu\n", pass ? "PASS" : "FAIL", maxErr, first.peak, wantPeak, first.rms, wantRms,
                empty.peak, empty.rms, monoLevels.rms, transportOk, (unsigned long long)gRenderAllocs.load());
    std::fflush(stdout);
    return pass;
}

//==============================================================================
// Arpeggiator (ios/Arpeggiator.h, pure C++), standalone with a recording sink.

struct ArpRec { int64_t at; int note; int velocity; bool on; };

// Runs `arp` over `total` samples (48 kHz) in blocks cycled from `blocks`; `before(done, bpm)` may
// change the tempo or the arp before each block. Events are recorded with absolute sample
// positions. Every process call runs with the allocation counter on (added to `allocs`).
static std::vector<ArpRec> runArpBlocks(Arpeggiator& arp, int total, const std::vector<int>& blocks, double bpm,
                                        bool playing, double startBeat, uint64_t& allocs,
                                        const std::function<void(int, double&)>& before = {}) {
    const double sr = 48000.0;
    std::vector<ArpRec> out;
    out.reserve(8192);
    size_t bi = 0;
    double beat = startBeat;
    for (int done = 0; done < total;) {
        const int n = std::min(blocks[bi++ % blocks.size()], total - done);
        if (before) before(done, bpm);
        gRenderAllocs.store(0); gRenderFrees.store(0);
        tCountAllocs = true;
        arp.process(beat, bpm, playing, sr, n, [&](const ArpEvent& e) {
            if (out.size() < out.capacity()) out.push_back({(int64_t)done + e.sampleOffset, e.note, e.velocity, e.on});
        });
        tCountAllocs = false;
        allocs += gRenderAllocs.load() + gRenderFrees.load();
        if (playing) beat += n * bpm / (60.0 * sr);
        done += n;
    }
    return out;
}

static std::vector<int64_t> arpOnTimes(const std::vector<ArpRec>& r, int note = -1) {
    std::vector<int64_t> t;
    for (const ArpRec& e : r) if (e.on && (note < 0 || e.note == note)) t.push_back(e.at);
    return t;
}
static std::vector<int64_t> arpOffTimes(const std::vector<ArpRec>& r, int note = -1) {
    std::vector<int64_t> t;
    for (const ArpRec& e : r) if (!e.on && (note < 0 || e.note == note)) t.push_back(e.at);
    return t;
}
static std::string arpList(const std::vector<int64_t>& v, size_t max = 10) {
    std::string s;
    for (size_t i = 0; i < v.size() && i < max; ++i) s += (i ? "," : "") + std::to_string(v[i]);
    return s.empty() ? "-" : s;
}

static ArpSettings arpBase() {
    ArpSettings s = ArpSettings::defaults();
    s.enabled = true;
    s.rate = ArpRate::Sixteenth; // 120 bpm: a step every 6000 samples at 48 kHz
    return s;
}

// Spec 1.2 vectors (pattern order; "-" = rest), then timing (exact offsets over odd block sizes,
// gate, swing, ratchet, 1/8D), the playing-transport start rule (beat 0.3 → step 0 at once and
// step 1 on the 0.5 line; beat 0.4 → step 0 on the 0.5 line), latch (plays on after release,
// the next press replaces the list), disable (an off for every sounding note in that block),
// a tap released before the first step (nothing, list empty), allNotesOff (stops, clears the
// latched list), a tempo change mid-run (the next step re-derived from the new bpm, none skipped
// or doubled), pattern bits beyond `length` ignored, 0 allocations in every process call.
static bool runArp() {
    const std::vector<int> b256 = {256};
    uint64_t allocs = 0;
    int failures = 0;
    std::string detail;
    auto check = [&](bool ok, const std::string& what) {
        if (!ok) { ++failures; detail += " [" + what + "]"; }
    };
    auto settle = [&](Arpeggiator& arp, const ArpSettings& s, std::initializer_list<int> notes) {
        arp.setSettings(s);
        uint64_t seq = 1;
        for (int n : notes) arp.noteOn(n, 100, seq++);
    };
    // The note (or -1 = rest) started on each of the first `count` steps, stopped transport.
    auto firstSteps = [&](const ArpSettings& s, std::initializer_list<int> notes, int count) {
        Arpeggiator arp;
        settle(arp, s, notes);
        const auto rec = runArpBlocks(arp, 6000 * count + 100, b256, 120.0, false, 0.0, allocs);
        std::vector<int> steps((size_t)count, -1);
        bool misaligned = false;
        for (const ArpRec& e : rec) {
            if (!e.on) continue;
            if (e.at % 6000 != 0) { misaligned = true; continue; }
            const int k = (int)(e.at / 6000);
            if (k < count) steps[(size_t)k] = e.note;
        }
        if (misaligned) steps.push_back(-99);
        return steps;
    };
    auto vec = [&](const char* name, const ArpSettings& s, std::initializer_list<int> notes, std::vector<int> want) {
        const std::vector<int> got = firstSteps(s, notes, 8);
        std::string g;
        for (int n : got) g += std::to_string(n) + " ";
        check(got == want, std::string("vector ") + name + ": " + g);
    };
    {
        ArpSettings s = arpBase();
        vec("up", s, {60, 64, 67}, {60, 64, 67, 60, 64, 67, 60, 64});
        s = arpBase(); s.mode = ArpMode::Down; s.octaves = 2;
        vec("down oct2", s, {60, 64, 67}, {79, 76, 72, 67, 64, 60, 79, 76});
        s = arpBase(); s.mode = ArpMode::Played;
        vec("played", s, {67, 60, 64}, {67, 60, 64, 67, 60, 64, 67, 60});
        s = arpBase(); s.mode = ArpMode::UpDown; s.octaves = 2;
        vec("upDown oct2", s, {60, 64}, {60, 64, 72, 76, 72, 64, 60, 64});
        s = arpBase(); s.patternMask = 0b1101; s.patternLength = 4;
        vec("pattern on off on on", s, {60, 64, 67}, {60, -1, 64, 67, 60, -1, 64, 67});
        s = arpBase(); s.patternLength = 2; s.transposeSteps = 2; s.transposeDistance = 5;
        vec("transpose", s, {60}, {60, 60, 65, 65, 70, 70, 60, 60});
        // Single-note edge cases of upDown, and the mask beyond `length` (bits 2.. set, length 2).
        s = arpBase(); s.mode = ArpMode::UpDown;
        vec("upDown one note", s, {60}, {60, 60, 60, 60, 60, 60, 60, 60});
        s = arpBase(); s.mode = ArpMode::UpDown;
        vec("upDown two notes", s, {60, 64}, {60, 64, 60, 64, 60, 64, 60, 64});
        s = arpBase(); s.patternMask = 0xFFFD; s.patternLength = 2;
        vec("mask beyond length", s, {60}, {60, -1, 60, -1, 60, -1, 60, -1});
        // Random: membership in the up cycle only; chord: every held note on every step.
        s = arpBase(); s.mode = ArpMode::Random; s.octaves = 2;
        {
            Arpeggiator arp;
            settle(arp, s, {60, 64});
            const auto rec = runArpBlocks(arp, 48100, b256, 120.0, false, 0.0, allocs);
            bool member = true;
            int ons = 0;
            for (const ArpRec& e : rec)
                if (e.on) { ++ons; member = member && (e.note == 60 || e.note == 64 || e.note == 72 || e.note == 76); }
            check(member && ons == 9, "random membership (" + std::to_string(ons) + " ons)");
        }
        s = arpBase(); s.mode = ArpMode::Chord;
        {
            Arpeggiator arp;
            arp.setSettings(s);
            arp.noteOn(60, 100, 1); arp.noteOn(64, 90, 2); arp.noteOn(67, 80, 3);
            const auto rec = runArpBlocks(arp, 12100, b256, 120.0, false, 0.0, allocs);
            const auto t60 = arpOnTimes(rec, 60), t64 = arpOnTimes(rec, 64), t67 = arpOnTimes(rec, 67);
            const std::vector<int64_t> want = {0, 6000, 12000};
            int vel64 = -1;
            for (const ArpRec& e : rec) if (e.on && e.note == 64 && e.at == 0) vel64 = e.velocity;
            check(t60 == want && t64 == want && t67 == want && vel64 == 90, "chord: " + arpList(t60) + " / " + arpList(t64));
        }
    }
    // Timing: exact grid over odd block sizes; gate 0.6 → off 3600 after each on.
    {
        Arpeggiator arp;
        ArpSettings s = arpBase(); s.gate = 0.6f;
        settle(arp, s, {60});
        const auto rec = runArpBlocks(arp, 48100, kFxBlocks, 120.0, false, 0.0, allocs);
        const auto on = arpOnTimes(rec), off = arpOffTimes(rec);
        std::vector<int64_t> wantOn, wantOff;
        for (int k = 0; k <= 8; ++k) { wantOn.push_back(6000 * k); if (k < 8) wantOff.push_back(6000 * k + 3600); }
        check(on == wantOn, "grid ons " + arpList(on));
        check(off == wantOff, "gate offs " + arpList(off));
        int vel = -1;
        for (const ArpRec& e : rec) if (e.on) vel = e.velocity;
        check(vel == 100, "velocity passthrough");
    }
    // Accent 0.5: steps other than slot 0 at velocity × 0.7.
    {
        Arpeggiator arp;
        ArpSettings s = arpBase(); s.accent = 0.5f; s.patternLength = 4;
        settle(arp, s, {60});
        const auto rec = runArpBlocks(arp, 12100, b256, 120.0, false, 0.0, allocs);
        int v0 = -1, v1 = -1;
        for (const ArpRec& e : rec) { if (e.on && e.at == 0) v0 = e.velocity; if (e.on && e.at == 6000) v1 = e.velocity; }
        check(v0 == 100 && v1 == 70, "accent velocities " + std::to_string(v0) + "/" + std::to_string(v1));
    }
    // Swing 0.25: odd steps 1500 later.
    {
        Arpeggiator arp;
        ArpSettings s = arpBase(); s.swing = 0.25f;
        settle(arp, s, {60});
        const auto on = arpOnTimes(runArpBlocks(arp, 24100, kFxBlocks, 120.0, false, 0.0, allocs));
        check(on == std::vector<int64_t>{0, 7500, 12000, 19500, 24000}, "swing " + arpList(on));
    }
    // Ratchet 3, gate 0.6: hits at 0 / 2000 / 4000 of each step, each 1200 long.
    {
        Arpeggiator arp;
        ArpSettings s = arpBase(); s.ratchet = 3; s.gate = 0.6f;
        settle(arp, s, {60});
        const auto rec = runArpBlocks(arp, 12100, kFxBlocks, 120.0, false, 0.0, allocs);
        const auto on = arpOnTimes(rec), off = arpOffTimes(rec);
        check(on == std::vector<int64_t>{0, 2000, 4000, 6000, 8000, 10000, 12000}, "ratchet ons " + arpList(on));
        check(off == std::vector<int64_t>{1200, 3200, 5200, 7200, 9200, 11200}, "ratchet offs " + arpList(off));
    }
    // 1/8D at 120 bpm: 18000 samples a step; 1/8T: 8000.
    {
        Arpeggiator arp;
        ArpSettings s = arpBase(); s.rate = ArpRate::EighthD;
        settle(arp, s, {60});
        const auto on = arpOnTimes(runArpBlocks(arp, 36100, kFxBlocks, 120.0, false, 0.0, allocs));
        check(on == std::vector<int64_t>{0, 18000, 36000}, "1/8D " + arpList(on));
        Arpeggiator arp2;
        s.rate = ArpRate::EighthT;
        settle(arp2, s, {60});
        const auto on2 = arpOnTimes(runArpBlocks(arp2, 16100, kFxBlocks, 120.0, false, 0.0, allocs));
        check(on2 == std::vector<int64_t>{0, 8000, 16000}, "1/8T " + arpList(on2));
    }
    // Playing transport. From beat 0.3 (0.2 step after the 0.25 line): step 0 at once, step 1 on
    // the 0.5 line = 0.2 beat = 4800 samples later. From beat 0.4: step 0 on the 0.5 line (2400).
    {
        Arpeggiator arp;
        settle(arp, arpBase(), {60});
        const auto on = arpOnTimes(runArpBlocks(arp, 17000, kFxBlocks, 120.0, true, 0.3, allocs));
        check(on == std::vector<int64_t>{0, 4800, 10800, 16800}, "playing from 0.3: " + arpList(on));
        Arpeggiator arp2;
        settle(arp2, arpBase(), {60});
        const auto on2 = arpOnTimes(runArpBlocks(arp2, 15000, kFxBlocks, 120.0, true, 0.4, allocs));
        check(on2 == std::vector<int64_t>{2400, 8400, 14400}, "playing from 0.4: " + arpList(on2));
        // Exactly on a line: step 0 at once.
        Arpeggiator arp3;
        settle(arp3, arpBase(), {60});
        const auto on3 = arpOnTimes(runArpBlocks(arp3, 7000, b256, 120.0, true, 2.0, allocs));
        check(on3 == std::vector<int64_t>{0, 6000}, "playing from 2.0: " + arpList(on3));
    }
    // Latch: released (every held note), the arp plays on; the next press replaces the list and,
    // the list having gone empty → non-empty, restarts the cycle (stopped: the grid restarts at
    // that press; the old note's pending off still comes).
    {
        Arpeggiator arp;
        ArpSettings s = arpBase(); s.latch = true;
        settle(arp, s, {60});
        const auto rec = runArpBlocks(arp, 36100, b256, 120.0, false, 0.0, allocs, [&](int done, double&) {
            if (done == 256) arp.releaseHeld([](int, uint64_t) { return true; });
            if (done == 15360) arp.noteOn(64, 100, 7);
        });
        const auto on60 = arpOnTimes(rec, 60), on64 = arpOnTimes(rec, 64), off60 = arpOffTimes(rec, 60);
        check(on60 == std::vector<int64_t>{0, 6000, 12000}, "latch keeps playing: " + arpList(on60));
        check(on64 == std::vector<int64_t>{15360, 21360, 27360, 33360}, "latch replaced by the next press: " + arpList(on64));
        check(off60 == std::vector<int64_t>{3600, 9600, 15600}, "latch: old note's off still comes " + arpList(off60));
        check(arp.isActive() && arp.heldCount() == 1, "latch active");
    }
    // No latch: release stops the arp after the current gate (its off still comes), list empty.
    {
        Arpeggiator arp;
        settle(arp, arpBase(), {60});
        const auto rec = runArpBlocks(arp, 12100, b256, 120.0, false, 0.0, allocs, [&](int done, double&) {
            if (done == 256) arp.releaseHeld([](int, uint64_t) { return true; });
        });
        check(arpOnTimes(rec) == std::vector<int64_t>{0} && arpOffTimes(rec) == std::vector<int64_t>{3600},
              "release stops: ons " + arpList(arpOnTimes(rec)) + " offs " + arpList(arpOffTimes(rec)));
        check(!arp.isActive() && arp.heldCount() == 0, "release empties the list");
    }
    // Disable while a gate is open (gate 1.2: the note of step 0 sounds past step 1): an off for
    // every sounding note at offset 0 of that block, nothing after, lists cleared.
    {
        Arpeggiator arp;
        ArpSettings s = arpBase(); s.gate = 1.2f; s.mode = ArpMode::Chord;
        settle(arp, s, {60, 64});
        const auto rec = runArpBlocks(arp, 24100, b256, 120.0, false, 0.0, allocs, [&](int done, double&) {
            if (done == 6400) { ArpSettings off = s; off.enabled = false; arp.setSettings(off); }
        });
        const auto off60 = arpOffTimes(rec, 60), off64 = arpOffTimes(rec, 64);
        // Step 0 (0..7200) and step 1 (6000..13200) are both sounding at 6400: one off per note.
        check(off60 == std::vector<int64_t>{6400} && off64 == std::vector<int64_t>{6400},
              "disable offs 60 " + arpList(off60) + " 64 " + arpList(off64));
        check(arpOnTimes(rec) == std::vector<int64_t>{0, 0, 6000, 6000}, "disable: no later ons " + arpList(arpOnTimes(rec)));
        check(!arp.isActive() && arp.heldCount() == 0, "disable clears");
        // A later release of a note the player still holds is harmless.
        arp.releaseHeld([](int, uint64_t) { return true; });
        arp.noteOff(60);
        const auto rec2 = runArpBlocks(arp, 12100, b256, 120.0, false, 0.0, allocs);
        check(rec2.empty(), "disabled: no events");
    }
    // A tap released before the first grid line (playing from 0.4; the line is 2400 in): nothing.
    {
        Arpeggiator arp;
        settle(arp, arpBase(), {60});
        const auto rec = runArpBlocks(arp, 20000, b256, 120.0, true, 0.4, allocs, [&](int done, double&) {
            if (done == 256) arp.releaseHeld([](int, uint64_t) { return true; });
        });
        check(rec.empty() && arp.heldCount() == 0 && !arp.isActive(), "tap before step 0: " + std::to_string(rec.size()) + " events");
    }
    // allNotesOff with latch: stops the sounding notes in the next process, clears the list.
    {
        Arpeggiator arp;
        ArpSettings s = arpBase(); s.latch = true; s.gate = 1.0f;
        settle(arp, s, {60, 64});
        const auto rec = runArpBlocks(arp, 24100, b256, 120.0, false, 0.0, allocs, [&](int done, double&) {
            if (done == 256) arp.releaseHeld([](int, uint64_t) { return true; }); // latched
            if (done == 6400) arp.allNotesOff();
        });
        const auto on = arpOnTimes(rec), off = arpOffTimes(rec);
        check(on == std::vector<int64_t>{0, 6000} && off == std::vector<int64_t>{6000, 6400},
              "allNotesOff: ons " + arpList(on) + " offs " + arpList(off));
        check(!arp.isActive() && arp.heldCount() == 0, "allNotesOff clears");
    }
    // Tempo 120 → 60 after step 1 (stopped): step 2 comes 12000 after step 1, step 3 12000 later.
    {
        Arpeggiator arp;
        settle(arp, arpBase(), {60});
        const auto on = arpOnTimes(runArpBlocks(arp, 30100, kFxBlocks, 120.0, false, 0.0, allocs, [&](int done, double& bpm) {
            if (done > 6000) bpm = 60.0;
        }));
        check(on == std::vector<int64_t>{0, 6000, 18000, 30000}, "tempo change " + arpList(on));
    }
    // Same-note legato (gate 1.5, one note): the previous step's off is dropped when the same note
    // retriggers (the allocator cuts the old voice), so the new voice is not stopped by it.
    {
        Arpeggiator arp;
        ArpSettings s = arpBase(); s.gate = 1.5f;
        settle(arp, s, {60});
        const auto rec = runArpBlocks(arp, 12100, b256, 120.0, false, 0.0, allocs);
        check(arpOnTimes(rec) == std::vector<int64_t>{0, 6000, 12000} && arpOffTimes(rec).empty(),
              "legato: offs " + arpList(arpOffTimes(rec)));
    }
    // Settings: clamping, defaults, enabled/disabled reads.
    {
        Arpeggiator arp;
        ArpSettings s = arpBase();
        s.octaves = 9; s.gate = 7.0f; s.swing = -1.0f; s.patternLength = 40; s.accent = 2.0f; s.ratchet = 0;
        s.transposeSteps = 20; s.transposeDistance = -40;
        arp.setSettings(s);
        const ArpSettings g = arp.getSettings();
        check(g.octaves == 4 && g.gate == 1.5f && g.swing == 0.0f && g.patternLength == 16 && g.accent == 1.0f
              && g.ratchet == 1 && g.transposeSteps == 8 && g.transposeDistance == -12 && g.enabled, "clamping");
        const ArpSettings d = ArpSettings::defaults();
        check(!d.enabled && d.mode == ArpMode::Up && d.rate == ArpRate::Sixteenth && d.octaves == 1 && d.gate == 0.6f
              && d.swing == 0.0f && !d.latch && d.patternMask == 0xFFFF && d.patternLength == 16 && d.accent == 0.0f
              && d.ratchet == 1 && d.transposeSteps == 0 && d.transposeDistance == 0, "defaults");
        check(arpModeIndex("upDown") == (int)ArpMode::UpDown && arpModeIndex("nope") < 0
              && arpRateIndex("1/16T") == (int)ArpRate::SixteenthT && arpRateIndex("1/3") < 0, "name tables");
    }
    const bool pass = failures == 0 && allocs == 0;
    std::printf("arp   arpeggiator: 6 vectors, grid/gate/swing/ratchet, playing start rule, latch, disable, tap, tempo  %-4s"
                " %d failed checks%s allocs %llu\n", pass ? "PASS" : "FAIL", failures, detail.c_str(),
                (unsigned long long)allocs);
    std::fflush(stdout);
    return pass;
}

// Arp through PolySynthProcessor: square wave, attack / release 1 ms, 1/16 at 120 bpm (stopped
// transport), gate 0.5, noteOn(60) → every step's onset is on sample k × 6000 (+ the attack's rise
// to 0.01, at most 3 samples) in 471-sample blocks, and the output is exactly silent for the 1000
// samples before each later step (gate 0.5 = 3000 on, 3000 off, 1 ms release). 16 held notes in
// chord mode sound on 16 voices. Disabling mid-gate silences the output within that block (+ the
// release), a later player noteOff of the arp note is harmless; enabling leaves a directly played
// voice sounding until its release. 0 allocations in every processBlock.
static bool runArpVoices() {
    const double sr = 48000.0;
    int failures = 0;
    std::string detail;
    auto check = [&](bool ok, const std::string& what) { if (!ok) { ++failures; detail += " [" + what + "]"; } };
    auto prepare = [&](PolySynthProcessor& synth, TransportEngine& transport) {
        synth.setTransport(&transport);
        synth.setRateAndBufferSizeDetails(sr, 512);
        synth.prepareToPlay(sr, 512);
        synth.setWaveform(Waveform::Square);
        synth.setAttack(1.0);
        synth.setRelease(1.0);
        synth.setMasterLevel(0.5);
    };
    // Renders `total` samples in 471-sample blocks; `before(done)` runs before each block.
    auto render = [&](PolySynthProcessor& synth, int total, std::vector<float>& out, const std::function<void(int)>& before = {}) {
        juce::AudioBuffer<float> buf(2, 512);
        juce::MidiBuffer midi;
        out.assign((size_t)total, 0.0f);
        for (int done = 0; done < total;) {
            const int n = std::min(471, total - done);
            if (before) before(done);
            buf.setSize(2, n, false, false, true);
            tCountAllocs = true;
            synth.processBlock(buf, midi);
            tCountAllocs = false;
            std::memcpy(&out[(size_t)done], buf.getReadPointer(0), (size_t)n * sizeof(float));
            done += n;
        }
    };
    gRenderAllocs.store(0); gRenderFrees.store(0);
    // 1. Onsets and gaps.
    {
        TransportEngine transport;
        PolySynthProcessor synth;
        prepare(synth, transport);
        ArpSettings s = ArpSettings::defaults();
        s.enabled = true; s.gate = 0.5f;
        synth.arp().setSettings(s);
        synth.noteOn(60, 127);
        std::vector<float> out;
        render(synth, 48000, out);
        std::string onsets;
        for (int k = 0; k < 8; ++k) {
            const int start = k * 6000;
            int first = -1;
            for (int i = start - (k > 0 ? 1000 : 0); i < start + 200; ++i)
                if (std::fabs(out[(size_t)i]) > 0.01f) { first = i; break; }
            onsets += std::to_string(first - start) + " ";
            check(first >= start && first <= start + 3, "step " + std::to_string(k) + " onset at " + std::to_string(first - start));
            if (k > 0) {
                int nonZero = 0;
                for (int i = start - 1000; i < start; ++i) nonZero += out[(size_t)i] != 0.0f ? 1 : 0;
                check(nonZero == 0, "step " + std::to_string(k) + ": " + std::to_string(nonZero) + " non-zero samples in the gap");
            }
        }
        detail += " onsets+" + onsets;
        check(synth.noteQueue().lastNoteStarted.load() == 60, "lastNoteStarted");
        check(synth.arp().isActive() && synth.arp().lastNote() == 60, "arp active");
    }
    // 2. 16 held notes in chord mode → 16 voices.
    int chordVoices = 0;
    {
        TransportEngine transport;
        PolySynthProcessor synth;
        prepare(synth, transport);
        ArpSettings s = ArpSettings::defaults();
        s.enabled = true; s.mode = ArpMode::Chord; s.gate = 1.0f;
        synth.arp().setSettings(s);
        for (int n = 48; n < 64; ++n) synth.noteOn(n, 100);
        std::vector<float> out;
        render(synth, 471, out);
        chordVoices = synth.diag.activeVoices.load();
        check(chordVoices == 16 && MAX_VOICES == 16, "chord voices " + std::to_string(chordVoices));
    }
    // 3. Disable mid-gate, then a player noteOff of the arp note; enabling keeps a direct voice.
    {
        TransportEngine transport;
        PolySynthProcessor synth;
        prepare(synth, transport);
        ArpSettings s = ArpSettings::defaults();
        s.enabled = true; s.gate = 0.5f;
        synth.arp().setSettings(s);
        synth.noteOn(60, 127);
        std::vector<float> out;
        const int disableAt = 6123; // block start right after step 1 (6000)
        render(synth, 24000, out, [&](int done) {
            if (done == disableAt) { ArpSettings off = s; off.enabled = false; synth.arp().setSettings(off); }
            if (done == 6594) synth.noteOff(60);
        });
        int lastNonZero = -1;
        for (int i = 0; i < 24000; ++i) if (out[(size_t)i] != 0.0f) lastNonZero = i;
        check(lastNonZero >= 6000 && lastNonZero < disableAt + 200, "disable silences at " + std::to_string(lastNonZero));
        check(synth.diag.activeVoices.load() == 0 && !synth.arp().isActive(), "disabled: no voices");
        // Direct note while the arp is off, then the arp is enabled: the voice continues.
        synth.noteOn(62, 100);
        render(synth, 942, out);
        const bool soundingBefore = synth.diag.activeVoices.load() == 1;
        synth.arp().setSettings(s);
        render(synth, 942, out);
        float peak = 0.0f;
        for (float v : out) peak = std::max(peak, std::fabs(v));
        const bool soundingAfter = synth.diag.activeVoices.load() == 1 && peak > 0.1f;
        synth.noteOff(62);
        render(synth, 942, out);
        check(soundingBefore && soundingAfter && synth.diag.activeVoices.load() == 0, "enable keeps the direct voice until its release");
    }
    const uint64_t allocs = gRenderAllocs.load() + gRenderFrees.load();
    const bool pass = failures == 0 && allocs == 0;
    std::printf("arpv  arp through PolySynthProcessor: onsets on k x 6000, silent gaps, 16-voice chord, disable, enable  %-4s"
                " %d failed checks%s allocs %llu\n", pass ? "PASS" : "FAIL", failures, detail.c_str(), (unsigned long long)allocs);
    std::fflush(stdout);
    return pass;
}

//==============================================================================
// Spectrum (ios/SpectrumAnalyser.h): 24 log-spaced bands from 2048 mono samples, 0..1 for
// −60..0 dBFS of the loudest bin in the band. A full-scale 1 kHz sine reads ≥ 0.95 in its band
// (800–1027 Hz at 48 kHz) and ≤ 0.6 two bands away; a full-scale sine on a bin centre inside that
// band (914.06 Hz) reads ≥ 0.99 and ≤ 0.6 in both neighbours (a tone straddling bins spills its
// main lobe into the adjacent band, so only sidelobes are checked there); −60 dBFS → ≤ 0.05;
// silence → exactly 0 everywhere; 60 Hz reads ≥ 0.9 in band 0 (40–63.4 Hz) and 70 Hz ≥ 0.95 in
// band 1 (louder than band 0), with nothing above 0.6 from band 3 up; the edge table is strictly
// increasing, every band at least one bin wide, 40 Hz to 16 kHz, at 48000 and 44100 Hz; the
// readRaw copy of the AnalysisRing is oldest-first; analyse() does not allocate.
static bool runSpectrum() {
    const double sr = 48000.0;
    int failures = 0;
    std::string detail;
    auto check = [&](bool ok, const std::string& what) { if (!ok) { ++failures; detail += " [" + what + "]"; } };
    SpectrumAnalyser analyser;
    std::vector<float> mono((size_t)SpectrumAnalyser::kSize, 0.0f);
    float bands[SpectrumAnalyser::kBands];
    float edges[SpectrumAnalyser::kBands + 1];
    SpectrumAnalyser::bandEdges(sr, edges);
    auto bandOf = [&](double hz) {
        for (int i = 0; i < SpectrumAnalyser::kBands; ++i)
            if (hz >= edges[i] && hz < edges[i + 1]) return i;
        return -1;
    };
    auto tone = [&](double hz, double amplitude) {
        for (int i = 0; i < SpectrumAnalyser::kSize; ++i)
            mono[(size_t)i] = (float)(amplitude * std::sin(juce::MathConstants<double>::twoPi * hz * i / sr));
    };
    auto fmt = [](float v) { char b[16]; std::snprintf(b, sizeof b, "%.3f", v); return std::string(b); };
    gRenderAllocs.store(0); gRenderFrees.store(0);
    auto analyse = [&] {
        tCountAllocs = true;
        analyser.analyse(mono.data(), sr, bands);
        tCountAllocs = false;
    };
    // 1 kHz full scale.
    tone(1000.0, 1.0);
    analyse();
    const int b1k = bandOf(1000.0);
    check(b1k == 12 && edges[12] < 801.0 && edges[13] > 1026.0 && edges[13] < 1028.0, "1 kHz band index " + std::to_string(b1k));
    check(bands[b1k] >= 0.95f, "1 kHz band " + fmt(bands[b1k]));
    check(bands[b1k - 2] <= 0.6f && bands[b1k + 2] <= 0.6f, "1 kHz two bands away " + fmt(bands[b1k - 2]) + "/" + fmt(bands[b1k + 2]));
    check(bands[b1k - 1] < bands[b1k] && bands[b1k + 1] < bands[b1k], "1 kHz neighbours below");
    detail += " 1k:" + fmt(bands[b1k - 1]) + "/" + fmt(bands[b1k]) + "/" + fmt(bands[b1k + 1]);
    // On a bin centre inside the same band: full scale, only sidelobes leak.
    tone(39.0 * sr / SpectrumAnalyser::kSize, 1.0);
    analyse();
    check(bands[12] >= 0.99f && bands[11] <= 0.6f && bands[13] <= 0.6f,
          "914 Hz " + fmt(bands[11]) + "/" + fmt(bands[12]) + "/" + fmt(bands[13]));
    // −60 dBFS and silence.
    tone(1000.0, 0.001);
    analyse();
    check(bands[12] <= 0.05f, "-60 dBFS " + fmt(bands[12]));
    std::fill(mono.begin(), mono.end(), 0.0f);
    analyse();
    bool allZero = true;
    for (float v : bands) allZero = allZero && v == 0.0f;
    check(allZero, "silence");
    // Low bands are one bin wide at 48 kHz: 60 Hz in band 0, 70 Hz in band 1.
    tone(60.0, 1.0);
    analyse();
    bool quietAbove = true;
    for (int i = 3; i < SpectrumAnalyser::kBands; ++i) quietAbove = quietAbove && bands[i] <= 0.6f;
    check(bandOf(60.0) == 0 && bands[0] >= 0.9f && quietAbove, "60 Hz band 0 " + fmt(bands[0]) + " band 1 " + fmt(bands[1]));
    tone(70.0, 1.0);
    analyse();
    quietAbove = true;
    for (int i = 3; i < SpectrumAnalyser::kBands; ++i) quietAbove = quietAbove && bands[i] <= 0.6f;
    check(bandOf(70.0) == 1 && bands[1] >= 0.95f && bands[0] < bands[1] && quietAbove,
          "70 Hz band 0 " + fmt(bands[0]) + " band 1 " + fmt(bands[1]));
    // Edge table at 48000 and 44100.
    for (double rate : {48000.0, 44100.0}) {
        float e[SpectrumAnalyser::kBands + 1];
        SpectrumAnalyser::bandEdges(rate, e);
        const double bin = rate / SpectrumAnalyser::kSize;
        bool ok = e[0] == 40.0f && e[SpectrumAnalyser::kBands] == 16000.0f;
        for (int i = 1; i <= SpectrumAnalyser::kBands; ++i) ok = ok && e[i] > e[i - 1] && e[i] - e[i - 1] >= bin - 1e-3;
        // The first bands follow the bin (one bin wide) until the geometric spacing overtakes.
        ok = ok && std::fabs(e[1] - (40.0 + bin)) < 1e-3 && e[23] > 12000.0f;
        check(ok, "edges at " + std::to_string((int)rate));
    }
    // AnalysisRing::readRaw: the last kSize samples, oldest first.
    {
        AnalysisRing ring;
        std::vector<float> ramp(5000);
        for (int i = 0; i < 5000; ++i) ramp[(size_t)i] = (float)i;
        const float* channels[1] = { ramp.data() };
        ring.write(channels, 1, 5000);
        std::vector<float> raw((size_t)AnalysisRing::kSize);
        ring.readRaw(raw.data());
        bool ok = true;
        for (int i = 0; i < AnalysisRing::kSize; ++i) ok = ok && raw[(size_t)i] == (float)(5000 - AnalysisRing::kSize + i);
        check(ok, "readRaw order");
        // GraphPlayer tells the ring the device rate when the device starts.
        GraphPlayer player;
        player.setAnalysis(&ring);
        FakeDevice device(44100, 256);
        player.audioDeviceAboutToStart(&device);
        check(ring.sampleRate() == 44100.0, "ring sample rate from the device");
    }
    const uint64_t allocs = gRenderAllocs.load() + gRenderFrees.load();
    const bool pass = failures == 0 && allocs == 0;
    std::printf("sp    spectrum: 24 bands, 1 kHz / bin-centred / -60 dBFS / silence / 60 + 70 Hz, edge table, readRaw  %-4s"
                " %d failed checks%s allocs %llu\n", pass ? "PASS" : "FAIL", failures, detail.c_str(), (unsigned long long)allocs);
    std::fflush(stdout);
    return pass;
}

// Only the cases named on the command line run (none: every case).
static std::vector<std::string> gOnlyCases;
static bool want(const char* id) {
    if (gOnlyCases.empty()) return true;
    for (const std::string& c : gOnlyCases) if (c == id) return true;
    return false;
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) gOnlyCases.emplace_back(argv[i]);
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
        {"fx", "as (d), every effect on (non-default params)",       48000, 512, 48000, 1024, {471, 1024},
         true, false, false, false, true},
    };

    std::printf("\n%-5s %-52s %-4s %8s %5s %11s %10s %7s %9s %5s %13s %6s\n",
                "case", "description", "res", "peak", "nan", "zero/blocks", "1st-sound", "synth#", "synthPk",
                "cap", "graph#/expect", "allocs");
    std::printf("%s\n", std::string(145, '-').c_str());
    auto row = [](const std::string& id, const std::string& d, const Result& r) {
        std::printf("%-5s %-52s %-4s %8.4f %5d %5d/%-5d %10d %7d %9.4f %5d %6llu/%-6llu %6llu\n", id.c_str(), d.c_str(),
                    r.pass ? "PASS" : "FAIL", r.peakNote, r.nans, r.zeroNoteBlocks, r.noteBlocks,
                    r.firstSoundBlock, r.synthCalls, r.synthPeak, r.capacity,
                    (unsigned long long)r.graphCalls, (unsigned long long)r.expectedGraphCalls,
                    (unsigned long long)r.renderAllocs);
        std::fflush(stdout);
    };
    int unexpected = 0;
    if (want("ctl")) {
        const Result ctl = runSynthAlone();
        unexpected += ctl.pass ? 0 : 1;
        row("ctl", "synth alone + touch stamp, direct, 48k/512", ctl);
    }
    for (auto& c : cases) {
        if (!want(c.id.c_str())) continue;
        auto r = runCase(c);
        unexpected += (r.pass == c.expectSilence) ? 1 : 0;
        // Allocation-free rendering is required in every case, the negative control included.
        if (c.expectSilence && r.renderAllocs > 0) ++unexpected;
        row(c.id, c.desc, r);
    }
    if (want("mp")) {
        const MultiProducerResult mp = runMultiProducer();
        unexpected += mp.pass ? 0 : 1;
        std::printf("mp    2 producers x 10k noteOn/noteOff + audio thread  %-4s attempted %u = queued %u + dropped %u;"
                    " consumed %u; voices after allNotesOff %d; nan %d\n",
                    mp.pass ? "PASS" : "FAIL", mp.attempted, mp.queued, mp.dropped, mp.consumed, mp.activeAfter,
                    mp.nans);
        std::fflush(stdout);
    }
    if (want("rs")) unexpected += runRebuildState() ? 0 : 1;
    if (want("rl")) unexpected += runReleases().pass ? 0 : 1;
    if (want("fx")) unexpected += runFxEffects();
    if (want("fxb")) unexpected += runFxBypass() ? 0 : 1;
    if (want("fxe")) unexpected += runFxEchoSync();
    if (want("fxp")) unexpected += runFxPumpAndLimiter() ? 0 : 1;
    if (want("fxt")) unexpected += runFxTails() ? 0 : 1;
    if (want("fxd")) unexpected += runFxDefaultsMatchJs() ? 0 : 1;
    if (want("met")) unexpected += runMetronome();
    if (want("bt")) unexpected += runBackingTrackPlayback() ? 0 : 1;
    if (want("btt")) unexpected += runBackingTrackTempoRamp() ? 0 : 1;
    if (want("bsf")) unexpected += runBackingTrackSwapFade() ? 0 : 1;
    if (want("btr")) unexpected += runBackingTrackRegion() ? 0 : 1;
    if (want("btl")) unexpected += runBackingTrackLimits() ? 0 : 1;
    if (want("bts")) unexpected += runBackingTrackSwapStress() ? 0 : 1;
    if (want("aux0")) unexpected += runAuxSilent() ? 0 : 1;
    if (want("rsa")) unexpected += runAuxRebuildState() ? 0 : 1;
    if (want("an")) unexpected += runAnalysis() ? 0 : 1;
    if (want("arp")) unexpected += runArp() ? 0 : 1;
    if (want("arpv")) unexpected += runArpVoices() ? 0 : 1;
    if (want("sp")) unexpected += runSpectrum() ? 0 : 1;
    std::printf("\npeak/zero/1st-sound are over the 'note held' window; synth# = PolySynth::processBlock calls\n"
                "(whole case), synthPk = peak inside the synth before the graph.\n"
                "Case (i) logs jasserts from Voice::prepare(0 Hz) during doSetup; expected, it is re-prepared.\n");

    juce::shutdownJuce_GUI();
    std::printf("%d unexpected result(s) (g2 is expected to FAIL)\n", unexpected);
    return unexpected == 0 ? 0 : 1;
}
