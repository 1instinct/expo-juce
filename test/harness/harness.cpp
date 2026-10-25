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
        && back.masterLevel == 0.0f;
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
    std::printf("rs    rebuild carry-over (settings, tempo 90, playing, pending seek, fx)  %-4s synth %d transport %d"
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

// A synthetic track (distinct values per sample) at `rate`.
static std::unique_ptr<DecodedBackingTrack> makeTrack(int length, double rate, double salt = 0.0) {
    auto t = std::make_unique<DecodedBackingTrack>();
    t->sampleRate = rate;
    t->audio.setSize(2, length);
    for (int i = 0; i < length; ++i) {
        const float v = 0.4f * (float)std::sin(i * 0.0123 + salt) + 0.001f * (float)(i % 97);
        t->audio.setSample(0, i, v);
        t->audio.setSample(1, i, -v);
    }
    t->info.sampleRate = rate;
    t->info.channels = 2;
    t->info.loopSamples = length;
    t->info.decodedSamples = length;
    t->info.durationSec = length / rate;
    return t;
}

// Backing track follows the transport: plays from the top, loops, seeks with setTransportPosition
// (5 ms crossfade, then sample-exact), starts in the bar the transport started on, stops (fade,
// then silence), no loop → silent past the end, follows a device-rate change by resampling.
static bool runBackingTrackPlayback() {
    const double sr = 48000.0;
    const int L = 30000;
    const int fade = (int)(AuxProcessor::kFadeSeconds * sr);
    const std::vector<int> blocks = {471, 127, 1024, 333};
    const auto expected = [&](const DecodedBackingTrack& t, int64_t p) { return t.audio.getSample(0, (int)(((p % L) + L) % L)); };
    int bad = 0;
    uint64_t allocs = 0, frees = 0;
    std::string detail;

    // 1. loop from the top, then seek to beat 10.25 (= 246000 → 6000 in the loop), then stop.
    {
        BackingTrack holder;
        auto ref = makeTrack(L, sr);
        DecodedBackingTrack refCopy; refCopy.audio.makeCopyOf(ref->audio); refCopy.sampleRate = sr;
        AuxProcessor aux;
        TransportEngine transport;
        transport.setTempo(120);
        aux.setBackingTrack(&holder);
        aux.setTransport(&transport);
        aux.auxSettings().setTrackLevel(1.0);
        aux.setRateAndBufferSizeDetails(sr, 512);
        aux.prepareToPlay(sr, 512);
        holder.install(std::move(ref), holder.beginLoad());
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
        for (int i = 0; i < seekBlock; ++i) b1 += l[(size_t)i] == expected(refCopy, i) ? 0 : 1;
        for (int i = seekBlock + fade; i < stopBlock; ++i) b2 += l[(size_t)i] == expected(refCopy, 246000 + (i - seekBlock)) ? 0 : 1;
        for (int i = stopBlock + fade; i < total; ++i) b3 += l[(size_t)i] == 0.0f ? 0 : 1;
        bad += b1 + b2 + b3;
        detail += " loop/seek/stop mismatches " + std::to_string(b1) + "/" + std::to_string(b2) + "/" + std::to_string(b3) + ";";
    }
    // 2. start at beat 5.5 with 4 beats per bar: anchor = beat 4, track at 1.5 beats (36000 → 6000).
    {
        BackingTrack holder;
        auto ref = makeTrack(L, sr);
        DecodedBackingTrack refCopy; refCopy.audio.makeCopyOf(ref->audio);
        AuxProcessor aux;
        TransportEngine transport;
        transport.setTempo(120);
        transport.setPosition(5.5);
        aux.setBackingTrack(&holder);
        aux.setTransport(&transport);
        aux.auxSettings().setTrackLevel(1.0);
        aux.setRateAndBufferSizeDetails(sr, 512);
        aux.prepareToPlay(sr, 512);
        holder.install(std::move(ref), holder.beginLoad());
        transport.start();
        std::vector<float> l;
        gRenderAllocs.store(0); gRenderFrees.store(0);
        renderAux(aux, transport, sr, 40000, blocks, l);
        allocs += gRenderAllocs.load(); frees += gRenderFrees.load();
        int b = 0;
        for (int i = fade; i < 40000; ++i) b += l[(size_t)i] == expected(refCopy, 36000 + i) ? 0 : 1;
        bad += b;
        detail += " bar anchor " + std::to_string(b) + ";";
    }
    // 3. no loop: the track plays once, then silence.
    {
        BackingTrack holder;
        auto ref = makeTrack(L, sr);
        DecodedBackingTrack refCopy; refCopy.audio.makeCopyOf(ref->audio);
        AuxProcessor aux;
        TransportEngine transport;
        aux.setBackingTrack(&holder);
        aux.setTransport(&transport);
        aux.auxSettings().setTrackLevel(1.0);
        aux.auxSettings().setTrackLoop(false);
        aux.setRateAndBufferSizeDetails(sr, 512);
        aux.prepareToPlay(sr, 512);
        holder.install(std::move(ref), holder.beginLoad());
        transport.start();
        std::vector<float> l;
        renderAux(aux, transport, sr, 45000, blocks, l);
        int b = 0;
        for (int i = 0; i < 45000; ++i) b += l[(size_t)i] == (i < L ? expected(refCopy, i) : 0.0f) ? 0 : 1;
        bad += b;
        detail += " no-loop " + std::to_string(b) + ";";
    }
    // 4. device rate change: prepare at 44.1 kHz → listener resamples → the 44.1 kHz buffer plays.
    bool rateOk = false;
    {
        BackingTrack holder;
        holder.setRateListener([&holder](double) { holder.resampleToWanted(); }); // the app posts this to a queue
        AuxProcessor aux;
        TransportEngine transport;
        aux.setBackingTrack(&holder);
        aux.setTransport(&transport);
        aux.setRateAndBufferSizeDetails(sr, 512);
        aux.prepareToPlay(sr, 512);
        holder.install(makeTrack(L, sr), holder.beginLoad());
        const BackingTrackBuffer* before = holder.acquire();
        aux.setRateAndBufferSizeDetails(44100, 512);
        aux.prepareToPlay(44100, 512);
        const BackingTrackBuffer* after = holder.acquire();
        rateOk = before != nullptr && before->sampleRate == sr && after != nullptr && after->sampleRate == 44100
              && after->length == (int)std::llround(L * 44100.0 / sr);
        transport.start();
        std::vector<float> l;
        renderAux(aux, transport, 44100, 8000, blocks, l);
        float peak = 0.0f;
        for (float v : l) peak = std::max(peak, std::fabs(v));
        rateOk = rateOk && peak > 0.1f;
        detail += std::string(" rate change ") + (rateOk ? "ok" : "FAILED") + ";";
    }
    const bool pass = bad == 0 && rateOk && allocs == 0 && frees == 0;
    std::printf("bt    backing track: loop, seek, bar anchor, stop, no-loop, rate change  %-4s%s allocs %llu frees %llu\n",
                pass ? "PASS" : "FAIL", detail.c_str(), (unsigned long long)allocs, (unsigned long long)frees);
    std::fflush(stdout);
    return pass;
}

// Loop region from the MP3 fixture (44.1 kHz stereo, LAME/Xing gapless tag) with the JUCE MP3
// decoder (the harness has no CoreAudio): which length it yields and which region the rule picks,
// then the resample to 48 kHz. Also the rule itself on the three lengths it distinguishes.
static bool runBackingTrackRegion() {
    const BackingTrackGapless g{682839, 4561, 1496};
    const BackingTrackRegion honoured = backingTrackRegion(682839, &g);
    const BackingTrackRegion ignored = backingTrackRegion(1152 + 4561 + 682839 + 1496, &g);
    const BackingTrackRegion other = backingTrackRegion(4561 + 682839 + 1496, &g);
    const BackingTrackRegion none = backingTrackRegion(5000, nullptr);
    const bool ruleOk = honoured.start == 0 && honoured.length == 682839 && ignored.start == 1152 + 4561
        && ignored.length == 682839 && other.start == 0 && other.length == 4561 + 682839 + 1496
        && none.start == 0 && none.length == 5000;

    const juce::File file(juce::String(HARNESS_FIXTURES) + "/loop-124bpm-44k.mp3");
    std::string error;
    auto decoded = BackingTrack::decode(file, &g, {}, error);
    bool decodeOk = decoded != nullptr;
    std::string path = "none";
    int64_t resampled = 0, expectedResampled = 0;
    double seamJump = 0.0, meanStep = 0.0;
    if (decodeOk) {
        const BackingTrackInfo& info = decoded->info;
        const int64_t d = info.decodedSamples;
        if (d == g.loopSamples) path = "honoured the tag (== loopSamples)";
        else if (d == 1152 + g.leadingSamples + g.loopSamples + g.trailingSamples) path = "ignored the tag, tag frame decoded (== 1152 + lead + loop + trail)";
        else if (d == g.leadingSamples + g.loopSamples + g.trailingSamples) path = "skipped the tag frame but ignored the gapless values (== lead + loop + trail): rule falls back to the whole file";
        else path = "other length";
        const BackingTrackRegion want = backingTrackRegion(d, &g);
        decodeOk = info.sampleRate == 44100.0 && info.channels == 2 && info.loopStartSample == want.start
                && info.loopSamples == want.length && decoded->audio.getNumSamples() == (int)want.length;
        const float* x = decoded->audio.getReadPointer(0);
        const int n = decoded->audio.getNumSamples();
        for (int i = 1; i < n; ++i) meanStep += std::fabs(x[i] - x[i - 1]);
        meanStep /= std::max(1, n - 1);
        seamJump = std::fabs(x[n - 1] - x[0]);
        auto buf = BackingTrack::resample(*decoded, 48000.0);
        resampled = buf->length;
        expectedResampled = std::llround((double)n * 48000.0 / 44100.0);
        decodeOk = decodeOk && resampled == expectedResampled && buf->audio.getNumSamples() == resampled;
        std::printf("btr   MP3 fixture via %s: decoded %lld samples (loop %lld + lead %lld + trail %lld) -> %s;"
                    " region [%lld, +%lld); seam |x[end]-x[0]| %.4f vs mean step %.4f\n",
                    info.formatName.c_str(), (long long)d, (long long)g.loopSamples, (long long)g.leadingSamples,
                    (long long)g.trailingSamples, path.c_str(), (long long)info.loopStartSample,
                    (long long)info.loopSamples, seamJump, meanStep);
    } else {
        std::printf("btr   MP3 fixture decode failed: %s\n", error.c_str());
    }
    const bool pass = ruleOk && decodeOk;
    std::printf("btr   loop-region rule (3 lengths) + fixture decode + resample 44.1k->48k  %-4s rule %d, decode %d,"
                " resampled %lld (expect %lld)\n", pass ? "PASS" : "FAIL", ruleOk, decodeOk, (long long)resampled,
                (long long)expectedResampled);
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
            if (i % 7 == 6) holder.unload();
            else holder.install(makeTrack(2000 + 37 * i, 44100.0, i), holder.beginLoad());
            if (i % 3 == 0) std::this_thread::sleep_for(std::chrono::microseconds(300));
            pendingMax = std::max(pendingMax, holder.collect());
        }
        // A load superseded before it installs must not publish.
        const uint64_t stale = holder.beginLoad();
        holder.beginLoad();
        const bool staleRejected = !holder.install(makeTrack(1000, 44100.0), stale);
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
                (unsigned long long)blocksRendered.load(), pass ? "PASS" : "FAIL", (unsigned long long)allocs, (unsigned long long)frees,
                pendingMax, liveAfter, pendingMax < 1000);
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
        holder.install(makeTrack(5000, 48000), holder.beginLoad());
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
            holder.install(makeTrack(20000, 48000), holder.beginLoad());
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
// scope points are the sine at the expected sample indices (stride 16, newest last), peak / rms
// of the last block match, and the callback still does not allocate.
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
    // Last block: 100 samples starting at total - 100.
    float wantPeak = 0.0f;
    double sum = 0.0;
    for (int i = total - 100; i < total; ++i) {
        const float v = SineSource::value(i);
        wantPeak = std::max(wantPeak, std::fabs(v));
        sum += 2.0 * v * v;
    }
    const double wantRms = std::sqrt(sum / 200.0);
    const bool pass = maxErr < 1e-6 && std::fabs(ring.peak() - wantPeak) < 1e-6 && std::fabs(ring.rms() - wantRms) < 1e-5
                   && ring.samplesWritten() == (uint32_t)total && gRenderAllocs.load() == 0 && gRenderFrees.load() == 0;
    std::printf("an    analysis ring: scope of a known sine, peak / rms of the last block  %-4s scope max err %.2g,"
                " peak %.4f (want %.4f), rms %.4f (want %.4f), allocs %llu\n", pass ? "PASS" : "FAIL", maxErr,
                ring.peak(), wantPeak, ring.rms(), wantRms, (unsigned long long)gRenderAllocs.load());
    std::fflush(stdout);
    return pass;
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
    {
        const Result ctl = runSynthAlone();
        unexpected += ctl.pass ? 0 : 1;
        row("ctl", "synth alone + touch stamp, direct, 48k/512", ctl);
    }
    for (auto& c : cases) {
        auto r = runCase(c);
        unexpected += (r.pass == c.expectSilence) ? 1 : 0;
        // Allocation-free rendering is required in every case, the negative control included.
        if (c.expectSilence && r.renderAllocs > 0) ++unexpected;
        row(c.id, c.desc, r);
    }
    {
        const MultiProducerResult mp = runMultiProducer();
        unexpected += mp.pass ? 0 : 1;
        std::printf("mp    2 producers x 10k noteOn/noteOff + audio thread  %-4s attempted %u = queued %u + dropped %u;"
                    " consumed %u; voices after allNotesOff %d; nan %d\n",
                    mp.pass ? "PASS" : "FAIL", mp.attempted, mp.queued, mp.dropped, mp.consumed, mp.activeAfter,
                    mp.nans);
        std::fflush(stdout);
    }
    unexpected += runRebuildState() ? 0 : 1;
    unexpected += runReleases().pass ? 0 : 1;
    unexpected += runFxEffects();
    unexpected += runFxBypass() ? 0 : 1;
    unexpected += runFxEchoSync();
    unexpected += runFxPumpAndLimiter() ? 0 : 1;
    unexpected += runFxTails() ? 0 : 1;
    unexpected += runFxDefaultsMatchJs() ? 0 : 1;
    unexpected += runMetronome();
    unexpected += runBackingTrackPlayback() ? 0 : 1;
    unexpected += runBackingTrackRegion() ? 0 : 1;
    unexpected += runBackingTrackSwapStress() ? 0 : 1;
    unexpected += runAuxSilent() ? 0 : 1;
    unexpected += runAuxRebuildState() ? 0 : 1;
    unexpected += runAnalysis() ? 0 : 1;
    std::printf("\npeak/zero/1st-sound are over the 'note held' window; synth# = PolySynth::processBlock calls\n"
                "(whole case), synthPk = peak inside the synth before the graph.\n"
                "Case (i) logs jasserts from Voice::prepare(0 Hz) during doSetup; expected, it is re-prepared.\n");

    juce::shutdownJuce_GUI();
    std::printf("%d unexpected result(s) (g2 is expected to FAIL)\n", unexpected);
    return unexpected == 0 ? 0 : 1;
}
