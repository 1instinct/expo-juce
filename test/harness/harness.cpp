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
#include "DrumMachine.h"
#include "LatencyStats.h"
#include "DisplayRate.h"
#include "DeviceFormat.h"
#include "MidiInput.h"
#include "ExpoJuceMidiInput.h"
#include "OwnerToken.h"
#include "SamplerEngine.h"
#include "SamplerBank.h"
#include "SequencePlayer.h"
#include "BackgroundPolicy.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <tuple>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

//==============================================================================
// Allocation counter for the render path. glibc's malloc family is interposed (operator new and
// JUCE's HeapBlock both end up here); while tCountAllocs is set on a thread, every allocation it
// makes is counted. The fake device sets it around each GraphPlayer IO callback, so any
// allocation anywhere inside the render (GraphPlayer, AudioProcessorGraph, synth) shows up.
// A sanitizer build (-DHARNESS_SANITIZE=address|thread|…) leaves malloc to the sanitizer: the
// counters then stay 0, and that build checks memory errors and races instead.
static thread_local bool tCountAllocs = false;
static std::atomic<uint64_t> gRenderAllocs{0};
static std::atomic<uint64_t> gRenderFrees{0}; // frees inside counted render calls (must be 0 too)
#ifndef HARNESS_NO_MALLOC_INTERPOSE
extern "C" {
void* __libc_malloc(size_t);
void* __libc_calloc(size_t, size_t);
void* __libc_realloc(void*, size_t);
void* __libc_memalign(size_t, size_t);
void __libc_free(void*);
}
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
#endif // HARNESS_NO_MALLOC_INTERPOSE

//==============================================================================
// BackingTrack::acquire() test seam (CMake defines BACKING_TRACK_ACQUIRE_HOOK as this name). A
// test arms gAcquireHook; the next acquire() runs it once, between its first read of the
// published pointer and the re-validation, i.e. as if the control thread published right then.
// What the hook does is not counted as a render allocation.
static std::function<void()> gAcquireHook;
static int gAcquireHookCalls = 0;
void harnessBackingTrackAcquireHook() {
    if (!gAcquireHook) return;
    const bool counting = tCountAllocs;
    tCountAllocs = false;
    auto hook = std::move(gAcquireHook);
    gAcquireHook = nullptr;
    ++gAcquireHookCalls;
    hook();
    tCountAllocs = counting;
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

    // The note queue outlives the synth. After an allNotesOff on it, a synth built later and
    // attached to it must take the queue's current all-released mark as already applied, not as
    // new: otherwise its first enabled block flushes the chord it drained in that same block.
    bool arpRebuildOk = false;
    {
        NoteEventQueue sharedQueue;
        ArpSettings a = ArpSettings::defaults();
        a.enabled = true; a.mode = ArpMode::Chord; a.gate = 1.0f;
        TransportEngine t1;
        PolySynthProcessor s1;
        s1.setTransport(&t1); s1.setNoteQueue(&sharedQueue);
        s1.setRateAndBufferSizeDetails(48000, 256); s1.prepareToPlay(48000, 256);
        s1.arp().setSettings(a);
        s1.noteOn(60, 100); s1.processBlock(buf, midi);
        s1.allNotesOff(); s1.processBlock(buf, midi); // the mark advances, the list is cleared
        const bool clearedOk = !s1.arp().isActive() && s1.arp().heldCount() == 0;
        TransportEngine t2;
        PolySynthProcessor s2;
        s2.setTransport(&t2); s2.setNoteQueue(&sharedQueue);
        s2.setRateAndBufferSizeDetails(48000, 256); s2.prepareToPlay(48000, 256);
        s2.arp().setSettings(a);
        s2.noteOn(60, 100); s2.noteOn(64, 100); s2.noteOn(67, 100);
        for (int b = 0; b < 4; ++b) s2.processBlock(buf, midi);
        arpRebuildOk = clearedOk && s2.arp().isActive() && s2.arp().heldCount() == 3 && s2.diag.activeVoices.load() == 3;
    }

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
    const bool ok = synthOk && transportOk && continuesOk && fxOk && arpRebuildOk;
    std::printf("rs    rebuild carry-over (settings + arp, tempo 90, playing, pending seek, fx, arp + shared queue)  %-4s synth %d"
                " transport %d continues %d (pos %.4f) fx state %d fx render %d arp rebuild %d\n", ok ? "PASS" : "FAIL",
                synthOk, transportOk, continuesOk, newTransport.getPosition(), fxStateOk, fxRenderOk, arpRebuildOk);
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

// ── Backing track following the tempo ──
// A click on every beat of a track at `srcBpm`: 6 ms of 1 kHz at 0.8, otherwise exact silence.
static std::unique_ptr<BackingTrackBuffer> clickTrack(double srcBpm, double sr, int beats) {
    auto t = std::make_unique<BackingTrackBuffer>();
    const double perBeat = 60.0 / srcBpm * sr;
    const int L = (int)std::llround(perBeat * beats);
    t->sampleRate = sr;
    t->length = L;
    t->audio.setSize(2, L);
    t->audio.clear();
    const int burst = (int)(0.006 * sr);
    for (int k = 0; k < beats; ++k) {
        const int at = (int)std::llround(k * perBeat);
        for (int i = 0; i < burst && at + i < L; ++i) {
            const float v = 0.8f * (float)std::sin(juce::MathConstants<double>::twoPi * 1000.0 * i / sr);
            t->audio.setSample(0, at + i, v);
            t->audio.setSample(1, at + i, v);
        }
    }
    return t;
}

// A steady sine, a whole number of periods long, so its own loop seam is invisible.
static std::unique_ptr<BackingTrackBuffer> sineTrack(double freq, double sr, double seconds, float amp = 0.5f) {
    auto t = std::make_unique<BackingTrackBuffer>();
    const int periods = (int)std::llround(freq * seconds);
    const int L = (int)std::llround(periods * sr / freq);
    t->sampleRate = sr;
    t->length = L;
    t->audio.setSize(2, L);
    for (int i = 0; i < L; ++i) {
        const float v = amp * (float)std::sin(juce::MathConstants<double>::twoPi * periods * (double)i / (double)L);
        t->audio.setSample(0, i, v);
        t->audio.setSample(1, i, v);
    }
    return t;
}

// Where each beat's click sounds, in samples from the start of the render: the first sample over
// half the click's level within ±windowSec of `gridAt(beat)`. -1 if none.
static std::vector<int> clickOnsets(const std::vector<float>& l, int firstBeat, int beats, double sr, double windowSec,
                                    const std::function<double(int)>& gridAt) {
    std::vector<int> out;
    const int w = (int)(windowSec * sr);
    for (int k = firstBeat; k < firstBeat + beats; ++k) {
        const int g = (int)std::llround(gridAt(k));
        int found = -1;
        for (int i = std::max(0, g - w); i < std::min((int)l.size(), g + w); ++i)
            if (std::fabs(l[(size_t)i]) > 0.4f) { found = i; break; }
        out.push_back(found);
    }
    return out;
}

// Frequency from positive-going zero crossings over [from, to).
static double risingCrossingHz(const std::vector<float>& l, int from, int to, double sr) {
    int first = -1, last = -1, crossings = 0;
    for (int i = from + 1; i < to; ++i) {
        if (l[(size_t)i - 1] < 0.0f && l[(size_t)i] >= 0.0f) {
            if (first < 0) first = i;
            last = i;
            ++crossings;
        }
    }
    return crossings > 1 ? (crossings - 1) * sr / (double)(last - first) : 0.0;
}

// A track with a source tempo keeps its beats on the transport's beats at any tempo, with its
// pitch unchanged, and is the untouched file at its own tempo.
static bool runBackingTrackFollow() {
    const std::vector<int> blocks = {471, 127, 1024, 333};
    int failures = 0;
    uint64_t allocs = 0, frees = 0;
    std::string detail;
    const auto check = [&](bool ok, const std::string& what) {
        if (!ok) ++failures;
        detail += std::string("  ") + (ok ? "ok   " : "FAIL ") + what + "\n";
    };
    struct Rig {
        BackingTrack holder;
        AuxProcessor aux;
        TransportEngine transport;
        Rig(double sr, double bpm) {
            transport.setTempo(bpm);
            aux.setBackingTrack(&holder);
            aux.setTransport(&transport);
            aux.auxSettings().setTrackLevel(1.0);
            aux.setRateAndBufferSizeDetails(sr, 512);
            aux.prepareToPlay(sr, 512);
        }
        void install(std::unique_ptr<BackingTrackBuffer> t, double sourceBpm) {
            t->sourceBpm = sourceBpm;
            holder.install(std::move(t), {}, holder.beginLoad());
        }
    };
    const auto render = [&](Rig& r, double sr, int total, std::vector<float>& l, const std::function<void(int)>& before = {}) {
        gRenderAllocs.store(0); gRenderFrees.store(0);
        renderAux(r.aux, r.transport, sr, total, blocks, l, nullptr, before);
        allocs += gRenderAllocs.load(); frees += gRenderFrees.load();
    };

    // 1. Beat lock at steady tempos, both ways and at both rates: every click within the limit
    //    of its beat, and no drift from the first beats to the last.
    struct Lock { double src, bpm, sr; };
    for (const Lock c : {Lock{100, 120, 48000}, Lock{100, 80, 48000}, Lock{100, 150, 48000}, Lock{124, 100, 44100}, Lock{84, 90, 48000}}) {
        Rig r(c.sr, c.bpm);
        r.install(clickTrack(c.src, c.sr, 32), c.src);
        r.transport.start();
        const int beats = 96; // three times around the loop
        const double perBeat = 60.0 / c.bpm * c.sr;
        std::vector<float> l;
        render(r, c.sr, (int)(perBeat * (beats + 1)), l);
        const auto on = clickOnsets(l, 0, beats, c.sr, 0.05, [&](int k) { return k * perBeat; });
        double worst = 0.0, early = 0.0, late = 0.0;
        int missing = 0;
        for (int k = 0; k < beats; ++k) {
            if (on[(size_t)k] < 0) { ++missing; continue; }
            const double err = (on[(size_t)k] - k * perBeat) / c.sr * 1000.0;
            worst = std::max(worst, std::fabs(err));
            if (k < 16) early += err / 16.0;
            if (k >= beats - 16) late += err / 16.0;
        }
        char text[160];
        std::snprintf(text, sizeof(text), "track %.0f at %.0f bpm, %.0f Hz: worst click %.1f ms from its beat, drift %.2f ms, missing %d",
                      c.src, c.bpm, c.sr, worst, late - early, missing);
        check(missing == 0 && worst <= 20.0 && std::fabs(late - early) <= 3.0, text);
    }

    // 2. A tempo change while playing takes effect: 120 → 90 bpm on bar 5, the clicks are on the
    //    new grid at once, and bar 1 of the second time round is still on its beat.
    {
        const double sr = 48000.0;
        Rig r(sr, 120);
        r.install(clickTrack(100, sr, 16), 100);
        r.transport.start();
        const int change = 16 * 24000;
        bool changed = false;
        std::vector<float> l;
        render(r, sr, change + 34 * 32000, l, [&](int done) {
            if (!changed && done >= change) { r.transport.setTempo(90); changed = true; }
        });
        const auto on = clickOnsets(l, 17, 32, sr, 0.05, [&](int k) { return change + (k - 16) * 32000.0; });
        double worst = 0.0;
        int missing = 0;
        for (int k = 0; k < 32; ++k) {
            if (on[(size_t)k] < 0) { ++missing; continue; }
            worst = std::max(worst, std::fabs(on[(size_t)k] - (change + (k + 1) * 32000.0)) / sr * 1000.0);
        }
        char text[120];
        std::snprintf(text, sizeof(text), "tempo 120 -> 90 while playing: worst click %.1f ms from its beat, missing %d", worst, missing);
        check(missing == 0 && worst <= 25.0, text);
    }

    // 3. Pitch stays, level stays, no clicks: a steady sine through the stretch.
    for (const double bpm : {120.0, 80.0, 133.0}) {
        for (const double freq : {55.0, 220.0, 1760.0}) {
            const double sr = 48000.0;
            Rig r(sr, bpm);
            r.install(sineTrack(freq, sr, 1.0), 100);
            r.transport.start();
            const int total = 3 * 48000;
            std::vector<float> l;
            render(r, sr, total, l);
            const double hz = risingCrossingHz(l, 4800, total, sr);
            float lowest = 1.0f, highest = 0.0f, step = 0.0f;
            const int win = std::max(1024, (int)(3.0 * sr / freq));
            for (int from = 4800; from + win < total; from += win / 2) {
                double e = 0.0;
                for (int i = from; i < from + win; ++i) e += (double)l[(size_t)i] * l[(size_t)i];
                const float rms = (float)std::sqrt(e / win);
                lowest = std::min(lowest, rms);
                highest = std::max(highest, rms);
            }
            for (int i = 1; i < total; ++i) step = std::max(step, std::fabs(l[(size_t)i] - l[(size_t)i - 1]));
            const float idealRms = 0.5f / std::sqrt(2.0f);
            const float idealStep = 0.5f * (float)(juce::MathConstants<double>::twoPi * freq / sr);
            char text[170];
            std::snprintf(text, sizeof(text), "%.0f Hz sine at %.0f bpm (track 100): %.2f Hz, rms %.3f..%.3f (ideal %.3f), largest step %.4f (sine's own %.4f)",
                          freq, bpm, hz, lowest, highest, idealRms, step, idealStep);
            check(std::fabs(hz / freq - 1.0) < 0.005 && lowest > 0.9f * idealRms && highest < 1.1f * idealRms
                  && step < idealStep * 1.25f + 0.002f, text);
        }
    }

    // 4. At its own tempo it is the file: the same samples as with no source tempo at all, across
    //    a start, a seek and a pause + resume.
    {
        const double sr = 48000.0;
        const auto run = [&](double sourceBpm, std::vector<float>& l) {
            Rig r(sr, 100);
            r.install(makeTrack(30000, sr), sourceBpm);
            r.transport.start();
            int phase = 0;
            render(r, sr, 200000, l, [&](int done) {
                if (phase == 0 && done >= 60000) { r.transport.setPosition(10.25); ++phase; }
                if (phase == 1 && done >= 90000) { r.transport.stop(); ++phase; }
                if (phase == 2 && done >= 100000) { r.transport.start(); ++phase; }
            });
        };
        std::vector<float> plain, own;
        run(0.0, plain);
        run(100.0, own);
        int diffs = 0;
        for (size_t i = 0; i < plain.size(); ++i) diffs += plain[i] == own[i] ? 0 : 1;
        check(diffs == 0, "track at its own tempo == no source tempo, sample for sample: " + std::to_string(diffs) + " differ");
    }

    // 5. Back at its own tempo it returns to the file, sample-exact and on the grid. A change at a
    //    known beat: 120 → 100 bpm, then every sample equals the track at beat × 60 / 100.
    {
        const double sr = 48000.0;
        const int L = 300000;
        juce::AudioBuffer<float> ref;
        { auto t = makeTrack(L, sr); ref.makeCopyOf(t->audio); }
        Rig r(sr, 120);
        r.install(makeTrack(L, sr), 100);
        r.transport.start();
        bool changed = false;
        int changeAt = -1;
        double beatAtChange = 0.0;
        std::vector<float> l;
        render(r, sr, 240000, l, [&](int done) {
            if (!changed && done >= 96000) {
                beatAtChange = done * 120.0 / 60.0 / sr;
                r.transport.setTempo(100);
                changeAt = done;
                changed = true;
            }
        });
        // Two hand-overs at most, then the plain path.
        const int settled = changeAt + (int)(2.0 * AuxProcessor::kGrainHopSeconds * sr) + 2048;
        int off = 0;
        for (int i = settled; i < 240000; ++i) {
            const double beat = beatAtChange + (i - changeAt) * 100.0 / 60.0 / sr;
            const int64_t p = std::llround(beat * 60.0 / 100.0 * sr);
            off += l[(size_t)i] == ref.getSample(0, (int)(p % L)) ? 0 : 1;
        }
        check(off == 0, "back at its own tempo: sample-exact on the grid, " + std::to_string(off) + " samples off");
    }

    // 6. Seek and pause + resume while stretched land on the track's own grid, not the transport's.
    {
        const double sr = 48000.0;
        Rig r(sr, 120);
        r.install(clickTrack(100, sr, 64), 100);
        r.transport.start();
        int seekAt = -1, pauseAt = -1, resumeAt = -1;
        std::vector<float> l;
        render(r, sr, 400000, l, [&](int done) {
            if (seekAt < 0 && done >= 50000) { r.transport.setPosition(16.5); seekAt = done; }
            if (pauseAt < 0 && done >= 150000) { r.transport.stop(); pauseAt = done; }
            if (pauseAt >= 0 && resumeAt < 0 && done >= 200000) { r.transport.start(); resumeAt = done; }
        });
        const auto worstFrom = [&](double beatBase, int base, int count) {
            double worst = 0.0;
            for (int k = 0; k < count; ++k) {
                const int beat = (int)std::ceil(beatBase) + k;
                const double at = base + (beat - beatBase) * 24000.0;
                const auto on = clickOnsets(l, 0, 1, sr, 0.05, [&](int) { return at; });
                worst = std::max(worst, on[0] < 0 ? 1000.0 : std::fabs(on[0] - at) / sr * 1000.0);
            }
            return worst;
        };
        const double afterSeek = worstFrom(16.5, seekAt, 3);
        const double beatAtPause = 16.5 + (pauseAt - seekAt) / 24000.0;
        const double afterResume = worstFrom(beatAtPause, resumeAt, 3);
        char text[120];
        std::snprintf(text, sizeof(text), "stretched: clicks after a seek within %.1f ms, after pause + resume within %.1f ms", afterSeek, afterResume);
        check(afterSeek <= 20.0 && afterResume <= 20.0, text);
    }

    // 7. Starting, seeking, stopping and swapping a stretched track are declicked.
    {
        const double sr = 48000.0;
        Rig r(sr, 120);
        r.install(sineTrack(220.0, sr, 1.0), 100);
        r.transport.start();
        int phase = 0;
        std::vector<float> l;
        render(r, sr, 192000, l, [&](int done) {
            if (phase == 0 && done >= 48000) { r.transport.setPosition(3.3); ++phase; }
            if (phase == 1 && done >= 96000) { r.holder.install([&] { auto t = sineTrack(220.0, sr, 1.5); t->sourceBpm = 100; return t; }(), {}, r.holder.beginLoad()); ++phase; }
            if (phase == 2 && done >= 144000) { r.transport.stop(); ++phase; }
        });
        float step = 0.0f;
        for (size_t i = 1; i < l.size(); ++i) step = std::max(step, std::fabs(l[i] - l[i - 1]));
        char text[120];
        std::snprintf(text, sizeof(text), "stretched seek, swap, stop: largest step %.4f (sine's own 0.0144)", step);
        check(step < 0.05f, text);
        int collected = -1;
        for (int i = 0; i < 3 && collected != 0; ++i) collected = r.holder.collect();
        check(collected == 0, "replaced buffer freed after its fade-out");
    }

    // 8. Loop off: one stretched pass, then silence.
    {
        const double sr = 48000.0;
        Rig r(sr, 120);
        r.aux.auxSettings().setTrackLoop(false);
        r.install(sineTrack(220.0, sr, 1.0), 100); // 1 s of track = 1 / 1.2 s on this transport
        r.transport.start();
        std::vector<float> l;
        render(r, sr, 96000, l);
        const int end = (int)(48000 / 1.2);
        const int after = end + (int)((AuxProcessor::kGrainHopSeconds * 2.0 + AuxProcessor::kGrainSearchSeconds) * sr);
        float sounding = 0.0f, past = 0.0f;
        for (int i = 0; i < end - 2400; ++i) sounding = std::max(sounding, std::fabs(l[(size_t)i]));
        for (int i = after; i < 96000; ++i) past = std::max(past, std::fabs(l[(size_t)i]));
        char text[100];
        std::snprintf(text, sizeof(text), "loop off: peak %.2f during the pass, %.5f after it", sounding, past);
        check(sounding > 0.4f && past == 0.0f, text);
    }

    // 9. The tempo travels with the file: load() puts it on the buffer, a device-rate reload keeps
    //    it, and nonsense means "unknown".
    {
        const double sr = 48000.0;
        juce::AudioBuffer<float> ref;
        { auto t = makeTrack(30000, sr); ref.makeCopyOf(t->audio); }
        const std::string path = writeWav(ref, sr, "ejfx-bt-follow.wav");
        BackingTrackSource source;
        source.path = path;
        source.sourceBpm = 128.5;
        BackingTrackInfo info;
        std::string error;
        auto buffer = BackingTrack::load(source, sr, {}, info, error);
        const bool onLoad = buffer != nullptr && buffer->sourceBpm == 128.5 && info.sourceBpm == 128.5;
        BackingTrack holder;
        holder.install(std::move(buffer), source, holder.beginLoad());
        holder.requestRate(44100.0);
        const bool reloaded = holder.reload();
        const BackingTrackBuffer* after = holder.publishedBuffer();
        const bool onReload = reloaded && after != nullptr && after->sampleRate == 44100.0 && after->sourceBpm == 128.5;
        bool nonsense = true;
        for (const double v : {-10.0, std::nan(""), std::numeric_limits<double>::infinity()}) {
            source.sourceBpm = v;
            auto b = BackingTrack::load(source, sr, {}, info, error);
            nonsense = nonsense && b != nullptr && b->sourceBpm == 0.0 && info.sourceBpm == 0.0;
        }
        holder.unload();
        holder.collect();
        juce::File(juce::String(path)).deleteFile();
        check(onLoad, "load() puts the tempo on the buffer and in the info");
        check(onReload, "a device-rate reload keeps the tempo");
        check(nonsense, "negative, NaN and infinite tempos mean unknown");
    }

    // 10. Cost: the stretch on a dense track, 128-sample blocks, 20 s, well under real time, with
    //     no block near its budget (a search lands inside one block).
    {
        const double sr = 48000.0;
        Rig r(sr, 120);
        r.install(makeTrack(480000, sr), 100);
        r.transport.start();
        juce::AudioBuffer<float> buf(2, 128);
        juce::MidiBuffer midi;
        double total = 0.0, worst = 0.0;
        for (int i = 0; i < (int)(20.0 * sr / 128); ++i) {
            const auto t0 = std::chrono::steady_clock::now();
            r.transport.advanceBlock(128, sr);
            r.aux.processBlock(buf, midi);
            const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            total += s;
            worst = std::max(worst, s);
        }
        char text[120];
        std::snprintf(text, sizeof(text), "cost: %.4f x realtime, worst block %.3f ms of its 2.667 ms", total / 20.0, worst * 1000.0);
        check(total / 20.0 < 0.05, text);
    }

    // 11. Real music (the 124 bpm MP3 fixture, 48 kHz) at 124, 123, 100, 150 and 70 bpm, with a click on
    //     every beat: each click within 12 ms of its beat (the search reaches 10 ms; 20 ms at 70 bpm,
    //     close to half speed), the median within 7 ms (the search now
    //     sees real music); the level per beat stays within 1.5 dB of the source's.
    {
        const double sr = 48000.0;
        BackingTrackSource source;
        source.path = std::string(HARNESS_FIXTURES) + "/loop-124bpm-44k.mp3";
        source.hasGapless = true;
        source.gapless = {682839, 4561, 1496};
        source.sourceBpm = 124.0;
        BackingTrackInfo info;
        std::string error;
        auto loaded = BackingTrack::load(source, sr, {}, info, error);
        if (loaded == nullptr) {
            check(false, "fixture decode: " + error);
        } else {
            juce::AudioBuffer<float> ref;
            ref.makeCopyOf(loaded->audio);
            const int L = loaded->length;
            // The music at half level plus a click on every beat (the click track of part 1).
            juce::AudioBuffer<float> mixed;
            mixed.makeCopyOf(ref);
            mixed.applyGain(0.5f);
            {
                auto clicks = clickTrack(124.0, sr, (int)std::floor(L / (60.0 / 124.0 * sr)));
                for (int ch = 0; ch < 2; ++ch) mixed.addFrom(ch, 0, clicks->audio, ch, 0, std::min(L, clicks->length));
            }
            for (const double bpm : {124.0, 123.0, 100.0, 150.0, 70.0}) {
                Rig r(sr, bpm);
                auto copy = std::make_unique<BackingTrackBuffer>();
                copy->audio.makeCopyOf(mixed);
                copy->sampleRate = sr;
                copy->length = L;
                r.install(std::move(copy), 124.0);
                r.transport.start();
                const double outPerBeat = 60.0 / bpm * sr, srcPerBeat = 60.0 / 124.0 * sr;
                const int beats = 24;
                std::vector<float> l;
                render(r, sr, (int)(outPerBeat * (beats + 1)), l);
                // A 1 kHz click (6 ms) on every source beat rides on the music; the matched filter
                // finds it in the output within 40 ms of its beat.
                const int burst = (int)(0.006 * sr), reach = (int)(0.04 * sr);
                const auto clickAt = [&](double beatAt) {
                    double best = -1.0;
                    int at = 0;
                    for (int d = -reach; d <= reach; ++d) {
                        double acc = 0.0;
                        for (int j = 0; j < burst; ++j)
                            acc += l[(size_t)((int)beatAt + d + j)] * std::sin(juce::MathConstants<double>::twoPi * 1000.0 * j / sr);
                        if (acc > best) { best = acc; at = d; }
                    }
                    return at;
                };
                std::vector<double> lags;
                double worstDb = 0.0;
                // The level is compared on the music alone: 30 ms clear of the clicks at both ends of a beat.
                const int skipOut = (int)(0.03 * sr), skipSrc = (int)(0.03 * sr);
                for (int k = 1; k < beats - 1; ++k) {
                    lags.push_back(std::fabs(clickAt(std::llround(k * outPerBeat)) / sr * 1000.0));
                    double eo = 0.0, es = 0.0;
                    for (int i = skipOut; i < (int)outPerBeat - skipOut; ++i) eo += (double)l[(size_t)(k * outPerBeat) + i] * l[(size_t)(k * outPerBeat) + i];
                    for (int i = skipSrc; i < (int)srcPerBeat - skipSrc; ++i) { const float v = mixed.getSample(0, ((int)(k * srcPerBeat) + i) % L); es += (double)v * v; }
                    worstDb = std::max(worstDb, std::fabs(10.0 * std::log10((eo / (outPerBeat - 2 * skipOut) + 1e-12) / (es / (srcPerBeat - 2 * skipSrc) + 1e-12))));
                }
                std::vector<double> sorted = lags;
                std::sort(sorted.begin(), sorted.end());
                const double median = sorted[sorted.size() / 2], worst = sorted.back();
                char text[160];
                std::snprintf(text, sizeof(text), "fixture 124 bpm at %.0f bpm: clicks on the music (%d beats) off their beat by median %.1f ms, worst %.1f ms; level per beat within %.2f dB",
                              bpm, (int)lags.size(), median, worst, worstDb);
                // Near half speed (70 bpm) a click may land a little further off: it is in up to four grains.
                check(median <= 7.0 && worst <= (bpm < 80.0 ? 20.0 : 12.0) && worstDb <= 1.5, text);
            }
        }
    }

    // 12. Bar 1 is the file's first sample, never the loop's tail: a track silent for its first half
    //     and loud in its last, from beat 0 at 80 and 120 bpm (loop on), and after a count-in from
    //     beat -4 with loop off that comes back on 30 ms after bar 1: exact silence until the
    //     loud half is due.
    for (const double bpm : {80.0, 120.0}) {
        for (const bool countIn : {false, true}) {
            const double sr = 48000.0;
            Rig r(sr, bpm);
            auto t = std::make_unique<BackingTrackBuffer>();
            t->sampleRate = sr;
            t->length = 48000;
            t->audio.setSize(2, 48000);
            t->audio.clear();
            for (int i = 24000; i < 48000; ++i)
                for (int ch = 0; ch < 2; ++ch) t->audio.setSample(ch, i, 0.5f * (float)std::sin(i * 0.05));
            r.install(std::move(t), 100);
            if (countIn) { r.aux.auxSettings().setTrackLoop(false); r.transport.setPosition(-4.0); }
            r.transport.start();
            const int bar1 = countIn ? (int)std::llround(4 * 60.0 / bpm * sr) : 0;
            bool restored = !countIn;
            std::vector<float> l;
            render(r, sr, bar1 + 48000, l, [&](int done) {
                if (!restored && done >= bar1 + (int)(0.03 * sr)) { r.aux.auxSettings().setTrackLoop(true); restored = true; }
            });
            // The loud half starts 0.5 s into the track: 0.5 × 100 / bpm s after bar 1, less the search.
            const int quietUntil = bar1 + (int)((0.5 * 100.0 / bpm - AuxProcessor::kGrainSearchSeconds - 0.005) * sr);
            float before = 0.0f;
            for (int i = 0; i < quietUntil; ++i) before = std::max(before, std::fabs(l[(size_t)i]));
            char text[140];
            std::snprintf(text, sizeof(text), "%s at %.0f bpm: peak %.5f before the track's loud half is due (its tail must not bleed in)",
                          countIn ? "count-in from beat -4, loop back on after bar 1," : "start at beat 0", bpm, before);
            check(before == 0.0f, text);
        }
    }

    // 13. A swap to a track without a tempo while stretching: after the 5 ms crossfade the new
    //     track plays plainly, sample after sample, and the ratio reads 1.
    {
        const double sr = 48000.0;
        const int L = 200000;
        juce::AudioBuffer<float> ref;
        { auto t = makeTrack(L, sr, 1.7); ref.makeCopyOf(t->audio); }
        Rig r(sr, 120);
        r.install(sineTrack(220.0, sr, 2.0), 100);
        r.transport.start();
        int swapAt = -1;
        std::vector<float> l;
        render(r, sr, 120000, l, [&](int done) {
            if (swapAt < 0 && done >= 30000) { r.holder.install(makeTrack(L, sr, 1.7), {}, r.holder.beginLoad()); swapAt = done; }
        });
        const int from = swapAt + 2048;
        int p0 = -1;
        for (int p = 0; p < L && p0 < 0; ++p)
            if (ref.getSample(0, p) == l[(size_t)from] && ref.getSample(0, (p + 1) % L) == l[(size_t)from + 1]) p0 = p;
        int off = p0 < 0 ? -1 : 0;
        for (int i = from; p0 >= 0 && i < 120000; ++i) off += l[(size_t)i] == ref.getSample(0, (p0 + i - from) % L) ? 0 : 1;
        const double ratio = r.aux.diag.trackTempoRatio.load();
        check(off == 0 && ratio == 1.0, "swap to a track without a tempo mid-stretch: plain after the fade, "
              + std::to_string(off) + " samples off, ratio " + std::to_string(ratio));
    }

    // 14. Loop toggled while stretching: on, off and on again inside the track keep the sound going
    //     with no step above the sine's own; with loop on it plays on past the end.
    {
        const double sr = 48000.0;
        Rig r(sr, 120);
        r.aux.auxSettings().setTrackLoop(false);
        r.install(sineTrack(220.0, sr, 1.0), 100);
        r.transport.start();
        int phase = 0;
        std::vector<float> l;
        render(r, sr, 144000, l, [&](int done) {
            if (phase == 0 && done >= 9000) { r.aux.auxSettings().setTrackLoop(true); ++phase; }
            if (phase == 1 && done >= 18000) { r.aux.auxSettings().setTrackLoop(false); ++phase; }
            if (phase == 2 && done >= 27000) { r.aux.auxSettings().setTrackLoop(true); ++phase; }
        });
        float step = 0.0f, lowest = 1.0f;
        for (size_t i = 1; i < l.size(); ++i) step = std::max(step, std::fabs(l[i] - l[i - 1]));
        for (int from = 4800; from + 4800 <= 144000; from += 2400) {
            double e = 0.0;
            for (int i = from; i < from + 4800; ++i) e += (double)l[(size_t)i] * l[(size_t)i];
            lowest = std::min(lowest, (float)std::sqrt(e / 4800));
        }
        char text[140];
        std::snprintf(text, sizeof(text), "loop toggled mid-stretch: largest step %.4f (sine's own 0.0144), lowest rms %.3f past the end too", step, lowest);
        check(step < 0.02f && lowest > 0.3f, text);
    }

    check(allocs == 0 && frees == 0, "render: " + std::to_string(allocs) + " allocations, " + std::to_string(frees) + " frees");
    std::printf("btf   backing track follows the tempo  %-4s\n%s", failures == 0 ? "PASS" : "FAIL", detail.c_str());
    std::fflush(stdout);
    return failures == 0;
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

// A swap that lands inside acquire(), after its first read of the published pointer and before
// the re-validation (the seam runs the control side right there): the reader moves to the new
// buffer (or to none, for an unload) in that block and fades the old one out, so collect() must
// keep the old buffer until the fade-out ends. Then it is freed. Deterministic replay of the
// race the threaded `bts` case hits at random (a freed buffer read by the fade-out).
static bool runBackingTrackSwapInAcquire() {
    const double sr = 48000.0;
    const int fade = (int)(AuxProcessor::kFadeSeconds * sr);
    BackingTrack holder;
    TransportEngine transport;
    AuxProcessor aux;
    aux.setBackingTrack(&holder);
    aux.setTransport(&transport);
    aux.auxSettings().setTrackLevel(1.0);
    aux.setRateAndBufferSizeDetails(sr, 128);
    aux.prepareToPlay(sr, 128);
    transport.start();
    juce::AudioBuffer<float> buf(2, 128);
    juce::MidiBuffer midi;
    uint64_t allocs = 0;
    auto block = [&] {
        tCountAllocs = true;
        transport.advanceBlock(128, sr);
        aux.processBlock(buf, midi);
        tCountAllocs = false;
        allocs += gRenderAllocs.exchange(0) + gRenderFrees.exchange(0);
    };
    gRenderAllocs.store(0); gRenderFrees.store(0);
    gAcquireHookCalls = 0;
    holder.install(constantTrack(20000, sr, 0.5f), {}, holder.beginLoad());
    for (int i = 0; i < 10; ++i) block();
    const float before = buf.getSample(0, 127);

    // Replace inside acquire(): the old buffer fades out over `fade` samples (2 blocks).
    auto next = constantTrack(20000, sr, -0.5f);
    gAcquireHook = [&] { holder.install(std::move(next), {}, holder.beginLoad()); };
    block();
    const float firstAfter = buf.getSample(0, 0); // ≈ 0.5: the old buffer still sounds at full gain
    const int pendingSwap = holder.collect();     // 1: the fading buffer is still in use
    const int liveSwap = BackingTrackBuffer::live.load();
    // With the old buffer freed the next blocks would read freed memory: stop there.
    const bool swapKept = pendingSwap == 1 && liveSwap == 2;
    int pendingSwapAfter = -2, liveSwapAfter = -1, pendingUnload = -2, liveUnload = -1, pendingEnd = -2, liveEnd = -1;
    if (swapKept) {
        for (int i = 0; i < fade / 128 + 3; ++i) block();
        pendingSwapAfter = holder.collect(); // 0: fade-out over, freed
        liveSwapAfter = BackingTrackBuffer::live.load();
        // Unload inside acquire(): the same for the fade-out to silence.
        gAcquireHook = [&] { holder.unload(); };
        block();
        pendingUnload = holder.collect();
        liveUnload = BackingTrackBuffer::live.load();
        if (pendingUnload == 1 && liveUnload == 1) {
            for (int i = 0; i < fade / 128 + 3; ++i) block();
            pendingEnd = holder.collect();
            liveEnd = BackingTrackBuffer::live.load();
        }
    }
    gAcquireHook = nullptr;
    const bool seam = gAcquireHookCalls == (swapKept ? 2 : 1);
    const bool pass = seam && swapKept && before == 0.5f && std::fabs(firstAfter - 0.5f) < 0.01f
                   && pendingSwapAfter == 0 && liveSwapAfter == 1 && pendingUnload == 1 && liveUnload == 1
                   && pendingEnd == 0 && liveEnd == 0 && allocs == 0;
    std::printf("bsr   swap / unload inside acquire(): old buffer kept for its fade-out, then freed  %-4s"
                " seam calls %d; swap: retired %d live %d, after fade %d live %d; unload: retired %d live %d,"
                " after fade %d live %d; first sample after swap %.4f; allocs %llu\n",
                pass ? "PASS" : "FAIL", gAcquireHookCalls, pendingSwap, liveSwap, pendingSwapAfter, liveSwapAfter,
                pendingUnload, liveUnload, pendingEnd, liveEnd, firstAfter, (unsigned long long)allocs);
    std::fflush(stdout);
    if (!swapKept) {
        // The aux still holds the freed buffer as its tail: detach it without rendering again.
        aux.setBackingTrack(nullptr);
    }
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
    // Swing while the transport plays, across block boundaries: 1/32 at 200 bpm (L = 1800), swing
    // 0.45 (odd steps + 810), in 1024- and 471-sample blocks. Every step fires exactly once, in
    // order, the odd ones at the swung offset: a grid line that passed near the end of a block
    // while its swung offset lies in the next block is still due there, not a transport jump.
    for (int blockSize : {1024, 471}) {
        Arpeggiator arp;
        ArpSettings s = arpBase(); s.rate = ArpRate::ThirtySecond; s.swing = 0.45f; s.gate = 0.3f;
        settle(arp, s, {60});
        const auto on = arpOnTimes(runArpBlocks(arp, 40 * 1800 + 100, std::vector<int>{blockSize}, 200.0, true, 0.0, allocs));
        std::vector<int64_t> want;
        for (int k = 0; k <= 40; ++k) want.push_back((int64_t)k * 1800 + ((k & 1) ? 810 : 0));
        check(on == want, "swing playing " + std::to_string(blockSize) + ": " + std::to_string(on.size()) + " hits " + arpList(on, 8));
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

//==============================================================================
// Drum machine (ios/DrumMachine.h, pure C++) and the drum part through PolySynthProcessor.

// Renders `total` samples of `dm` in `block`-sample blocks into `out` (allocation-counted);
// `before(done)` runs before each block.
static void renderDrums(DrumMachine& dm, int total, int block, std::vector<float>& out,
                        const std::function<void(int)>& before = {}) {
    out.assign((size_t)total, 0.0f);
    for (int done = 0; done < total;) {
        const int n = std::min(block, total - done);
        if (before) before(done);
        tCountAllocs = true;
        dm.render(&out[(size_t)done], n);
        tCountAllocs = false;
        done += n;
    }
}

static double rmsOf(const std::vector<float>& x, int from, int to) {
    from = std::max(0, from);
    to = std::min((int)x.size(), to);
    if (to <= from) return 0.0;
    double acc = 0.0;
    for (int i = from; i < to; ++i) acc += (double)x[(size_t)i] * x[(size_t)i];
    return std::sqrt(acc / (to - from));
}

// Sample after which the output is exactly 0 to the end (0 if it is all 0).
static int lastNonZero(const std::vector<float>& x) {
    for (int i = (int)x.size() - 1; i >= 0; --i)
        if (x[(size_t)i] != 0.0f) return i + 1;
    return 0;
}

static bool cleanSamples(const std::vector<float>& x, float limit) {
    for (float v : x)
        if (!std::isfinite(v) || std::fabs(v) > limit || std::fpclassify(v) == FP_SUBNORMAL) return false;
    return true;
}

static bool runDrums() {
    const double sr = 48000.0;
    int failures = 0;
    std::string detail;
    auto check = [&](bool ok, const std::string& what) { if (!ok) { ++failures; detail += " [" + what + "]"; } };
    gRenderAllocs.store(0); gRenderFrees.store(0);

    // 1. Every voice: sounds from sample 0, its envelope decays, ends in exact silence (the voice
    //    goes idle), finite, bounded, no subnormals; decay 0 < 0.5 < 1 in length.
    std::string lengths;
    for (int voice = 0; voice < kDrumVoices; ++voice) {
        int len[3] = {0, 0, 0};
        const double decays[3] = {0.0, 0.5, 1.0};
        const std::string name = kDrumVoiceNames[voice];
        for (int d = 0; d < 3; ++d) {
            DrumMachine dm;
            dm.prepare(sr);
            dm.params().setVoiceParam(voice, kDrumParamDecay, decays[d]);
            std::vector<float> out;
            renderDrums(dm, (int)(6.0 * sr), 128, out, [&](int done) { if (done == 0) dm.trigger(voice, 127); });
            const std::string tag = name + " decay " + std::to_string(decays[d]).substr(0, 3);
            check(cleanSamples(out, 4.0f), tag + " finite/no subnormal/bounded");
            const double first = rmsOf(out, 0, (int)(0.02 * sr));
            check(first > 0.005, tag + " sounds in its first 20 ms (rms " + std::to_string(first) + ")");
            check(out[0] != 0.0f || out[1] != 0.0f || out[2] != 0.0f, tag + " starts on sample 0");
            len[d] = lastNonZero(out);
            check(len[d] < (int)(5.5 * sr), tag + " ends in exact silence");
            check(!dm.isVoiceActive(voice), tag + " idle after its tail");
            // Decays: each 50 ms window after the first 60 ms is quieter than the one 100 ms before.
            const int w = (int)(0.05 * sr);
            bool decaying = true;
            for (int a = (int)(0.06 * sr); a + 3 * w < len[d]; a += w)
                if (rmsOf(out, a + 2 * w, a + 3 * w) > rmsOf(out, a, a + w) * 1.05 + 1e-6) decaying = false;
            check(decaying, tag + " envelope decays");
        }
        check(len[0] < len[1] && len[1] < len[2], name + " decay scales its length");
        lengths += " " + name + ":" + std::to_string((int)std::lround(len[1] * 1000.0 / sr));
    }

    // 2. Choke: an open hat rings past 300 ms alone; a closed hat 50 ms in ends it within 40 ms.
    {
        DrumMachine alone, choked;
        alone.prepare(sr); choked.prepare(sr);
        alone.params().setVoiceParam(kDrumClosedHat, kDrumParamLevel, 0.0); // isolate the open hat
        choked.params().setVoiceParam(kDrumClosedHat, kDrumParamLevel, 0.0);
        std::vector<float> a, c;
        renderDrums(alone, (int)(0.5 * sr), 128, a, [&](int done) { if (done == 0) alone.trigger(kDrumOpenHat, 127); });
        const int chokeAt = 2400 / 128 * 128 + 128; // first block start after 50 ms
        renderDrums(choked, (int)(0.5 * sr), 128, c, [&](int done) {
            if (done == 0) choked.trigger(kDrumOpenHat, 127);
            if (done == chokeAt) choked.trigger(kDrumClosedHat, 127);
        });
        check(rmsOf(a, (int)(0.3 * sr), (int)(0.35 * sr)) > 1e-3, "open hat rings alone");
        check(lastNonZero(c) < chokeAt + (int)(0.04 * sr), "closed hat chokes the open hat within 40 ms (ended at "
              + std::to_string(lastNonZero(c)) + ")");
        check(!choked.isVoiceActive(kDrumOpenHat), "choked open hat idle");
        // An open hat does not choke the closed hat.
        DrumMachine dm;
        dm.prepare(sr);
        std::vector<float> o;
        renderDrums(dm, 256, 128, o, [&](int done) {
            if (done == 0) { dm.trigger(kDrumClosedHat, 127); dm.trigger(kDrumOpenHat, 127); }
        });
        check(dm.isVoiceActive(kDrumClosedHat) && dm.isVoiceActive(kDrumOpenHat), "open hat does not choke closed hat");
    }

    // 3. Retrigger restarts the voice: after the declick, a second hit is sample-exact the same as a
    //    hit on a fresh machine (low tom, tone 0: no noise); the declick has no larger step than the attack.
    {
        DrumMachine re, fresh;
        re.prepare(sr); fresh.prepare(sr);
        re.params().setVoiceParam(kDrumLowTom, kDrumParamTone, 0.0);
        fresh.params().setVoiceParam(kDrumLowTom, kDrumParamTone, 0.0);
        const int at = 128 * 40;
        std::vector<float> r, f;
        renderDrums(re, at + 9600, 128, r, [&](int done) { if (done == 0 || done == at) re.trigger(kDrumLowTom, 100); });
        renderDrums(fresh, 9600, 128, f, [&](int done) { if (done == 0) fresh.trigger(kDrumLowTom, 100); });
        bool same = true;
        for (int i = DrumMachine::kDeclickSamples; i < 9600; ++i)
            if (r[(size_t)(at + i)] != f[(size_t)i]) { same = false; break; }
        check(same, "retrigger == fresh hit after the declick");
        float jump = 0.0f;
        for (int i = at - 4; i < at + DrumMachine::kDeclickSamples + 4; ++i)
            jump = std::max(jump, std::fabs(r[(size_t)i] - r[(size_t)i - 1]));
        float freshJump = 0.0f;
        for (int i = 1; i < 200; ++i) freshJump = std::max(freshJump, std::fabs(f[(size_t)i] - f[(size_t)i - 1]));
        check(jump <= freshJump + 0.05f, "retrigger declick step " + std::to_string(jump));
        check(re.diag.hits.load() == 2, "hits counted");
    }

    // 4. Parameters clamp; non-finite / out of range are ignored.
    {
        DrumParamStore s;
        check(s.setVoiceParam(0, kDrumParamTune, 100.0) && s.voice(0).tune == 24.0f, "tune clamps to 24");
        check(s.setVoiceParam(0, kDrumParamTune, -100.0) && s.voice(0).tune == -24.0f, "tune clamps to -24");
        check(s.setVoiceParam(1, kDrumParamDecay, 2.0) && s.voice(1).decay == 1.0f, "decay clamps to 1");
        check(s.setVoiceParam(1, kDrumParamTone, -1.0) && s.voice(1).tone == 0.0f, "tone clamps to 0");
        check(s.setVoiceParam(7, kDrumParamLevel, 1.5) && s.voice(7).level == 1.0f, "level clamps to 1");
        check(!s.setVoiceParam(2, kDrumParamLevel, std::nan("")) && s.voice(2).level == kDrumVoiceDefaults.level, "NaN ignored");
        check(!s.setVoiceParam(2, kDrumParamTune, INFINITY) && s.voice(2).tune == 0.0f, "inf ignored");
        check(!s.setVoiceParam(kDrumVoices, kDrumParamLevel, 0.5) && !s.setVoiceParam(-1, kDrumParamLevel, 0.5), "voice out of range");
        check(!s.setVoiceParam(0, kDrumParamCount, 0.5), "param out of range");
        check(s.setKitLevel(-1.0) && s.kit().level == 0.0f, "kit level clamps");
        check(s.setKitDrive(5.0) && s.kit().drive == 1.0f, "drive clamps");
        check(!s.setKitLevel(std::nan("")) && s.kit().level == 0.0f, "kit NaN ignored");
        s.setKitMute(true);
        check(s.kit().mute, "mute stored");
        s.reset();
        check(s.voice(0).tune == 0.0f && s.kit().level == kDrumKitDefaults.level && !s.kit().mute, "reset");
        check(drumParamIndex("tune") == 0 && drumParamIndex("level") == 3 && drumParamIndex("lev") == -1
              && drumParamIndex("levels") == -1 && drumParamIndex(nullptr) == -1, "param names");
    }

    // 5. Extremes (tune ±24, decay 0/1, tone 0/1, level 1, drive 1, all voices at once, odd blocks):
    //    finite, bounded, no subnormal output, ends silent.
    for (int combo = 0; combo < 4; ++combo) {
        DrumMachine dm;
        dm.prepare(sr);
        for (int v = 0; v < kDrumVoices; ++v) {
            dm.params().setVoiceParam(v, kDrumParamTune, (combo & 1) ? 24.0 : -24.0);
            dm.params().setVoiceParam(v, kDrumParamDecay, (combo & 2) ? 1.0 : 0.0);
            dm.params().setVoiceParam(v, kDrumParamTone, (combo & 1) ? 0.0 : 1.0);
            dm.params().setVoiceParam(v, kDrumParamLevel, 1.0);
        }
        dm.params().setKitLevel(1.0);
        dm.params().setKitDrive(combo == 3 ? 1.0 : 0.0);
        std::vector<float> out;
        renderDrums(dm, (int)(6.0 * sr), 471, out, [&](int done) {
            if (done == 0 || done == 471 * 30) for (int v = 0; v < kDrumVoices; ++v) dm.trigger(v, 127);
        });
        check(cleanSamples(out, 8.0f), "extremes combo " + std::to_string(combo) + " finite/bounded");
        check(lastNonZero(out) < (int)out.size(), "extremes combo " + std::to_string(combo) + " ends silent");
    }

    // 6. Kit mute silences exactly; unmute brings the sound back; muting mid-sound ramps over one
    //    block; voice level 0 is silent.
    {
        DrumMachine dm;
        dm.prepare(sr);
        dm.params().setKitMute(true);
        std::vector<float> out;
        renderDrums(dm, 4800, 128, out, [&](int done) { if (done == 0) dm.trigger(kDrumKick, 127); });
        bool zero = true;
        for (float v : out) zero = zero && v == 0.0f;
        check(zero, "muted kit is exact 0");
        dm.params().setKitMute(false);
        renderDrums(dm, 4800, 128, out, [&](int done) { if (done == 0) dm.trigger(kDrumKick, 127); });
        check(rmsOf(out, 128, 4800) > 0.01, "unmuted kit sounds");
        dm.params().setKitMute(true);
        renderDrums(dm, 1280, 128, out);
        zero = true;
        for (int i = 128; i < 1280; ++i) zero = zero && out[(size_t)i] == 0.0f;
        check(zero && out[0] != 0.0f, "mute mid-sound: one-block ramp then exact 0");
        dm.params().setKitMute(false);
        dm.params().setVoiceParam(kDrumSnare, kDrumParamLevel, 0.0);
        DrumMachine quiet;
        quiet.prepare(sr);
        quiet.params().setVoiceParam(kDrumSnare, kDrumParamLevel, 0.0);
        renderDrums(quiet, (int)(1.0 * sr), 128, out, [&](int done) { if (done == 0) quiet.trigger(kDrumSnare, 127); });
        zero = true;
        for (float v : out) zero = zero && v == 0.0f;
        check(zero, "voice level 0 is exact 0");
    }

    // 7. Out-of-range hits are ignored; an idle machine leaves the buffer untouched; velocity scales;
    //    peak / hit counters move and the peak decays; drive changes the sound.
    {
        DrumMachine dm;
        dm.prepare(sr);
        dm.trigger(kDrumVoices, 127); dm.trigger(-1, 127);
        check(dm.diag.hits.load() == 0 && dm.activeVoiceCount() == 0, "out-of-range hit ignored");
        std::vector<float> buf(128, 0.25f);
        dm.render(buf.data(), 128);
        bool untouched = true;
        for (float v : buf) untouched = untouched && v == 0.25f;
        check(untouched, "idle machine leaves the buffer untouched");
        std::vector<float> loud, soft;
        DrumMachine a, b;
        a.prepare(sr); b.prepare(sr);
        a.params().setVoiceParam(kDrumLowTom, kDrumParamTone, 0.0);
        b.params().setVoiceParam(kDrumLowTom, kDrumParamTone, 0.0);
        renderDrums(a, 4800, 128, loud, [&](int done) { if (done == 0) a.trigger(kDrumLowTom, 127); });
        renderDrums(b, 4800, 128, soft, [&](int done) { if (done == 0) b.trigger(kDrumLowTom, 32); });
        const double ratio = rmsOf(soft, 0, 4800) / std::max(1e-9, rmsOf(loud, 0, 4800));
        check(std::fabs(ratio - 32.0 / 127.0) < 0.01, "velocity scales linearly (" + std::to_string(ratio) + ")");
        check(a.diag.hits.load() == 1 && a.diag.peak.load() > 0.1f, "peak diag moves");
        const float p0 = a.diag.peak.load();
        std::vector<float> rest;
        renderDrums(a, (int)(1.0 * sr), 128, rest);
        check(a.diag.peak.load() < p0 * 0.5f, "peak diag decays");
        DrumMachine c;
        c.prepare(sr);
        c.params().setVoiceParam(kDrumLowTom, kDrumParamTone, 0.0);
        c.params().setKitDrive(1.0);
        std::vector<float> driven;
        renderDrums(c, 4800, 128, driven, [&](int done) { if (done == 0) c.trigger(kDrumLowTom, 127); });
        bool differs = false;
        for (int i = 0; i < 4800; ++i) differs = differs || driven[(size_t)i] != loud[(size_t)i];
        check(differs, "drive changes the sound");
    }

    const uint64_t allocs = gRenderAllocs.load(), frees = gRenderFrees.load();
    check(allocs == 0 && frees == 0, "render allocations " + std::to_string(allocs) + "/" + std::to_string(frees));
    const bool pass = failures == 0;
    std::printf("dr    drums: every voice decays to exact silence, decay scales, choke, retrigger, clamps, extremes,"
                " mute, velocity, diag  %-4s %d failed checks; lengths ms (decay 0.5):%s allocs %llu%s\n",
                pass ? "PASS" : "FAIL", failures, lengths.c_str(), (unsigned long long)allocs, detail.c_str());
    std::fflush(stdout);
    return pass;
}

// The drum part through PolySynthProcessor and its note queue: a hit pushed from another thread
// renders in the next block, with or without the arpeggiator; drums stay out of the synth peak and
// the note counters; releases do not touch drums; out-of-range hits are not queued; discardPending
// and a full ring count hits apart from notes.
static bool runDrumsInSynth() {
    const double sr = 48000.0;
    int failures = 0;
    std::string detail;
    auto check = [&](bool ok, const std::string& what) { if (!ok) { ++failures; detail += " [" + what + "]"; } };
    gRenderAllocs.store(0); gRenderFrees.store(0);
    auto block = [&](PolySynthProcessor& synth, juce::AudioBuffer<float>& buf, int n) {
        juce::MidiBuffer midi;
        buf.setSize(2, n, false, false, true);
        tCountAllocs = true;
        synth.processBlock(buf, midi);
        tCountAllocs = false;
        return buf.getMagnitude(0, 0, n);
    };
    for (int arpOn = 0; arpOn < 2; ++arpOn) {
        TransportEngine transport;
        PolySynthProcessor synth;
        NoteEventQueue queue;
        synth.setNoteQueue(&queue);
        synth.setTransport(&transport);
        synth.setRateAndBufferSizeDetails(sr, 512);
        synth.prepareToPlay(sr, 512);
        ArpSettings s = ArpSettings::defaults();
        s.enabled = arpOn == 1;
        synth.arp().setSettings(s);
        juce::AudioBuffer<float> buf(2, 512);
        const std::string tag = arpOn ? " (arp on)" : " (arp off)";
        check(block(synth, buf, 128) == 0.0f, "silent before the hit" + tag);
        std::thread producer([&] { queue.pushDrumHit(kDrumKick, 127); });
        producer.join();
        const float p = block(synth, buf, 128);
        check(p > 0.05f, "hit renders within one block" + tag + " peak " + std::to_string(p));
        check(buf.getSample(0, 0) != 0.0f || buf.getSample(0, 1) != 0.0f, "hit starts at the block's first sample" + tag);
        bool stereo = true;
        for (int i = 0; i < 128; ++i) stereo = stereo && buf.getSample(0, i) == buf.getSample(1, i);
        check(stereo, "both channels" + tag);
        check(synth.diag.peak.load() == 0.0f, "drums not in the synth peak" + tag);
        check(synth.drums().diag.peak.load() > 0.05f, "drum peak diag" + tag);
        check(queue.drumHitsQueued.load() == 1 && queue.drumHitsConsumed.load() == 1, "drum counters" + tag);
        check(queue.notesQueued.load() == 0 && queue.notesConsumed.load() == 0, "note counters untouched" + tag);
        check(!synth.arp().isActive(), "arp never gets drum hits" + tag);
        check(synth.diag.lastNoteToRenderMs.load() >= 0.0, "hit latency recorded" + tag);
        check(!queue.pushDrumHit(kDrumVoices, 100) && !queue.pushDrumHit(-1, 100) && queue.drumHitsQueued.load() == 1,
              "out-of-range voice not queued" + tag);
        // A synth note and a drum hit in the same block; allNotesOff never touches drums.
        synth.noteOn(60, 100);
        synth.drumHit(kDrumSnare, 100);
        synth.allNotesOff();
        block(synth, buf, 128);
        check(synth.drums().isVoiceActive(kDrumSnare), "allNotesOff leaves the drum hit" + tag);
        // Kit mute through a shared store (as the engine attaches its process-lifetime one).
        DrumParamStore store;
        synth.drums().setParamStore(&store);
        store.setKitMute(true);
        block(synth, buf, 128);
        for (int i = 0; i < 100; ++i) block(synth, buf, 128); // let the synth voice release
        synth.drumHit(kDrumKick, 127);
        const float muted = block(synth, buf, 128);
        check(muted == 0.0f, "kit mute through the store silences" + tag + " " + std::to_string(muted));
    }
    {
        NoteEventQueue queue;
        queue.pushNoteOn(60, 100);
        queue.pushDrumHit(1, 100);
        queue.pushDrumHit(2, 100);
        queue.discardPending();
        check(queue.notesDiscarded.load() == 1 && queue.drumHitsDiscarded.load() == 2, "discardPending counts by kind");
        int full = 0;
        for (int i = 0; i < NoteEventQueue::kSize + 5; ++i) full += queue.pushDrumHit(0, 100) ? 0 : 1;
        check(full == 6 && queue.drumHitsDropped.load() == 6, "full ring drops hits (counted) " + std::to_string(full));
        check(queue.notesDropped.load() == 0, "drops counted apart from notes");
    }
    const uint64_t allocs = gRenderAllocs.load(), frees = gRenderFrees.load();
    check(allocs == 0 && frees == 0, "render allocations " + std::to_string(allocs) + "/" + std::to_string(frees));
    const bool pass = failures == 0;
    std::printf("drs   drums through PolySynthProcessor + note queue (arp off / on), counters, mute  %-4s %d failed"
                " checks allocs %llu%s\n", pass ? "PASS" : "FAIL", failures, (unsigned long long)allocs, detail.c_str());
    std::fflush(stdout);
    return pass;
}

// Render budget: every voice retriggered every 250 ms at decay 1 (so all always sound), drive on,
// 48 kHz / 128-sample blocks, 20 s of audio. Must render far faster than realtime.
static bool runDrumBudget() {
    const double sr = 48000.0;
    DrumMachine dm;
    dm.prepare(sr);
    for (int v = 0; v < kDrumVoices; ++v) dm.params().setVoiceParam(v, kDrumParamDecay, 1.0);
    dm.params().setKitDrive(0.5);
    const int total = (int)(20.0 * sr);
    const int every = (int)(0.25 * sr) / 128 * 128;
    std::vector<float> buf(128);
    int minActive = kDrumVoices;
    const auto t0 = std::chrono::steady_clock::now();
    for (int done = 0; done < total; done += 128) {
        if (done % every == 0) for (int v = 0; v < kDrumVoices; ++v) dm.trigger(v, 127);
        std::fill(buf.begin(), buf.end(), 0.0f);
        dm.render(buf.data(), 128);
        minActive = std::min(minActive, dm.activeVoiceCount());
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const double ratio = secs / 20.0;
    const double perBlockUs = secs * 1e6 / (double)(total / 128);
    const bool pass = ratio < 0.05 && minActive == kDrumVoices;
    std::printf("drb   drums render budget: all voices sounding, 48k/128, 20 s  %-4s %.4f x realtime (%.2f us per"
                " 2667 us block, limit 0.05), min active %d\n", pass ? "PASS" : "FAIL", ratio, perBlockUs, minActive);
    std::fflush(stdout);
    return pass;
}

// Fundamental of `x` over [from, to) from its rising zero crossings (linearly interpolated).
static double zeroCrossingHz(const std::vector<float>& x, int from, int to, double sr) {
    double first = -1.0, last = -1.0;
    int crossings = 0;
    for (int i = std::max(1, from); i < std::min((int)x.size(), to); ++i) {
        const float a = x[(size_t)i - 1], b = x[(size_t)i];
        if (a < 0.0f && b >= 0.0f) {
            const double t = (double)(i - 1) + (double)a / (double)(a - b);
            if (first < 0.0) first = t;
            last = t;
            ++crossings;
        }
    }
    return crossings > 1 ? (double)(crossings - 1) * sr / (last - first) : 0.0;
}

// Mid tom (voice 8, 0.8.0-rc.2): a ninth voice on the tom model. Sounds and decays to exact
// silence; its pitch is between the low and high tom, centred (log scale) between them; tune
// moves it; the hat choke and the other toms are unaffected; index 8 is accepted by the store,
// the machine and the note queue, 9 is not.
static bool runMidTom() {
    const double sr = 48000.0;
    int failures = 0;
    std::string detail;
    auto check = [&](bool ok, const std::string& what) { if (!ok) { ++failures; detail += " [" + what + "]"; } };
    gRenderAllocs.store(0); gRenderFrees.store(0);
    const int mid = 8;
    double hz[3] = {0.0, 0.0, 0.0};
    if (kDrumVoices < 9) {
        check(false, "kDrumVoices " + std::to_string(kDrumVoices) + " < 9");
    } else {
        check(std::string(kDrumVoiceNames[mid]) == "midTom", "voice 8 is named midTom");
        check(std::string(kDrumVoiceNames[kDrumLowTom]) == "lowTom" && std::string(kDrumVoiceNames[kDrumHighTom]) == "highTom"
              && std::string(kDrumVoiceNames[kDrumRim]) == "rim" && std::string(kDrumVoiceNames[kDrumKick]) == "kick",
              "voices 0-7 keep their names");

        // Sounds from sample 0, decays to exact silence, goes idle.
        {
            DrumMachine dm;
            dm.prepare(sr);
            std::vector<float> out;
            renderDrums(dm, (int)(4.0 * sr), 128, out, [&](int done) { if (done == 0) dm.trigger(mid, 127); });
            check(cleanSamples(out, 4.0f), "finite/bounded/no subnormal");
            check(rmsOf(out, 0, (int)(0.02 * sr)) > 0.05, "non-silent in its first 20 ms");
            check(rmsOf(out, (int)(0.3 * sr), (int)(0.35 * sr)) < rmsOf(out, (int)(0.05 * sr), (int)(0.1 * sr)) * 0.5,
                  "decays");
            const int end = lastNonZero(out);
            check(end > (int)(0.2 * sr) && end < (int)(3.0 * sr), "ends in exact silence (" + std::to_string(end) + ")");
            check(!dm.isVoiceActive(mid) && dm.activeVoiceCount() == 0, "idle after its tail");
            check(dm.diag.hits.load() == 1, "hit counted");
        }

        // Pitch (tone 0: no attack noise), 250-350 ms after the hit (the pitch drop is over).
        const int toms[3] = {kDrumLowTom, mid, kDrumHighTom};
        for (int t = 0; t < 3; ++t) {
            DrumMachine dm;
            dm.prepare(sr);
            dm.params().setVoiceParam(toms[t], kDrumParamTone, 0.0);
            std::vector<float> out;
            renderDrums(dm, (int)(0.4 * sr), 128, out, [&](int done) { if (done == 0) dm.trigger(toms[t], 127); });
            hz[t] = zeroCrossingHz(out, (int)(0.25 * sr), (int)(0.35 * sr), sr);
        }
        check(hz[0] < hz[1] && hz[1] < hz[2], "low < mid < high");
        const double centre = 0.5 * (std::log2(hz[0]) + std::log2(hz[2]));
        check(std::fabs(std::log2(hz[1]) - centre) * 12.0 < 0.25, "mid within a quarter semitone of the toms' centre");
        {
            DrumMachine dm;
            dm.prepare(sr);
            dm.params().setVoiceParam(mid, kDrumParamTone, 0.0);
            dm.params().setVoiceParam(mid, kDrumParamTune, 12.0);
            std::vector<float> out;
            renderDrums(dm, (int)(0.4 * sr), 128, out, [&](int done) { if (done == 0) dm.trigger(mid, 127); });
            const double up = zeroCrossingHz(out, (int)(0.25 * sr), (int)(0.35 * sr), sr);
            check(std::fabs(up / std::max(1.0, hz[1]) - 2.0) < 0.01, "tune +12 doubles the pitch (" + std::to_string(up) + ")");
        }

        // Level 0 is exact 0; velocity scales linearly.
        {
            DrumMachine dm, soft;
            dm.prepare(sr); soft.prepare(sr);
            dm.params().setVoiceParam(mid, kDrumParamLevel, 0.0);
            std::vector<float> out;
            renderDrums(dm, 9600, 128, out, [&](int done) { if (done == 0) dm.trigger(mid, 127); });
            bool zero = true;
            for (float v : out) zero = zero && v == 0.0f;
            check(zero, "level 0 is exact 0");
            DrumMachine loud;
            loud.prepare(sr);
            loud.params().setVoiceParam(mid, kDrumParamTone, 0.0);
            soft.params().setVoiceParam(mid, kDrumParamTone, 0.0);
            std::vector<float> l, s;
            renderDrums(loud, 4800, 128, l, [&](int done) { if (done == 0) loud.trigger(mid, 127); });
            renderDrums(soft, 4800, 128, s, [&](int done) { if (done == 0) soft.trigger(mid, 32); });
            const double ratio = rmsOf(s, 0, 4800) / std::max(1e-9, rmsOf(l, 0, 4800));
            check(std::fabs(ratio - 32.0 / 127.0) < 0.01, "velocity scales linearly");
        }

        // Hat choke unaffected: with the mid tom sounding, a closed hat still ends the open hat within
        // 40 ms; the mid tom (tone 0) renders sample for sample as it does alone (not choked).
        {
            DrumMachine alone, both;
            alone.prepare(sr); both.prepare(sr);
            for (DrumMachine* m : {&alone, &both}) {
                m->params().setVoiceParam(mid, kDrumParamTone, 0.0);
                m->params().setVoiceParam(kDrumClosedHat, kDrumParamLevel, 0.0);
                m->params().setVoiceParam(kDrumOpenHat, kDrumParamLevel, 0.0);
            }
            const int chokeAt = 2400 / 128 * 128 + 128;
            std::vector<float> a, b;
            renderDrums(alone, (int)(0.5 * sr), 128, a, [&](int done) { if (done == 0) alone.trigger(mid, 127); });
            renderDrums(both, (int)(0.5 * sr), 128, b, [&](int done) {
                if (done == 0) { both.trigger(mid, 127); both.trigger(kDrumOpenHat, 127); }
                if (done == chokeAt) both.trigger(kDrumClosedHat, 127);
            });
            check(a == b, "closed / open hats do not touch the mid tom");
            check(both.isVoiceActive(mid), "mid tom still sounding after the choke");
            check(!both.isVoiceActive(kDrumOpenHat), "open hat choked with the mid tom sounding");

            DrumMachine hats;
            hats.prepare(sr);
            hats.params().setVoiceParam(kDrumClosedHat, kDrumParamLevel, 0.0);
            std::vector<float> c;
            renderDrums(hats, (int)(0.5 * sr), 128, c, [&](int done) {
                if (done == 0) { hats.trigger(kDrumOpenHat, 127); hats.trigger(mid, 127); }
                if (done == chokeAt) hats.trigger(kDrumClosedHat, 127);
            });
            check(lastNonZero(c) > chokeAt + (int)(0.04 * sr), "mid tom keeps sounding past the choke");
            check(!hats.isVoiceActive(kDrumOpenHat) && hats.isVoiceActive(mid),
                  "closed hat chokes the open hat, not the mid tom (open hat audible)");

            // A mid tom hit does not choke the open hat or either tom.
            DrumMachine dm;
            dm.prepare(sr);
            std::vector<float> o;
            renderDrums(dm, 256, 128, o, [&](int done) {
                if (done == 0) { dm.trigger(kDrumOpenHat, 127); dm.trigger(kDrumLowTom, 127); dm.trigger(kDrumHighTom, 127); }
                if (done == 128) dm.trigger(mid, 127);
            });
            check(dm.isVoiceActive(kDrumOpenHat) && dm.isVoiceActive(kDrumLowTom) && dm.isVoiceActive(kDrumHighTom)
                  && dm.isVoiceActive(mid), "mid tom chokes nothing");
        }

        // Index range: 8 accepted, 9 rejected (store, machine, queue).
        {
            DrumParamStore s;
            check(s.setVoiceParam(mid, kDrumParamDecay, 0.25) && s.voice(mid).decay == 0.25f, "store accepts voice 8");
            check(!s.setVoiceParam(9, kDrumParamDecay, 0.25) && !s.setVoiceParam(-1, kDrumParamDecay, 0.25),
                  "store rejects 9 / -1");
            s.reset();
            check(s.voice(mid).decay == kDrumVoiceDefaults.decay, "reset covers voice 8");
            DrumMachine dm;
            dm.prepare(sr);
            dm.trigger(9, 127);
            check(dm.activeVoiceCount() == 0 && dm.diag.hits.load() == 0, "machine ignores voice 9");
            NoteEventQueue q;
            check(q.pushDrumHit(mid, 100), "queue accepts voice 8");
            check(!q.pushDrumHit(9, 100) && q.drumHitsQueued.load() == 1, "queue rejects voice 9");
        }
    }

    const uint64_t allocs = gRenderAllocs.load(), frees = gRenderFrees.load();
    check(allocs == 0 && frees == 0, "render allocations " + std::to_string(allocs) + "/" + std::to_string(frees));
    const bool pass = failures == 0;
    std::printf("drm   drums: mid tom (voice 8) sounds, decays, pitch between the toms, tune, choke unaffected,"
                " range  %-4s %d failed checks; Hz low %.1f mid %.1f high %.1f allocs %llu%s\n",
                pass ? "PASS" : "FAIL", failures, hz[0], hz[1], hz[2], (unsigned long long)allocs, detail.c_str());
    std::fflush(stdout);
    return pass;
}

// src/drumDefaults.ts lists the same defaults as kDrumVoiceDefaults / kDrumKitDefaults.
static bool runDrumDefaultsMatchJs() {
    const juce::File file(juce::String(HARNESS_JS_SRC) + "/drumDefaults.ts");
    const juce::String text = file.loadFileAsString();
    auto body = [&](const char* name) {
        const juce::String from = text.fromFirstOccurrenceOf(juce::String("export const ") + name, false, false);
        return from.fromFirstOccurrenceOf("({", false, false).upToFirstOccurrenceOf("})", false, false);
    };
    auto value = [](const juce::String& b, const char* key, bool& found) {
        juce::StringArray pairs;
        pairs.addTokens(b, ",", "");
        pairs.trim();
        for (const juce::String& p : pairs)
            if (p.upToFirstOccurrenceOf(":", false, false).trim() == key) {
                found = true;
                return p.fromFirstOccurrenceOf(":", false, false).trim();
            }
        found = false;
        return juce::String();
    };
    int mismatches = 0;
    std::string detail;
    auto num = [&](const juce::String& b, const char* key, float want) {
        bool found = false;
        const juce::String v = value(b, key, found);
        if (!found || std::fabs(v.getDoubleValue() - (double)want) > 1e-6) { ++mismatches; detail += std::string(" ") + key; }
    };
    const juce::String voice = body("DRUM_VOICE_DEFAULTS"), kit = body("DRUM_KIT_DEFAULTS");
    num(voice, "tune", kDrumVoiceDefaults.tune);
    num(voice, "decay", kDrumVoiceDefaults.decay);
    num(voice, "tone", kDrumVoiceDefaults.tone);
    num(voice, "level", kDrumVoiceDefaults.level);
    num(kit, "level", kDrumKitDefaults.level);
    num(kit, "drive", kDrumKitDefaults.drive);
    bool found = false;
    if (value(kit, "mute", found) != juce::String(kDrumKitDefaults.mute ? "true" : "false") || !found) {
        ++mismatches;
        detail += " mute";
    }
    // export const DRUM_VOICE_COUNT = <n>;
    const juce::String count = text.fromFirstOccurrenceOf("export const DRUM_VOICE_COUNT", false, false)
                                   .fromFirstOccurrenceOf("=", false, false).upToFirstOccurrenceOf(";", false, false).trim();
    if (count.isEmpty() || count.getIntValue() != kDrumVoices) {
        ++mismatches;
        detail += " DRUM_VOICE_COUNT(" + count.toStdString() + ")";
    }
    const bool pass = file.existsAsFile() && mismatches == 0;
    std::printf("drd   src/drumDefaults.ts == DrumMachine.h defaults and voice count  %-4s %d mismatches%s\n", pass ? "PASS" : "FAIL",
                mismatches, detail.c_str());
    std::fflush(stdout);
    return pass;
}

//==============================================================================
// Touch latency windows (ios/LatencyStats.h): percentiles over the last 64, wrap-around, empty,
// non-finite input, a writer racing a reader, and the legs PolySynthProcessor records from the
// queued event's stamps. No allocation in push(), summary() or the render that records.
static bool runLatencyStats() {
    int failed = 0;
    std::string detail;
    auto check = [&](bool ok, const std::string& what) {
        if (!ok) { ++failed; if (detail.size() < 600) detail += "; FAIL " + what; }
    };

    { // empty
        LatencyWindow w;
        const LatencySummary s = w.summary();
        check(s.count == 0 && s.last == -1.0 && s.p50 == -1.0 && s.p95 == -1.0 && s.max == -1.0, "empty window");
    }
    { // fewer than 64: 5, 1, 3 -> last 3, p50 (rank 2) 3, p95 (rank 3) 5, max 5
        LatencyWindow w;
        w.push(5.0); w.push(1.0); w.push(3.0);
        w.push(std::numeric_limits<double>::quiet_NaN());
        w.push(std::numeric_limits<double>::infinity());
        const LatencySummary s = w.summary();
        check(s.count == 3 && s.last == 3.0 && s.p50 == 3.0 && s.p95 == 5.0 && s.max == 5.0,
              "3 values (NaN / inf ignored): count " + std::to_string(s.count) + " p50 " + std::to_string(s.p50));
    }
    { // 1 … 100: the window holds 37 … 100; p50 = rank 32 -> 68, p95 = rank 61 -> 97
        LatencyWindow w;
        for (int i = 1; i <= 100; ++i) w.push((double)i);
        const LatencySummary s = w.summary();
        check(s.count == 100 && s.last == 100.0 && s.p50 == 68.0 && s.p95 == 97.0 && s.max == 100.0,
              "1..100 wrapped: p50 " + std::to_string(s.p50) + " p95 " + std::to_string(s.p95));
        // Descending: the window holds 64 … 1 and the last value is 1.
        LatencyWindow d;
        for (int i = 100; i >= 1; --i) d.push((double)i);
        const LatencySummary t = d.summary();
        check(t.last == 1.0 && t.p50 == 32.0 && t.p95 == 61.0 && t.max == 64.0, "100..1 wrapped");
    }
    { // nearest-rank helper
        double one[1] = {7.0};
        check(latencyPercentile(one, 1, 0.5) == 7.0 && latencyPercentile(one, 1, 0.0) == 7.0, "percentile of 1 value");
        double four[4] = {1.0, 2.0, 3.0, 4.0};
        check(latencyPercentile(four, 4, 0.5) == 2.0 && latencyPercentile(four, 4, 0.95) == 4.0
              && latencyPercentile(four, 4, 1.0) == 4.0, "percentile of 4 values");
    }
    { // no allocation in push / summary
        LatencyWindow w;
        gRenderAllocs.store(0); gRenderFrees.store(0);
        tCountAllocs = true;
        for (int i = 0; i < 1000; ++i) w.push((double)i);
        const LatencySummary s = w.summary();
        tCountAllocs = false;
        check(gRenderAllocs.load() == 0 && gRenderFrees.load() == 0 && s.max == 999.0, "push / summary allocate");
    }
    { // one writer, one reader: every snapshot is consistent (count never goes back, p50 <= p95 <= max,
      // every value one the writer has written)
        LatencyWindow w;
        constexpr int kN = 400000;
        std::atomic<bool> done{false};
        std::thread writer([&] {
            for (int i = 0; i < kN; ++i) w.push((double)i);
            done.store(true);
        });
        uint64_t lastCount = 0;
        int snapshots = 0, bad = 0;
        while (!done.load() || snapshots < 10) {
            const LatencySummary s = w.summary();
            ++snapshots;
            if (s.count < lastCount) ++bad;
            lastCount = s.count;
            if (s.count == 0) continue;
            if (!(s.p50 <= s.p95 && s.p95 <= s.max)) ++bad;
            if (s.max < 0.0 || s.max >= (double)kN || !std::isfinite(s.last)) ++bad;
        }
        writer.join();
        const LatencySummary end = w.summary();
        check(bad == 0 && end.count == (uint64_t)kN && end.last == kN - 1.0 && end.max == kN - 1.0
              && end.p50 == kN - 33.0, "writer vs reader: " + std::to_string(bad) + " bad of "
              + std::to_string(snapshots) + " snapshots");
    }
    double handlerMs = -1.0, queueMs = -1.0, touchMs = -1.0;
    { // the synth records handler / queueToRender / touchToRender for touch-stamped events only
        TouchLatencyStats stats;
        TransportEngine transport;
        PolySynthProcessor synth;
        synth.setTransport(&transport);
        synth.setTouchLatency(&stats);
        synth.setRateAndBufferSizeDetails(48000, 128);
        synth.prepareToPlay(48000, 128);
        juce::AudioBuffer<float> buf(2, 128);
        juce::MidiBuffer midi;
        const juce::int64 now = juce::Time::getHighResolutionTicks();
        const juce::int64 ms = juce::Time::secondsToHighResolutionTicks(0.001);
        // Touch 30 ms ago, handler entered 10 ms ago, pushed now.
        synth.noteOn(60, 100, now - 30 * ms, now - 10 * ms);
        synth.noteOn(62, 100);                   // not from a touch: not recorded
        synth.drumHit(0, 100, now - 30 * ms, 0); // a touch without an entry time: no handler value
        gRenderAllocs.store(0); gRenderFrees.store(0);
        tCountAllocs = true;
        synth.processBlock(buf, midi);
        tCountAllocs = false;
        const uint64_t allocs = gRenderAllocs.load() + gRenderFrees.load();
        const LatencySummary h = stats.handler.summary(), q = stats.queueToRender.summary(),
                             t = stats.touchToRender.summary(), d = stats.delivery.summary();
        handlerMs = h.last; queueMs = q.max; touchMs = t.p50;
        check(h.count == 1 && h.last >= 10.0 && h.last < 15.0, "handler leg " + std::to_string(h.last));
        check(q.count == 2 && q.max >= 0.0 && q.max < 50.0, "queueToRender leg count " + std::to_string(q.count));
        check(t.count == 2 && t.p50 >= 30.0 && t.max < 1000.0 && t.p50 > q.max, "touchToRender leg " + std::to_string(t.p50));
        check(d.count == 0, "delivery is not the synth's");
        check(std::abs(synth.diag.lastTouchToRenderMs.load() - t.last) < 1e-6, "lastTouchToRenderMs == touchToRender.last");
        check(allocs == 0, "render allocations " + std::to_string(allocs));
    }
    const bool pass = failed == 0;
    std::printf("lat   touch latency windows: percentiles, wrap, empty, NaN, writer vs reader, synth legs  %-4s"
                " %d failed checks; synth legs handler %.2f ms, queue->render max %.2f ms, touch->render p50 %.2f ms%s\n",
                pass ? "PASS" : "FAIL", failed, handlerMs, queueMs, touchMs, detail.c_str());
    std::fflush(stdout);
    return pass;
}

// Main-thread stall window (ios/LatencyStats.h StallWindow): longest gap over the last 10 s, slot
// reuse after 10 s, the > 50 ms count.
static bool runStallWindow() {
    int failed = 0;
    std::string detail;
    auto check = [&](bool ok, const std::string& what) {
        if (!ok) { ++failed; detail += "; FAIL " + what; }
    };
    StallWindow w;
    check(w.maxGapMs(100.0) == 0.0 && w.stallCount() == 0, "empty");
    w.addGap(100.2, 16.7);
    w.addGap(100.5, 80.0);
    w.addGap(100.6, 50.0); // exactly 50: not a stall
    check(w.maxGapMs(100.9) == 80.0, "80 within the second");
    check(w.maxGapMs(109.99) == 80.0, "80 still inside 10 s");
    check(w.maxGapMs(110.0) == 0.0, "80 out of the window at 110 s: " + std::to_string(w.maxGapMs(110.0)));
    w.addGap(105.0, 30.0);
    check(w.maxGapMs(110.5) == 30.0, "30 from second 105");
    w.addGap(110.1, 20.0); // reuses second 100's slot
    check(w.maxGapMs(110.2) == 30.0 && w.maxGapMs(115.5) == 20.0, "slot reuse");
    check(w.maxGapMs(99.0) == 0.0, "a gap after `now` is not counted");
    w.addGap(111.0, -5.0);
    w.addGap(111.0, std::numeric_limits<double>::quiet_NaN());
    check(w.stallCount() == 1 && w.tickCount() == 5, "stall count " + std::to_string(w.stallCount()) + " ticks "
          + std::to_string(w.tickCount()));
    const bool pass = failed == 0;
    std::printf("stall main-thread gap window (10 s) and > 50 ms count  %-4s %d failed checks%s\n",
                pass ? "PASS" : "FAIL", failed, detail.c_str());
    std::fflush(stdout);
    return pass;
}

// Main-thread stall log (ios/LatencyStats.h StallLog): only gaps over 50 ms, start = end − gap,
// the last 32 kept oldest first, the median interval between starts, no allocation in addGap /
// copy / intervalP50Ms.
static bool runStallLog() {
    int failed = 0;
    std::string detail;
    auto check = [&](bool ok, const std::string& what) {
        if (!ok) { ++failed; if (detail.size() < 600) detail += "; FAIL " + what; }
    };
    auto near = [](double a, double b) { return std::fabs(a - b) < 1e-6; };
    StallRecord out[StallLog::kSize];
    {
        StallLog log;
        check(log.copy(out) == 0 && StallLog::intervalP50Ms(out, 0) == 0.0, "empty");
        log.addGap(10.0, 16.7);
        log.addGap(10.5, 50.0); // exactly 50: not a stall
        log.addGap(11.0, 100.0);
        log.addGap(11.0, std::numeric_limits<double>::quiet_NaN());
        log.addGap(std::numeric_limits<double>::infinity(), 80.0);
        const int n = log.copy(out);
        check(n == 1 && near(out[0].atSec, 10.9) && out[0].ms == 100.0,
              "one stall at 10.9 s: n " + std::to_string(n) + " at " + std::to_string(n ? out[0].atSec : -1.0));
        check(StallLog::intervalP50Ms(out, n) == 0.0, "one stall has no interval");
    }
    { // a stall every 3 s (one 1 s off): median 3000 ms
        StallLog log;
        const double ends[] = {100.0, 103.0, 106.0, 107.0, 110.0, 113.0};
        for (double e : ends) log.addGap(e, 90.0);
        const int n = log.copy(out);
        const double p50 = StallLog::intervalP50Ms(out, n);
        check(n == 6 && near(p50, 3000.0), "every 3 s: p50 " + std::to_string(p50));
        check(near(out[0].atSec, 99.91) && near(out[5].atSec, 112.91), "oldest first");
    }
    { // wrap-around: 40 stalls 1 s apart keep the last 32 (starts 8 … 39 s, minus the gap)
        StallLog log;
        for (int i = 0; i < 40; ++i) log.addGap((double)i + 0.1, 60.0 + i);
        const int n = log.copy(out);
        check(n == StallLog::kSize && log.count() == 40, "kept 32 of 40");
        check(near(out[0].atSec, 8.1 - 0.068) && out[0].ms == 68.0 && out[31].ms == 99.0, "wrap keeps the newest, oldest first");
        const double p50 = StallLog::intervalP50Ms(out, n);
        check(std::fabs(p50 - 999.0) < 1e-3, "1 s apart, lengths 1 ms longer each: 999 ms: p50 " + std::to_string(p50));
    }
    { // intervals that are not positive (a reader racing the writer) are skipped
        StallRecord r[3] = {{5.0, 60.0}, {4.0, 60.0}, {7.0, 60.0}};
        check(near(StallLog::intervalP50Ms(r, 3), 3000.0), "negative interval skipped");
    }
    { // no allocation
        StallLog log;
        gRenderAllocs.store(0); gRenderFrees.store(0);
        tCountAllocs = true;
        for (int i = 0; i < 1000; ++i) log.addGap((double)i, 51.0 + (i % 7));
        const int n = log.copy(out);
        const double p50 = StallLog::intervalP50Ms(out, n);
        tCountAllocs = false;
        check(gRenderAllocs.load() == 0 && gRenderFrees.load() == 0 && n == 32 && p50 > 0.0, "addGap / copy / p50 allocate");
    }
    const bool pass = failed == 0;
    std::printf("stlog main-thread stall log (last 32 over 50 ms) and median interval  %-4s %d failed checks%s\n",
                pass ? "PASS" : "FAIL", failed, detail.c_str());
    std::fflush(stdout);
    return pass;
}

// Display rate (ios/DisplayRate.h): the hint's sanitizing, the link's frame-rate range per hint and
// screen, and the measured rate (median frame duration over the last 32 ticks). No allocation.
static bool runDisplayRate() {
    using namespace displayrate;
    int failed = 0;
    std::string detail;
    auto check = [&](bool ok, const std::string& what) {
        if (!ok) { ++failed; if (detail.size() < 600) detail += "; FAIL " + what; }
    };
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    check(sanitizeHint(0) == 0 && sanitizeHint(-5) == 0 && sanitizeHint(nan) == 0 && sanitizeHint(inf) == 0
          && sanitizeHint(120) == 120 && sanitizeHint(119.6) == 120 && sanitizeHint(1000) == 240 && sanitizeHint(60) == 60,
          "sanitizeHint");
    auto same = [](const RateRange& r, bool raised, float mn, float mx, float pf) {
        return r.raised == raised && r.minimum == mn && r.maximum == mx && r.preferred == pf;
    };
    check(same(rangeFor(120, 120), true, 80, 120, 120), "120 on a 120 Hz screen: (80, 120, 120)");
    check(same(rangeFor(240, 120), true, 80, 120, 120), "240 clamped to the screen");
    check(same(rangeFor(90, 120), true, 80, 90, 90), "90: (80, 90, 90)");
    check(same(rangeFor(72, 120), true, 72, 72, 72), "72: minimum not above the target");
    check(same(rangeFor(0, 120), false, 60, 60, 60), "no hint on ProMotion: held to 60");
    check(same(rangeFor(60, 120), false, 60, 60, 60), "60 on ProMotion: held to 60");
    check(same(rangeFor(61, 120), true, 61, 61, 61), "61 raises");
    check(same(rangeFor(120, 60), false, 0, 0, 0), "60 Hz screen: no range (default)");
    check(same(rangeFor(120, 0), false, 0, 0, 0), "unknown screen: no range");
    check(same(rangeFor(0, 60), false, 0, 0, 0), "no hint on 60 Hz: no range");
    {
        RateWindow w;
        check(w.medianHz() == 0.0, "empty: 0 Hz");
        for (int i = 0; i < 10; ++i) w.push(1.0 / 120.0);
        check(w.medianHz() == 120.0, "120 Hz: " + std::to_string(w.medianHz()));
        w.push(nan); w.push(-1.0); w.push(0.0); w.push(2.0); w.push(inf);
        check(w.medianHz() == 120.0, "invalid durations ignored");
        for (int i = 0; i < 9; ++i) w.push(1.0 / 60.0); // 10 at 120, 9 at 60: median still 120
        check(w.medianHz() == 120.0, "10 of 19 at 120: " + std::to_string(w.medianHz()));
        for (int i = 0; i < 32; ++i) w.push(1.0 / 60.0);
        check(w.medianHz() == 60.0, "window turned over to 60: " + std::to_string(w.medianHz()));
        w.clear();
        check(w.medianHz() == 0.0, "cleared");
        w.push(1.0 / 120.0); w.push(1.0 / 80.0);
        check(w.medianHz() == 96.0, "two values: mean of the durations (10.42 ms → 96 Hz): " + std::to_string(w.medianHz()));
        w.clear();
        w.push(1.0 / 59.94);
        check(w.medianHz() == 59.9, "rounded to 0.1 Hz: " + std::to_string(w.medianHz()));
    }
    { // no allocation
        RateWindow w;
        gRenderAllocs.store(0); gRenderFrees.store(0);
        tCountAllocs = true;
        for (int i = 0; i < 1000; ++i) w.push(i % 2 ? 1.0 / 120.0 : 1.0 / 60.0);
        const double hz = w.medianHz();
        const RateRange r = rangeFor(sanitizeHint(120.0), 120);
        tCountAllocs = false;
        check(gRenderAllocs.load() == 0 && gRenderFrees.load() == 0 && hz > 0.0 && r.raised, "push / medianHz allocate");
    }
    const bool pass = failed == 0;
    std::printf("rate  display-rate hint range and measured rate (median of 32)  %-4s %d failed checks%s\n",
                pass ? "PASS" : "FAIL", failed, detail.c_str());
    std::fflush(stdout);
    return pass;
}

//==============================================================================
// Device format (ios/DeviceFormat.h) against JUCE's real AudioDeviceManager and a fake device
// that behaves like juce_Audio_ios.cpp: it lists the sizes granted for 64 (then doubling) and
// 4096 frames, asks the "route" for (size + 1) frames when it opens, and defaults to 256. The
// route grants a multiple of `grain` frames between `minFrames` and 4096, at one fixed rate.
struct FakeRoute { double rate; int grain; int minFrames; };
static FakeRoute gFakeRoute{48000.0, 1, 16};

static int fakeRouteGrant(int frames) {
    const int g = std::max(1, gFakeRoute.grain);
    const int rounded = ((frames + g - 1) / g) * g;
    return std::clamp(rounded, gFakeRoute.minFrames, 4096);
}

class IosLikeDevice final : public juce::AudioIODevice {
public:
    IosLikeDevice() : AudioIODevice("Out", "IosLike") { listSizes(); }
    // Like juce_Audio_ios.cpp restart() after a route change: lists the new route's sizes, asks
    // it for the size the device was opened with (+ 1) and restarts the callback in place.
    void routeChange() {
        listSizes();
        current = fakeRouteGrant(target + 1);
        if (callback) { callback->audioDeviceStopped(); callback->audioDeviceAboutToStart(this); }
    }
    // The device changes size without telling anyone (AudioDeviceManager's stored setup goes stale).
    void resizeSilently(int frames) { current = frames; }
    int opens = 0;   // open() calls
    int xruns = 0;   // restarts at 0 on open(), like the iOS device
    int getXRunCount() const noexcept override { return xruns; }
    juce::StringArray getOutputChannelNames() override { return {"L", "R"}; }
    juce::StringArray getInputChannelNames() override { return {}; }
    juce::Array<double> getAvailableSampleRates() override { return {gFakeRoute.rate}; }
    juce::Array<int> getAvailableBufferSizes() override { return sizes; }
    int getDefaultBufferSize() override { return 256; }
    juce::String open(const juce::BigInteger&, const juce::BigInteger& outputs, double, int buffer) override {
        target = buffer > 0 ? buffer : 256;
        current = fakeRouteGrant(target + 1);
        outs = outputs;
        opened = true;
        ++opens;
        xruns = 0;
        return {};
    }
    void close() override { stop(); opened = false; }
    bool isOpen() override { return opened; }
    void start(juce::AudioIODeviceCallback* cb) override { if (cb) { cb->audioDeviceAboutToStart(this); callback = cb; } }
    void stop() override { if (callback) { auto* cb = callback; callback = nullptr; cb->audioDeviceStopped(); } }
    bool isPlaying() override { return callback != nullptr; }
    juce::String getLastError() override { return {}; }
    int getCurrentBufferSizeSamples() override { return current; }
    double getCurrentSampleRate() override { return gFakeRoute.rate; }
    int getCurrentBitDepth() override { return 32; }
    juce::BigInteger getActiveOutputChannels() const override { return outs; }
    juce::BigInteger getActiveInputChannels() const override { return {}; }
    int getOutputLatencyInSamples() override { return 0; }
    int getInputLatencyInSamples() override { return 0; }
private:
    void listSizes() {
        sizes.clear();
        int size = fakeRouteGrant(64 + 1);
        const int longest = fakeRouteGrant(4096 + 1);
        while (size <= longest) { sizes.add(size); size *= 2; }
        sizes.addIfNotAlreadyThere(longest);
    }
    juce::Array<int> sizes;
    int current = 0;
    int target = 256;
    bool opened = false;
    juce::BigInteger outs;
    juce::AudioIODeviceCallback* callback = nullptr;
};

class IosLikeType final : public juce::AudioIODeviceType {
public:
    IosLikeType() : AudioIODeviceType("IosLike") {}
    void scanForDevices() override {}
    juce::StringArray getDeviceNames(bool wantInputNames) const override {
        return wantInputNames ? juce::StringArray() : juce::StringArray("Out");
    }
    int getDefaultDeviceIndex(bool) const override { return 0; }
    int getIndexOfDevice(juce::AudioIODevice* d, bool asInput) const override { return (d && !asInput) ? 0 : -1; }
    bool hasSeparateInputsAndOutputs() const override { return true; }
    juce::AudioIODevice* createDevice(const juce::String& out, const juce::String&) override {
        return out.isEmpty() || out == "Out" ? new IosLikeDevice() : nullptr;
    }
};

static bool runDeviceFormat() {
    int failed = 0;
    std::string detail, rows;
    auto check = [&](bool ok, const std::string& what) {
        if (!ok) { ++failed; detail += "; FAIL " + what; }
    };
    using devicefmt::nearestListedBufferSize;
    check(nearestListedBufferSize({}, 128) == 128, "empty list");
    check(nearestListedBufferSize({64, 128, 256}, 128) == 128, "listed");
    check(nearestListedBufferSize({65, 130, 260}, 128) == 130, "65/130/260 -> 130");
    check(nearestListedBufferSize({96, 192, 384}, 128) == 96, "96/192 -> 96");
    check(nearestListedBufferSize({96, 160}, 128) == 160, "tie -> larger");
    check(nearestListedBufferSize({288, 576}, 128) == 288, "route floor");
    check(devicefmt::chooseDeviceSampleRate(44100.0, 48000.0) == 44100.0, "hardware rate wins over the hint");
    check(devicefmt::chooseDeviceSampleRate(48000.0, 44100.0) == 48000.0, "hardware rate wins (2)");
    check(devicefmt::chooseDeviceSampleRate(0.0, 44100.0) == 44100.0, "hint when the hardware rate is unknown");
    check(devicefmt::chooseDeviceSampleRate(std::numeric_limits<double>::quiet_NaN(), 0.0) == 48000.0, "default");
    check(devicefmt::chooseDeviceSampleRate(24000.0, 48000.0) == 48000.0, "a voice rate is not adopted");
    check(devicefmt::chooseDeviceSampleRate(16000.0, 8000.0) == 48000.0, "nor a voice hint");
    check(devicefmt::chooseDeviceSampleRate(96000.0, 48000.0) == 96000.0, "96 kHz route");
    check(std::abs(devicefmt::ioBufferDurationSeconds(128, 44100.0) - 128.0 / 44100.0) < 1e-12
          && devicefmt::ioBufferDurationSeconds(0, 48000.0) == 0.0 && devicefmt::ioBufferDurationSeconds(128, 0.0) == 0.0,
          "IO buffer duration");

    struct Scenario { const char* name; FakeRoute route; int wantFirst, wantOldRestart, wantNew; };
    const Scenario scenarios[] = {
        // Exact grants at 48 kHz (the built-in speaker of the 2026-10-22 baseline: 256 -> 257).
        {"48k exact", {48000.0, 1, 16}, 131, 257, 131},
        // A route with 32-frame steps at 44.1 kHz: JUCE's default 256 + 1 -> 288, as on the device.
        {"44.1k step 32", {44100.0, 32, 32}, 128, 288, 128},
        // A route that cannot go below 288 frames: nothing to win, every path lands on the floor
        // (plus JUCE's extra frame when it asks for 288 itself).
        {"floor 288", {44100.0, 1, 288}, 288, 289, 288},
    };
    for (const Scenario& s : scenarios) {
        gFakeRoute = s.route;
        juce::AudioDeviceManager manager;
        manager.addAudioDeviceType(std::make_unique<IosLikeType>());
        const juce::String initError = manager.initialise(0, 2, nullptr, true);
        const devicefmt::OpenResult first = devicefmt::applyPreferredSetup(manager, 128, s.route.rate);
        auto granted = [&] { auto* d = manager.getCurrentAudioDevice(); return d ? d->getCurrentBufferSizeSamples() : -1; };
        const int g1 = granted();
        // The recovery path before this change: close + JUCE's restartLastAudioDevice.
        manager.closeAudioDevice();
        manager.restartLastAudioDevice();
        const int gOld = granted();
        // The recovery path now.
        const devicefmt::OpenResult again = devicefmt::reopenWithPreferredSetup(manager, 128, s.route.rate);
        const int gNew = granted();
        const double rate = manager.getCurrentAudioDevice() ? manager.getCurrentAudioDevice()->getCurrentSampleRate() : 0.0;
        check(initError.isEmpty() && first.error.isEmpty() && again.error.isEmpty() && again.reopened,
              std::string(s.name) + ": errors");
        check(g1 == s.wantFirst && gOld == s.wantOldRestart && gNew == s.wantNew && rate == s.route.rate,
              std::string(s.name) + ": got " + std::to_string(g1) + "/" + std::to_string(gOld) + "/" + std::to_string(gNew));
        rows += std::string("; ") + s.name + ": listed " + std::to_string(first.chosenBufferSize) + " -> " + std::to_string(g1)
              + ", old restart " + std::to_string(gOld) + ", new " + std::to_string(gNew);
        manager.closeAudioDevice();
    }
    const bool pass = failed == 0;
    std::printf("bsel  buffer size / rate choice, preferred 128 (first open / old restart / new reopen)  %-4s %d failed checks%s%s\n",
                pass ? "PASS" : "FAIL", failed, rows.c_str(), detail.c_str());
    std::fflush(stdout);
    return pass;
}

// Route changes (ios/DeviceFormat.h bufferChoiceDrifted / applyPreferredSetup) as the engine's
// ensurePreferredFormat runs them: speaker -> Bluetooth (1024-frame floor) -> a stale setup ->
// speaker -> speaker. Checks the device comes back to the preferred size on the speaker, that a
// no-change request reopens nothing, and that each device's xruns are carried exactly once.
static bool runDeviceRoutes() {
    int failed = 0;
    std::string detail, rows;
    auto check = [&](bool ok, const std::string& what) {
        if (!ok) { ++failed; detail += "; FAIL " + what; }
    };
    const FakeRoute speaker{48000.0, 1, 16};
    const FakeRoute bluetooth{48000.0, 1, 1024};
    gFakeRoute = speaker;
    juce::AudioDeviceManager manager;
    manager.addAudioDeviceType(std::make_unique<IosLikeType>());
    check(manager.initialise(0, 2, nullptr, true).isEmpty(), "initialise");
    auto dev = [&] { return dynamic_cast<IosLikeDevice*>(manager.getCurrentAudioDevice()); };
    auto granted = [&] { auto* d = dev(); return d ? d->getCurrentBufferSizeSamples() : -1; };
    auto opens = [&] { auto* d = dev(); return d ? d->opens : -1; };

    int openedSize = 0;       // ExpoJuceAudioEngine.deviceTargetBufferSize
    int lastGranted = 0;      // what the 0.9.0-rc.1 draft compared against
    uint64_t carried = 0;     // sXrunsCarried
    auto record = [&](const devicefmt::OpenResult& r) {
        if (r.opened) openedSize = r.targetBufferSize;
        carried += (uint64_t)r.xrunsBeforeOpen;
        lastGranted = granted();
    };
    // ensurePreferredFormat's buffer half: ask again only when the choice drifted.
    auto routeCheck = [&](devicefmt::OpenResult& r) {
        if (!devicefmt::bufferChoiceDrifted(dev()->getAvailableBufferSizes(), 128, openedSize)) return false;
        r = devicefmt::applyPreferredSetup(manager, 128, 48000.0);
        record(r);
        return true;
    };
    auto step = [&](const char* name) { rows += std::string("; ") + name + " " + std::to_string(granted()); };

    record(devicefmt::applyPreferredSetup(manager, 128, 48000.0));
    check(granted() == 131 && openedSize == 130, "speaker: " + std::to_string(granted()));
    step("speaker");

    // Bluetooth: JUCE's restart in place lands on the route's floor, which is its nearest listed
    // size. The request equals the stored setup: nothing is reopened, no xruns are carried.
    gFakeRoute = bluetooth;
    dev()->routeChange();
    dev()->xruns = 3;
    int before = opens();
    devicefmt::OpenResult r;
    check(routeCheck(r), "bluetooth: choice drifted from 130");
    check(!r.opened && r.xrunsBeforeOpen == 0 && opens() == before && dev()->xruns == 3 && granted() == 1024,
          "bluetooth: no-change request reopened (" + std::to_string(opens() - before) + " opens, "
          + std::to_string(r.xrunsBeforeOpen) + " xruns)");
    step("bluetooth");

    // The device changes size behind AudioDeviceManager's back: the same request is stale, so a
    // new device is opened (at the listed 1024, as a dead-device recovery on Bluetooth would).
    dev()->resizeSilently(2048);
    check(routeCheck(r), "stale: choice still drifted");
    check(r.opened && r.reopened && r.error.isEmpty() && r.xrunsBeforeOpen == 3 && granted() == 1025
          && openedSize == 1024,
          "stale: reopened " + std::to_string(r.reopened) + ", xruns " + std::to_string(r.xrunsBeforeOpen) + ", granted "
          + std::to_string(granted()) + ", opened at " + std::to_string(openedSize));
    step("stale");

    // Back on the speaker. JUCE restarts in place with the 1024 the device was opened with. The
    // draft's check (granted > last granted: 1025 > 1025) missed it; the listed choice does not.
    gFakeRoute = speaker;
    dev()->routeChange();
    check(granted() == 1025 && !(granted() > lastGranted), "speaker after bluetooth: JUCE kept 1024 + 1");
    dev()->xruns = 5;
    check(routeCheck(r), "speaker after bluetooth: choice drifted from 1024");
    check(r.opened && !r.reopened && r.xrunsBeforeOpen == 5 && granted() == 131 && openedSize == 130,
          "speaker after bluetooth: granted " + std::to_string(granted()) + ", xruns " + std::to_string(r.xrunsBeforeOpen));
    step("speaker again");

    // Another route change on the speaker (e.g. an override): JUCE's restart keeps 130 + 1.
    dev()->routeChange();
    before = opens();
    check(!routeCheck(r) && granted() == 131 && opens() == before, "speaker override: asked again");
    check(carried == 8, "xruns carried " + std::to_string(carried) + ", want 3 + 5");
    rows += "; xruns carried " + std::to_string(carried);
    manager.closeAudioDevice();

    const bool pass = failed == 0;
    std::printf("broute route changes keep the preferred buffer (granted frames per step)  %-4s %d failed checks%s%s\n",
                pass ? "PASS" : "FAIL", failed, rows.c_str(), detail.c_str());
    std::fflush(stdout);
    return pass;
}

//==============================================================================
// MIDI byte input (ios/MidiInput.h, ios/ExpoJuceMidiInput.mm).

// A message as a compact string ("on c0 60 100", "off c1 64 0", "pp c0 60 33", "cc c3 74 12",
// "pb c0 8192"), so the vectors read like (and match) the tests of a JS parser with the same rules.
static std::string midiStr(const midiin::Message& m) {
    const std::string ch = " c" + std::to_string(m.channel) + " ";
    switch (m.type) {
        case midiin::kNoteOn: return "on" + ch + std::to_string(m.data1) + " " + std::to_string(m.data2);
        case midiin::kNoteOff: return "off" + ch + std::to_string(m.data1) + " " + std::to_string(m.data2);
        case midiin::kPolyPressure: return "pp" + ch + std::to_string(m.data1) + " " + std::to_string(m.data2);
        case midiin::kControlChange: return "cc" + ch + std::to_string(m.data1) + " " + std::to_string(m.value);
        case midiin::kPitchBend: return "pb" + ch + std::to_string(m.value);
        default: return "?";
    }
}

static std::string joinStrs(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& x : v) s += (s.empty() ? "" : ", ") + x;
    return "[" + s + "]";
}

// Parsers: every vector of the TypeScript reference, plus the raw-packet (serial bridge) rule.
static bool runMidiParsers() {
    int failed = 0;
    std::string detail;
    auto check = [&](bool ok, const std::string& what) {
        if (!ok) { ++failed; if (detail.size() < 900) detail += "; FAIL " + what; }
    };
    using Bytes = std::vector<uint8_t>;
    auto ble = [](midiin::ParserState& s, const Bytes& b) {
        std::vector<std::string> out;
        midiin::parseBleMidiPacket(b.data(), b.size(), s, [&](const midiin::Message& m) { out.push_back(midiStr(m)); });
        return out;
    };
    auto raw = [](midiin::ParserState& s, const Bytes& b) {
        std::vector<std::string> out;
        midiin::parseRawStream(b.data(), b.size(), s, [&](const midiin::Message& m) { out.push_back(midiStr(m)); });
        return out;
    };
    auto expect = [&](const std::vector<std::string>& got, const std::vector<std::string>& want, const std::string& what) {
        check(got == want, what + ": got " + joinStrs(got) + " want " + joinStrs(want));
    };
    using V = std::vector<std::string>;

    { midiin::ParserState s; expect(ble(s, {0x80, 0x80, 0x90, 60, 100}), V{"on c0 60 100"}, "single note on"); }
    { midiin::ParserState s;
      expect(ble(s, {0x81, 0x82, 0x91, 64, 90, 0x85, 0x81, 64, 0}), V{"on c1 64 90", "off c1 64 0"}, "on + off with timestamps"); }
    { midiin::ParserState s;
      expect(ble(s, {0x80, 0x80, 0x90, 60, 100, 0x81, 62, 101, 64, 102}), V{"on c0 60 100", "on c0 62 101", "on c0 64 102"},
             "running status with / without timestamp"); }
    { midiin::ParserState s; expect(ble(s, {0x80, 0x80, 0x90, 60, 0}), V{"off c0 60 0"}, "velocity 0 is a note-off"); }
    { midiin::ParserState s;
      expect(ble(s, {0x80, 0x80, 0x92, 67}), V{}, "split: first half");
      expect(ble(s, {0x80, 80}), V{"on c2 67 80"}, "split: completes on the next packet"); }
    { midiin::ParserState s;
      ble(s, {0x80, 0x80, 0x90, 60, 100});
      expect(ble(s, {0x80, 0x80, 61, 99}), V{"on c0 61 99"}, "running status across packets"); }
    { midiin::ParserState s;
      expect(ble(s, {0x80, 0x80, 0xA0, 60, 33, 0x80, 0xB3, 74, 12, 0x80, 0xE0, 0x00, 0x40}),
             V{"pp c0 60 33", "cc c3 74 12", "pb c0 8192"}, "pressure, cc, bend"); }
    { midiin::ParserState s;
      expect(ble(s, {0x80, 0x80, 0xF0, 0x7E, 0x7F, 0x06}), V{}, "SysEx start");
      expect(ble(s, {0x80, 0x01, 0x02, 0x80, 0xF7, 0x80, 0x90, 60, 100}), V{"on c0 60 100"}, "SysEx across packets, then a note"); }
    { midiin::ParserState s;
      expect(ble(s, {0x80, 0x80, 0x90, 60, 100, 0x80, 0xF8, 61, 90}), V{"on c0 60 100", "on c0 61 90"},
             "real-time keeps running status"); }
    { midiin::ParserState s;
      expect(ble(s, {}), V{}, "empty");
      expect(ble(s, {0x80}), V{}, "header only");
      expect(ble(s, {0x12, 0x90, 60, 100}), V{}, "no header bit");
      expect(ble(s, {0x80, 0x80}), V{}, "truncated after timestamp");
      expect(ble(s, {0x80, 1, 2, 3}), V{}, "data with no status");
      Bytes noise(64);
      for (size_t i = 0; i < noise.size(); ++i) noise[i] = (uint8_t)((i * 73 + 11) & 0xFF);
      midiin::ParserState n;
      ble(n, noise); // must not crash
    }
    { midiin::ParserState s;
      expect(raw(s, {0x90, 60, 100, 60, 0}), V{"on c0 60 100", "off c0 60 0"}, "raw with running status");
      midiin::ParserState t;
      const std::string text = "hello device\r\n";
      expect(raw(t, Bytes(text.begin(), text.end())), V{}, "raw text yields nothing");
      midiin::ParserState u;
      expect(raw(u, {0xC0, 5, 0xF2, 1, 2, 0x80, 60, 64}), V{"off c0 60 64"}, "program change and system common skipped"); }
    { // Raw packets (the serial bridge rule): complete MIDI plays, anything else is rejected whole.
      auto pkt = [](midiin::ParserState& s, const Bytes& b, std::vector<std::string>& out) {
          return midiin::parseRawPacket(b.data(), b.size(), s, [&](const midiin::Message& m) { out.push_back(midiStr(m)); });
      };
      midiin::ParserState s;
      V out;
      const bool played = pkt(s, {0x90, 60, 100, 62, 90, 0xF8, 0x80, 60, 0}, out);
      check(played && out == V{"on c0 60 100", "on c0 62 90", "off c0 60 0"},
            "raw packet: complete MIDI with running status and real-time: " + joinStrs(out));
      const std::string t = "note 60\r\n";
      const std::vector<Bytes> bad = {Bytes(t.begin(), t.end()), {0x90, 60}, {0x90, 60, 100, 61}, {0xF0, 1, 0xF7},
                                      {0x90, 200, 100}, {}, {0xC3, 0xA9, 0x20}};
      int i = 0;
      for (const Bytes& b : bad) {
          V o;
          const bool played = pkt(s, b, o);
          check(!played && o.empty() && s.status == 0, "raw packet rejected #" + std::to_string(i));
          ++i;
      }
    }
    { // No allocation in the parsers.
        midiin::ParserState s;
        const uint8_t pkt[] = {0x80, 0x80, 0x90, 60, 100, 0x81, 62, 101, 64, 102, 0x80, 0xE0, 0, 0x40};
        int n = 0;
        gRenderAllocs.store(0); gRenderFrees.store(0);
        tCountAllocs = true;
        for (int k = 0; k < 1000; ++k)
            midiin::parseBleMidiPacket(pkt, sizeof(pkt), s, [&](const midiin::Message&) { ++n; });
        tCountAllocs = false;
        check(gRenderAllocs.load() + gRenderFrees.load() == 0 && n == 4000, "parsers allocate / count " + std::to_string(n));
    }
    const bool pass = failed == 0;
    std::printf("mip   MIDI input parsers: BLE-MIDI, raw stream, raw packets (JS parser vectors)  %-4s %d failed checks%s\n",
                pass ? "PASS" : "FAIL", failed, detail.c_str());
    std::fflush(stdout);
    return pass;
}

// Records what the input asks the engine for (thread-safe), and can refuse note-ons.
struct RecordingMidiSink : midiin::MidiInputSink {
    std::mutex m;
    std::vector<std::string> ops;
    std::atomic<bool> accept{true};
    int lastOp[128] = {}; // per synth note: 0 none, 1 on, 2 off (after the last allNotesOff)
    bool noteOn(int note, int velocity, double) override {
        if (!accept.load()) return false;
        std::lock_guard<std::mutex> g(m);
        ops.push_back("on " + std::to_string(note) + " " + std::to_string(velocity));
        lastOp[note & 127] = 1;
        return true;
    }
    bool drumHit(int voice, int velocity, double) override {
        if (!accept.load()) return false;
        std::lock_guard<std::mutex> g(m);
        ops.push_back("drum " + std::to_string(voice) + " " + std::to_string(velocity));
        return true;
    }
    void noteOff(int note) override {
        std::lock_guard<std::mutex> g(m);
        ops.push_back("off " + std::to_string(note));
        lastOp[note & 127] = 2;
    }
    void allNotesOff() override {
        std::lock_guard<std::mutex> g(m);
        ops.push_back("all");
        for (int& x : lastOp) x = 0;
    }
    std::vector<std::string> take() {
        std::lock_guard<std::mutex> g(m);
        std::vector<std::string> out;
        out.swap(ops);
        return out;
    }
};

static void identityMap(int* actions, int* velocity) {
    for (int i = 0; i < 128; ++i) { actions[i] = i; velocity[i] = i; }
}

// The input map: sanitising, and a writer swapping two maps while readers read (every read is
// wholly one map or the other), no allocation, the reader never blocked by a lock.
static bool runMidiMap() {
    int failed = 0;
    std::string detail;
    auto check = [&](bool ok, const std::string& what) {
        if (!ok) { ++failed; if (detail.size() < 600) detail += "; FAIL " + what; }
    };
    midiin::InputMap map;
    { // defaults: pass, identity velocity (0 -> 1)
        const auto e = map.read(60, 0), f = map.read(60, 100);
        check(e.action == midiin::InputMap::kPass && e.velocity == 1 && f.velocity == 100 && map.version() == 0, "defaults");
    }
    { // sanitising
        int a[128], v[128];
        identityMap(a, v);
        a[1] = 128; a[2] = -5; a[3] = 256 + 8; a[4] = 256 + 9; a[5] = 255; v[10] = 0; v[11] = 300;
        map.set(a, v);
        check(map.read(1, 1).action == -1 && map.read(2, 1).action == -1 && map.read(3, 1).action == 264
              && map.read(4, 1).action == -1 && map.read(5, 1).action == -1 && map.read(6, 1).action == 6,
              "actions sanitised");
        check(map.read(0, 10).velocity == 1 && map.read(0, 11).velocity == 127 && map.version() == 1, "velocity clamped");
        map.set(a, nullptr);
        check(map.read(0, 77).velocity == 77, "null velocity = identity");
    }
    { // swap under load
        int a1[128], v1[128], a2[128], v2[128];
        for (int i = 0; i < 128; ++i) {
            a1[i] = i; v1[i] = 1;                      // map 1: synth notes, velocity 1
            a2[i] = 256 + (i % 9); v2[i] = 127;        // map 2: drum voices, velocity 127
        }
        map.set(a1, v1);
        std::atomic<bool> stop{false};
        std::atomic<uint64_t> reads{0}, torn{0};
        std::vector<std::thread> readers;
        for (int r = 0; r < 3; ++r) {
            readers.emplace_back([&, r] {
                uint64_t n = 0, bad = 0;
                int note = r;
                while (!stop.load(std::memory_order_relaxed)) {
                    note = (note + 7) & 127;
                    const auto e = map.read(note, (note * 5) & 127);
                    const bool drum = e.action >= 256;
                    const bool ok = drum ? (e.velocity == 127 && e.action == 256 + note % 9)
                                         : (e.velocity == 1 && e.action == note);
                    if (!ok) ++bad;
                    ++n;
                }
                reads.fetch_add(n);
                torn.fetch_add(bad);
            });
        }
        const uint32_t before = map.version();
        for (int k = 0; k < 20000; ++k) map.set((k & 1) ? a1 : a2, (k & 1) ? v1 : v2);
        stop.store(true);
        for (auto& t : readers) t.join();
        check(torn.load() == 0, "torn reads " + std::to_string(torn.load()) + " of " + std::to_string(reads.load()));
        check(map.version() - before == 20000, "version counts swaps");
        gRenderAllocs.store(0); gRenderFrees.store(0);
        tCountAllocs = true;
        for (int k = 0; k < 100; ++k) { map.set(a1, v1); (void)map.read(k & 127, k & 127); }
        tCountAllocs = false;
        check(gRenderAllocs.load() + gRenderFrees.load() == 0, "set / read allocate");
        std::printf("mim   MIDI input map: sanitising, 20000 swaps vs 3 readers (%llu reads)  ",
                    (unsigned long long)reads.load());
    }
    const bool pass = failed == 0;
    std::printf("%-4s %d failed checks%s\n", pass ? "PASS" : "FAIL", failed, detail.c_str());
    std::fflush(stdout);
    return pass;
}

// Holds: a note-off releases what its note-on played whatever the map says by then; shared synth
// notes are counted; a note another source holds is left to it; retrigger, disabled, refused,
// drums, releaseAll and allNotesOff; events and their flags. Then the real queue + synth: a mapped
// note sounds, the latency leg is recorded, the release stops it, the render does not allocate.
static bool runMidiHolds() {
    int failed = 0;
    std::string detail;
    auto check = [&](bool ok, const std::string& what) {
        if (!ok) { ++failed; if (detail.size() < 1200) detail += "; FAIL " + what; }
    };
    using V = std::vector<std::string>;
    auto expectOps = [&](RecordingMidiSink& sink, const V& want, const std::string& what) {
        const V got = sink.take();
        check(got == want, what + ": got " + joinStrs(got) + " want " + joinStrs(want));
    };
    using Bytes = std::vector<uint8_t>;
    auto send = [](midiin::MidiInput& in, const Bytes& b, int format = midiin::kFormatBleMidi) {
        return in.input(b.data(), b.size(), format, 1.0);
    };
    auto on = [](int n, int v) { return Bytes{0x80, 0x80, 0x90, (uint8_t)n, (uint8_t)v}; };
    auto off = [](int n) { return Bytes{0x80, 0x80, 0x80, (uint8_t)n, 0}; };
    auto drain = [](midiin::MidiInput& in) {
        std::vector<midiin::Event> out;
        in.beginDrain();
        in.drainEvents([&](const midiin::Event& e) { out.push_back(e); });
        return out;
    };
    int a[128], v[128];

    RecordingMidiSink sink;
    midiin::MidiInput in(sink);
    identityMap(a, v);
    for (int i = 0; i < 128; ++i) v[i] = i < 1 ? 1 : (i + 1) / 2 + 1; // a soft curve, to see it applied
    a[36] = 60; a[38] = 60; a[40] = 256 + 2; a[41] = -1;
    in.map().set(a, v);

    // Disabled by default: everything passes.
    send(in, on(36, 100));
    expectOps(sink, {}, "disabled note-on passes");
    auto ev = drain(in);
    check(ev.size() == 1 && ev[0].message.type == midiin::kNoteOn && !(ev[0].flags & midiin::kEventPlayed) && ev[0].out == -1,
          "passed event");
    send(in, off(36));
    expectOps(sink, {}, "note-off of a passed note passes");
    drain(in);

    in.setEnabled(true);
    { // map change between on and off
        bool flush = false;
        in.input(on(36, 100).data(), 5, midiin::kFormatBleMidi, 2.5, &flush);
        check(flush, "first event asks for a drain");
        bool again = true;
        in.input(on(50, 90).data(), 5, midiin::kFormatBleMidi, 2.6, &again);
        check(!again, "second event before the drain does not");
        int b[128];
        for (int i = 0; i < 128; ++i) b[i] = 100;
        in.map().set(b, nullptr);
        send(in, off(36));
        send(in, off(50));
        expectOps(sink, {"on 60 51", "on 50 46", "off 60", "off 50"}, "off releases what on played after a map change");
        ev = drain(in);
        check(ev.size() == 4 && (ev[0].flags & midiin::kEventPlayed) && ev[0].out == 60 && ev[0].outVelocity == 51
              && ev[0].arrival == 2.5 && (ev[2].flags & midiin::kEventLast) && ev[2].out == 60, "events of the map change");
        in.map().set(a, v);
    }
    { // two input notes on one synth note
        send(in, on(36, 127)); send(in, on(38, 127));
        send(in, off(36));
        expectOps(sink, {"on 60 65", "on 60 65"}, "first off of a shared note: no release");
        check(in.holdCountForTests(60) == 1 && in.stats().held == 1, "one hold left");
        send(in, off(38));
        expectOps(sink, {"off 60"}, "last off releases");
        ev = drain(in);
        check(ev.size() == 4 && !(ev[2].flags & midiin::kEventLast) && (ev[3].flags & midiin::kEventLast), "last flag on the last off only");
    }
    { // another source holds the note
        in.externalNoteOn(60);
        send(in, on(36, 100)); send(in, off(36));
        expectOps(sink, {"on 60 51"}, "a note held elsewhere is not released");
        ev = drain(in);
        check(ev.size() == 2 && (ev[1].flags & midiin::kEventPlayed) && (ev[1].flags & midiin::kEventLast), "still a played, last off");
        in.externalNoteOff(60);
        send(in, on(36, 100)); send(in, off(36));
        expectOps(sink, {"on 60 51", "off 60"}, "released again once the other source let go");
        drain(in);
    }
    { // retrigger without a note-off
        send(in, on(36, 100)); send(in, on(36, 80));
        expectOps(sink, {"on 60 51", "off 60", "on 60 41"}, "second note-on releases the first");
        ev = drain(in);
        check(ev.size() == 3 && ev[1].message.type == midiin::kNoteOff && (ev[1].flags & midiin::kEventSynthetic)
              && ev[1].message.data1 == 36 && ev[1].out == 60, "synthetic off event");
        send(in, off(36));
        expectOps(sink, {"off 60"}, "then the off");
        drain(in);
    }
    { // disabled while held: the off still releases
        send(in, on(36, 100));
        in.setEnabled(false);
        send(in, off(36));
        expectOps(sink, {"on 60 51", "off 60"}, "off while disabled releases the hold");
        ev = drain(in);
        check(ev.size() == 2 && (ev[1].flags & midiin::kEventPlayed), "played off while disabled");
        in.setEnabled(true);
    }
    { // drums, unmapped, velocity 0
        send(in, on(40, 127)); send(in, off(40));
        send(in, on(41, 127)); send(in, off(41));
        send(in, {0x80, 0x80, 0x90, 36, 100, 0x80, 36, 0});
        expectOps(sink, {"drum 2 65", "on 60 51", "off 60"}, "drum hit; unmapped passes; velocity 0 is the off");
        ev = drain(in);
        check(ev.size() == 6 && (ev[0].flags & midiin::kEventDrum) && ev[0].out == 2 && !(ev[1].flags & midiin::kEventPlayed)
              && !(ev[2].flags & midiin::kEventPlayed) && (ev[5].flags & midiin::kEventPlayed), "drum / pass events");
    }
    { // the engine refuses: passes to the app, counted
        const uint64_t dropped = in.stats().dropped;
        sink.accept.store(false);
        send(in, on(36, 100)); send(in, off(36));
        sink.accept.store(true);
        expectOps(sink, {}, "refused note-on: nothing held");
        ev = drain(in);
        check(ev.size() == 2 && !(ev[0].flags & midiin::kEventPlayed) && !(ev[1].flags & midiin::kEventPlayed)
              && in.stats().dropped == dropped + 1, "refused events pass");
    }
    { // releaseAll (disconnect)
        const uint32_t epoch = in.epoch();
        in.externalNoteOn(62);
        a[37] = 62; in.map().set(a, v);
        send(in, on(36, 100)); send(in, on(37, 100)); send(in, {0x80, 0x80, 0x92, 67}); // half a message
        sink.take();
        const uint32_t next = in.releaseAll();
        expectOps(sink, {"off 60"}, "releaseAll: releases its holds, not one held elsewhere");
        check(next == epoch + 1 && in.epoch() == next && in.stats().held == 0, "epoch bumped");
        send(in, {0x80, 80}); // the half message's second half: the parser was reset
        send(in, off(36));
        expectOps(sink, {}, "after releaseAll: offs pass");
        ev = drain(in);
        check(ev.size() == 3 && ev[2].epoch == next && !(ev[2].flags & midiin::kEventPlayed), "later events carry the new epoch");
        in.externalNoteOff(62);
    }
    { // allNotesOff
        send(in, on(36, 100));
        in.allNotesOff();
        expectOps(sink, {"on 60 51", "all"}, "allNotesOff goes to the sink");
        send(in, off(36));
        expectOps(sink, {}, "and forgets the hold");
        drain(in);
    }
    { // raw packets: text rejected, counted; no allocation per call
        const uint64_t rejected = in.stats().rejectedPackets;
        const std::string text = "pad 1 down\r\n";
        in.input((const uint8_t*)text.data(), text.size(), midiin::kFormatRawPackets, 1.0);
        Bytes nus = {0x90, 36, 100};
        // The recording sink allocates its strings: refuse, so only the input's own work is counted.
        sink.accept.store(false);
        gRenderAllocs.store(0); gRenderFrees.store(0);
        tCountAllocs = true;
        for (int k = 0; k < 100; ++k) {
            in.input(nus.data(), nus.size(), midiin::kFormatRawPackets, 1.0);
            in.input((const uint8_t*)text.data(), text.size(), midiin::kFormatRawPackets, 1.0);
        }
        tCountAllocs = false;
        sink.accept.store(true);
        check(gRenderAllocs.load() + gRenderFrees.load() == 0, "input() allocates");
        drain(in);
        const uint64_t rejectedNow = in.stats().rejectedPackets;
        in.input(nus.data(), nus.size(), midiin::kFormatRawPackets, 1.0);
        expectOps(sink, {"on 60 51"}, "raw packet note-on plays");
        check(rejectedNow == rejected + 101, "text rejected " + std::to_string(rejectedNow - rejected));
        check(in.input(nullptr, 3, 0, 1.0) == -1 && in.input(nus.data(), 3, 3, 1.0) == -1, "bad args");
        in.releaseAll();
        sink.take();
        drain(in);
    }
    { // a rejected raw packet's note-offs still end what they held; its note-ons never play
        const Bytes nusOn = {0x90, 36, 100};
        in.input(nusOn.data(), nusOn.size(), midiin::kFormatRawPackets, 1.0);
        expectOps(sink, {"on 60 51"}, "raw note-on held");
        drain(in);
        // A bridge log line around a note-off (8n): not complete MIDI, so rejected whole; before
        // the salvage the note-off was lost with it and synth note 60 stayed on.
        const Bytes mixed = {'o', 'k', ' ', 0x80, 36, 0x40, '\r', '\n'};
        const uint64_t rejected = in.stats().rejectedPackets;
        const int parsed = in.input(mixed.data(), mixed.size(), midiin::kFormatRawPackets, 1.0);
        expectOps(sink, {"off 60"}, "note-off salvaged from a rejected packet");
        check(parsed == 1 && in.heldNoteForTests(36) == -1 && in.holdCountForTests(60) == 0
              && in.stats().rejectedPackets == rejected + 1, "salvaged note-off releases the hold");
        auto evs = drain(in);
        check(evs.size() == 1 && evs[0].message.type == midiin::kNoteOff && evs[0].message.data1 == 36
              && (evs[0].flags & midiin::kEventPlayed) && (evs[0].flags & midiin::kEventLast) && evs[0].out == 60,
              "salvaged note-off event");
        // 9n kk 00 is a note-off too; a note-on in a rejected packet never plays.
        in.input(nusOn.data(), nusOn.size(), midiin::kFormatRawPackets, 1.0);
        sink.take();
        const Bytes mixed2 = {'x', 0x90, 38, 100, 0x91, 36, 0x00, 'y'};
        in.input(mixed2.data(), mixed2.size(), midiin::kFormatRawPackets, 1.0);
        expectOps(sink, {"off 60"}, "9n velocity 0 salvaged, the note-on beside it ignored");
        check(in.heldNoteForTests(38) == -1, "no hold from a rejected note-on");
        drain(in);
        // A salvaged note-off of a note the input does not hold is reported, not played.
        const Bytes mixed3 = {'z', 0x80, 50, 0};
        in.input(mixed3.data(), mixed3.size(), midiin::kFormatRawPackets, 1.0);
        expectOps(sink, {}, "unheld salvaged note-off sends nothing");
        evs = drain(in);
        check(evs.size() == 1 && evs[0].message.data1 == 50 && !(evs[0].flags & midiin::kEventPlayed), "unheld note-off passed");
    }
    { // the engine's own noteOn marks before its push and restores the mark when the push failed
        send(in, on(36, 100));
        sink.take();
        const bool previous = in.externalNoteOn(60);
        in.restoreExternal(60, previous); // the queue refused the engine's note-on
        send(in, off(36));
        expectOps(sink, {"off 60"}, "a restored mark does not hold the input's release back");
        send(in, on(36, 100));
        sink.take();
        check(!in.externalNoteOn(60), "mark was clear");      // the engine's note-on, queued
        send(in, off(36));
        expectOps(sink, {}, "a kept mark leaves the release to the other source");
        in.externalNoteOff(60);
        drain(in);
    }
    { // the event ring: full -> counted, nothing else breaks
        const uint64_t before = in.stats().eventsDropped;
        for (int k = 0; k < midiin::MidiInput::kEventCapacity + 10; ++k) send(in, {0x80, 0x80, 0xB0, 1, (uint8_t)(k & 127)});
        check(in.stats().eventsDropped == before + 11, "full ring drops " + std::to_string(in.stats().eventsDropped - before));
        check(drain(in).size() == (size_t)midiin::MidiInput::kEventCapacity - 1, "ring holds capacity - 1");
    }

    // Through the real queue and synth.
    double legMs = -1.0;
    {
        NoteEventQueue queue;
        std::atomic<bool> accepting{false};
        static std::atomic<int> refusedCalls{0};
        refusedCalls.store(0);
        midiin::NoteQueueSink qsink(queue, &accepting, [] { refusedCalls.fetch_add(1); });
        midiin::MidiInput input(qsink);
        identityMap(a, v);
        a[36] = 60;
        input.map().set(a, v);
        input.setEnabled(true);
        LatencyWindow leg;
        TransportEngine transport;
        PolySynthProcessor synth;
        synth.setNoteQueue(&queue);
        synth.setTransport(&transport);
        synth.setInputLatency(&leg);
        synth.setRelease(1.0);
        synth.setRateAndBufferSizeDetails(48000, 128);
        synth.prepareToPlay(48000, 128);
        juce::AudioBuffer<float> buf(2, 128);
        juce::MidiBuffer midi;
        auto render = [&](int blocks) {
            float peak = 0.0f;
            for (int b = 0; b < blocks; ++b) {
                tCountAllocs = true;
                synth.processBlock(buf, midi);
                tCountAllocs = false;
                peak = std::max(peak, buf.getMagnitude(0, 0, 128));
            }
            return peak;
        };
        gRenderAllocs.store(0); gRenderFrees.store(0);
        const Bytes n1 = on(36, 100);
        const int parsedNotReady = input.input(n1.data(), n1.size(), midiin::kFormatBleMidi, 0.0);
        check(parsedNotReady == 1 && input.stats().dropped == 1 && queue.notesQueued.load() == 0, "not accepting: dropped, not queued");
        check(refusedCalls.load() == 1, "not accepting: the refused hook ran (the engine wakes from its idle stop)");
        accepting.store(true);
        const double arrival = midiin::NoteQueueSink::nowSeconds() - 0.005; // arrived 5 ms ago
        input.input(n1.data(), n1.size(), midiin::kFormatBleMidi, arrival);
        const float held = render(4);
        const LatencySummary s = leg.summary();
        legMs = s.last;
        check(held > 0.01f && synth.diag.activeVoices.load() == 1, "mapped note sounds: peak " + std::to_string(held));
        check(s.count == 1 && s.last >= 5.0 && s.last < 200.0, "arrival -> render leg " + std::to_string(s.last));
        const Bytes f1 = off(36);
        input.input(f1.data(), f1.size(), midiin::kFormatBleMidi, 0.0);
        render(20);
        check(synth.diag.activeVoices.load() == 0, "released by the input's note-off");
        // A drum hit through the queue records the leg too.
        a[38] = 256 + 0; input.map().set(a, v);
        const Bytes d1 = on(38, 120);
        input.input(d1.data(), d1.size(), midiin::kFormatBleMidi, midiin::NoteQueueSink::nowSeconds());
        render(1);
        check(leg.summary().count == 2, "drum hit leg");
        check(gRenderAllocs.load() + gRenderFrees.load() == 0, "render allocations");
    }

    const bool pass = failed == 0;
    std::printf("mih   MIDI input holds: map change, shared notes, other sources, retrigger, releaseAll, real synth  %-4s"
                " %d failed checks; arrival->render %.2f ms%s\n", pass ? "PASS" : "FAIL", failed, legMs, detail.c_str());
    std::fflush(stdout);
    return pass;
}

// OwnerToken (ios/OwnerToken.h): a reload can create the new module before the old one is
// destroyed. The old one's teardown (clear the MIDI input handler, shut the engine down) must
// then do nothing; the plain "clear / shut down" it replaced removed the new module's handler.
static bool runOwnerToken() {
    int failed = 0;
    std::string detail;
    auto check = [&](bool ok, const std::string& what) {
        if (!ok) { ++failed; detail += "; FAIL " + what; }
    };
    OwnerToken owner;
    check(owner.current() == 0 && !owner.isCurrent(0), "none at start");
    const uint64_t oldModule = owner.claim();
    check(owner.isCurrent(oldModule), "first claim owns");
    // Reload: the new module is created, then the old one is destroyed.
    const uint64_t newModule = owner.claim();
    check(newModule != oldModule && !owner.isCurrent(oldModule) && owner.isCurrent(newModule), "new claim takes over");
    check(!owner.release(oldModule) && owner.isCurrent(newModule), "late release of the old token does nothing");
    // The usual order: destroyed, then the next one created.
    check(owner.release(newModule) && owner.current() == 0, "release by the owner");
    check(!owner.release(newModule), "second release does nothing");
    const uint64_t third = owner.claim();
    check(owner.isCurrent(third) && !owner.release(0), "claim after release; token 0 never owns");
    // Racing claims and stale releases from 4 threads: exactly one current owner at the end.
    std::vector<std::thread> threads;
    std::atomic<uint64_t> lastClaim{0};
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&] {
            for (int k = 0; k < 5000; ++k) {
                const uint64_t mine = owner.claim();
                lastClaim.store(mine);
                owner.release(mine - 1); // a stale token (often another thread's)
            }
        });
    }
    for (auto& th : threads) th.join();
    const uint64_t cur = owner.current();
    check(cur == 0 || owner.isCurrent(cur), "consistent after races");
    const bool pass = failed == 0;
    std::printf("own   owner tokens: a destroyed module's late teardown leaves its replacement alone  %-4s %d failed checks%s\n",
                pass ? "PASS" : "FAIL", failed, detail.c_str());
    std::fflush(stdout);
    return pass;
}

static std::atomic<int> gMidiDrainRequests{0};
static void countMidiDrainRequest() { gMidiDrainRequests.fetch_add(1); }

// The exported C entry point: before install, then 4 threads sending BLE-MIDI note pairs while a
// fifth swaps the map, a sixth drains events and a seventh flips `enabled`. Afterwards every synth
// note the input started was released last, nothing is held, and events add up.
static bool runMidiEntry() {
    int failed = 0;
    std::string detail;
    auto check = [&](bool ok, const std::string& what) {
        if (!ok) { ++failed; if (detail.size() < 800) detail += "; FAIL " + what; }
    };
    const uint8_t one[] = {0x80, 0x80, 0x90, 60, 100};
    check(expojuce_midi_input_bytes(one, sizeof(one), EXPOJUCE_MIDI_FORMAT_BLE_MIDI, 0.0) == -1, "before install: -1");

    RecordingMidiSink sink;
    midiin::MidiInput input(sink);
    midiin::installEntryPoint(&input, countMidiDrainRequest);
    int a1[128], a2[128], v[128];
    for (int i = 0; i < 128; ++i) { a1[i] = i; a2[i] = (i + 1) & 127; v[i] = i; }
    input.map().set(a1, v);
    input.setEnabled(true);

    check(expojuce_midi_input_bytes(one, sizeof(one), 7, 0.0) == -1, "unknown format: -1");
    check(expojuce_midi_input_bytes(nullptr, 0, EXPOJUCE_MIDI_FORMAT_RAW_STREAM, 0.0) == 0, "empty: 0");
    const uint8_t off60[] = {0x80, 0x80, 0x80, 60, 0};
    expojuce_midi_input_bytes(off60, sizeof(off60), EXPOJUCE_MIDI_FORMAT_BLE_MIDI, 0.0);

    constexpr int kThreads = 4, kPairs = 20000;
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> drained{0}, synthetic{0};
    std::thread swapper([&] {
        for (int k = 0; !stop.load(); ++k) input.map().set((k & 1) ? a1 : a2, v);
    });
    std::thread toggler([&] {
        for (int k = 0; !stop.load(); ++k) { input.setEnabled((k % 3) != 0); std::this_thread::yield(); }
    });
    std::thread consumer([&] {
        for (;;) {
            const bool last = stop.load();
            input.beginDrain();
            input.drainEvents([&](const midiin::Event& e) {
                drained.fetch_add(1);
                if (e.flags & midiin::kEventSynthetic) synthetic.fetch_add(1);
            });
            if (last) break;
            std::this_thread::yield();
        }
    });
    std::vector<std::thread> senders;
    std::atomic<int> parsedTotal{0};
    for (int t = 0; t < kThreads; ++t) {
        senders.emplace_back([&, t] {
            int parsed = 0;
            for (int k = 0; k < kPairs; ++k) {
                const uint8_t n = (uint8_t)(t * 20 + (k % 20)); // 20 notes per thread, neighbours share synth notes under map 2
                const uint8_t pkt[] = {0x80, 0x80, 0x90, n, 100, 0x80, 0x80, n, 0};
                if (k % 7 == 3) { // note-on now, note-off in its own packet
                    parsed += expojuce_midi_input_bytes(pkt, 5, EXPOJUCE_MIDI_FORMAT_BLE_MIDI, 0.0);
                    const uint8_t offPkt[] = {0x80, 0x80, 0x80, n, 0};
                    parsed += expojuce_midi_input_bytes(offPkt, sizeof(offPkt), EXPOJUCE_MIDI_FORMAT_BLE_MIDI, 0.0);
                } else {
                    parsed += expojuce_midi_input_bytes(pkt, sizeof(pkt), EXPOJUCE_MIDI_FORMAT_BLE_MIDI, 0.0);
                }
            }
            parsedTotal.fetch_add(parsed);
        });
    }
    for (auto& s : senders) s.join();
    stop.store(true);
    swapper.join();
    toggler.join();
    consumer.join();
    input.beginDrain();
    input.drainEvents([&](const midiin::Event&) { drained.fetch_add(1); });

    const midiin::Stats st = input.stats();
    check(parsedTotal.load() == kThreads * kPairs * 2, "parsed " + std::to_string(parsedTotal.load()));
    check(st.held == 0, "held after every pair: " + std::to_string(st.held));
    int hanging = 0;
    {
        std::lock_guard<std::mutex> g(sink.m);
        for (int s = 0; s < 128; ++s) if (sink.lastOp[s] == 1) ++hanging;
    }
    check(hanging == 0, std::to_string(hanging) + " synth notes left on");
    for (int s = 0; s < 128; ++s)
        if (input.holdCountForTests(s) != 0) { check(false, "hold count of " + std::to_string(s)); break; }
    check(drained.load() + st.eventsDropped == st.messages + synthetic.load() && synthetic.load() == 0,
          "events: drained " + std::to_string(drained.load()) + " + dropped " + std::to_string(st.eventsDropped)
          + " vs messages " + std::to_string(st.messages));
    check(st.played + st.passed + st.dropped == (uint64_t)(kThreads * kPairs), "every note-on counted once");
    check(gMidiDrainRequests.load() > 0, "drains requested");
    midiin::installEntryPoint(nullptr, nullptr);

    const bool pass = failed == 0;
    std::printf("mic   MIDI input C entry: 4 senders x %d pairs, map swaps, enable flips, a drainer  %-4s %d failed checks;"
                " played %llu passed %llu, drain requests %d%s\n", kPairs, pass ? "PASS" : "FAIL", failed,
                (unsigned long long)st.played, (unsigned long long)st.passed, gMidiDrainRequests.load(), detail.c_str());
    std::fflush(stdout);
    return pass;
}

#include "sampler_cases.inc"

//==============================================================================
// Loop sequence player (ios/SequencePlayer.h, 0.9.0-rc.5), driven like PolySynthProcessor drives
// it: the transport advances a block, then the player runs with the block's start beat / bpm /
// play state / seek flag, and its outputs are recorded at absolute sample positions.
struct SeqRec { int64_t at; int note; bool on; bool drum; double beat; };

struct SeqRig {
    double sr = 48000.0;
    TransportEngine transport;
    std::unique_ptr<seqplay::SequenceStore> store = std::make_unique<seqplay::SequenceStore>();
    seqplay::SequencePlayer player;
    std::vector<SeqRec> out;
    int64_t clock = 0;
    uint64_t allocs = 0;
    SeqRig() { player.setStore(store.get()); out.reserve(200000); }

    // Renders `total` samples in blocks cycling through `blocks`; before(clock) runs before each.
    void run(int64_t total, const std::vector<int>& blocks, const std::function<void(int64_t)>& before = {}) {
        size_t bi = 0;
        for (int64_t done = 0; done < total;) {
            const int n = (int)std::min<int64_t>(blocks[bi++ % blocks.size()], total - done);
            if (before) before(clock);
            gRenderAllocs.store(0); gRenderFrees.store(0);
            tCountAllocs = true;
            transport.advanceBlock(n, sr);
            player.process(transport.getBlockStartBeat(), transport.getBlockBpm(), transport.isBlockPlaying(),
                           transport.wasBlockSeeked(), sr, n, [&](const seqplay::Output& o) {
                               out.push_back({clock + o.offset, o.note, o.on, o.kind == seqplay::EventKind::Drum, o.atBeat});
                           });
            tCountAllocs = false;
            allocs += gRenderAllocs.load() + gRenderFrees.load();
            clock += n;
            done += n;
        }
    }
};

static seqplay::Event seqNote(double start, double length, int note, int velocity = 100) {
    seqplay::Event e;
    e.start = start; e.length = length; e.note = (uint8_t)note; e.velocity = (uint8_t)velocity;
    e.kind = seqplay::EventKind::Note;
    return e;
}
static seqplay::Event seqDrum(double start, int voice, int velocity = 100) {
    seqplay::Event e;
    e.start = start; e.length = 0.0; e.note = (uint8_t)voice; e.velocity = (uint8_t)velocity;
    e.kind = seqplay::EventKind::Drum;
    return e;
}
// First sample at or after beat t at a constant tempo from beat 0 at sample 0.
static int64_t seqSampleOf(double t, double samplesPerBeat) {
    return (int64_t)std::ceil(t * samplesPerBeat - 1.0e-6);
}

static bool runSequence() {
    int failed = 0;
    std::string detail;
    auto check = [&](bool ok, const std::string& what) { if (!ok) { ++failed; detail += " [" + what + "]"; } };
    uint64_t allocs = 0;
    const std::vector<int> odd = {471, 128, 1024, 333, 64, 97};

    // 1. Exact sample offsets across odd block sizes and the loop wrap: 120 bpm, 48 kHz (24000
    //    samples a beat), a 4-beat loop, three passes. Events on integer samples, between samples,
    //    on the loop's first and last sample, a note across the loop end and a drum hit.
    {
        SeqRig rig;
        const double spb = 24000.0, L = 4.0;
        std::vector<seqplay::Event> ev = {
            seqNote(0.0, 0.5, 60), seqNote(0.25, 0.25, 62), seqNote(1.0 / 3.0, 0.1, 64), seqNote(1.0, 1.0, 65),
            seqNote(3.99, 0.5, 67), seqNote(4.0 - 1.0 / 24000.0, 0.0001, 69), seqDrum(2.0, 0), seqDrum(2.5 + 1e-5, 3),
        };
        check(rig.store->set(ev.data(), (int)ev.size(), L).ok, "set");
        rig.transport.start();
        rig.run((int64_t)(3 * L * spb), odd);
        allocs += rig.allocs;
        // Expected: every event once per pass, on at its sample, off at start + length.
        std::vector<SeqRec> want;
        for (int pass = 0; pass < 3; ++pass) {
            for (const auto& e : ev) {
                const double t = pass * L + e.start;
                want.push_back({seqSampleOf(t, spb), e.note, true, e.kind == seqplay::EventKind::Drum, t});
                if (e.kind == seqplay::EventKind::Note && t + e.length < 3 * L)
                    want.push_back({seqSampleOf(t + e.length, spb), e.note, false, false, t + e.length});
            }
        }
        auto key = [](const SeqRec& r) { return std::make_tuple(r.at, r.on ? 1 : 0, r.note, r.drum ? 1 : 0); };
        std::sort(want.begin(), want.end(), [&](const SeqRec& a, const SeqRec& b) { return key(a) < key(b); });
        std::vector<SeqRec> got = rig.out;
        std::sort(got.begin(), got.end(), [&](const SeqRec& a, const SeqRec& b) { return key(a) < key(b); });
        // The 0.0001-beat note (2.4 samples) at the loop's last sample: its release comes in the
        // block after its start when the start is a block's last sample, so compare its off loosely.
        int mismatches = 0;
        std::string first;
        const size_t n = std::min(want.size(), got.size());
        for (size_t i = 0; i < n; ++i) {
            const bool loose = !want[i].on && want[i].note == 69;
            const bool same = got[i].on == want[i].on && got[i].note == want[i].note && got[i].drum == want[i].drum
                && (loose ? std::llabs(got[i].at - want[i].at) <= 1 : got[i].at == want[i].at);
            if (!same && mismatches++ == 0)
                first = "want " + std::to_string(want[i].at) + (want[i].on ? " on " : " off ") + std::to_string(want[i].note)
                    + " got " + std::to_string(got[i].at) + (got[i].on ? " on " : " off ") + std::to_string(got[i].note);
        }
        check(want.size() == got.size(), "count want " + std::to_string(want.size()) + " got " + std::to_string(got.size()));
        check(mismatches == 0, "exact offsets: " + std::to_string(mismatches) + " differ, first " + first);
        check(rig.store->notesFired.load() == 3 * ev.size() && rig.store->lateFires.load() == 0, "fired / late");
        detail += " exact " + std::to_string(got.size()) + " events";
    }

    // 2. Tempo change mid-loop (120 -> 90 -> 173 bpm at block starts): every event still fires once
    //    per pass, in order, on the first sample at or after its beat by the transport's own beats.
    {
        SeqRig rig;
        std::vector<seqplay::Event> ev;
        for (int k = 0; k < 16; ++k) ev.push_back(seqNote(k * 0.25, 0.125, 48 + k));
        rig.store->set(ev.data(), (int)ev.size(), 4.0);
        rig.transport.start();
        std::vector<std::pair<int64_t, double>> blockStarts; // (sample, beat) at each block start
        std::vector<double> blockBps;
        rig.run(48000 * 9, odd, [&](int64_t at) {
            if (at >= 48000 * 3 && rig.transport.getTempo() == 120.0) rig.transport.setTempo(90.0);
            if (at >= 48000 * 6 && rig.transport.getTempo() == 90.0) rig.transport.setTempo(173.0);
            blockStarts.push_back({at, rig.transport.getPosition()});
            blockBps.push_back(rig.transport.getTempo() / (60.0 * 48000.0));
        });
        allocs += rig.allocs;
        int wrong = 0, ons = 0;
        double lastBeat = -1.0;
        for (const SeqRec& r : rig.out) {
            if (!r.on) continue;
            ++ons;
            if (r.beat <= lastBeat) ++wrong;
            lastBeat = r.beat;
            // The block holding r.at, and the first sample at or after r.beat in it.
            size_t b = 0;
            while (b + 1 < blockStarts.size() && blockStarts[b + 1].first <= r.at) ++b;
            const int64_t expect = blockStarts[b].first
                + (int64_t)std::max(0.0, std::ceil((r.beat - blockStarts[b].second) / blockBps[b] - 1e-6));
            if (expect != r.at) ++wrong;
        }
        const double endBeat = rig.transport.getPosition();
        const int expectOns = (int)std::floor(endBeat / 0.25 - 1e-9) + 1;
        check(wrong == 0 && ons == expectOns, "tempo change: " + std::to_string(wrong) + " wrong, ons " + std::to_string(ons)
              + " want " + std::to_string(expectOns));
        check(rig.player.activeNoteCount() <= 1, "tempo: notes left");
    }

    // 3. A jump releases everything at once and the loop goes on from the new position; a stop
    //    releases everything; a start plays an event on the start beat at once.
    {
        SeqRig rig;
        std::vector<seqplay::Event> ev = { seqNote(0.0, 3.0, 60), seqNote(1.0, 2.5, 64), seqNote(2.0, 0.5, 67) };
        rig.store->set(ev.data(), (int)ev.size(), 4.0);
        rig.transport.start();
        rig.run(24000 * 1.5, {256}); // 60 and 64 sounding
        check(rig.player.activeNoteCount() == 2, "two sounding before the jump");
        const int64_t jumpAt = rig.clock;
        rig.transport.setPosition(2.0); // the next block
        rig.run(256, {256});
        int offsAtJump = 0, onsAtJump = 0;
        for (const SeqRec& r : rig.out) {
            if (r.at == jumpAt && !r.on) ++offsAtJump;
            if (r.at == jumpAt && r.on && r.note == 67) ++onsAtJump;
        }
        check(offsAtJump == 2 && onsAtJump == 1, "jump: offs " + std::to_string(offsAtJump) + " on " + std::to_string(onsAtJump));
        rig.run(24000, {256});
        rig.transport.stop();
        rig.run(512, {256});
        check(rig.player.activeNoteCount() == 0, "stop releases");
        int ons = 0, offs = 0;
        for (const SeqRec& r : rig.out) (r.on ? ons : offs)++;
        check(ons == offs, "every on has its off (" + std::to_string(ons) + "/" + std::to_string(offs) + ")");
        // Start from beat 1.0 exactly: 64 plays on the first sample.
        rig.out.clear();
        rig.transport.setPosition(1.0);
        rig.transport.start();
        const int64_t startAt = rig.clock;
        rig.run(256, {256});
        check(!rig.out.empty() && rig.out[0].on && rig.out[0].note == 64 && rig.out[0].at == startAt, "start on the beat");
        allocs += rig.allocs;
    }

    // 4. A new sequence while notes sound: a note whose event is gone is released at once; one whose
    //    event stays keeps sounding with the new length; a loop-length change releases everything.
    {
        SeqRig rig;
        std::vector<seqplay::Event> ev = { seqNote(0.0, 3.0, 60), seqNote(0.0, 3.0, 64) };
        rig.store->set(ev.data(), (int)ev.size(), 4.0);
        rig.transport.start();
        rig.run(24000, {128}); // beat 1: both sounding
        std::vector<seqplay::Event> next = { seqNote(0.0, 1.5, 60), seqNote(2.0, 1.0, 72) };
        rig.store->set(next.data(), (int)next.size(), 4.0);
        const int64_t swapAt = rig.clock;
        rig.run(24000, {128});
        bool off64 = false, early60 = false, off60 = false;
        for (const SeqRec& r : rig.out) {
            if (!r.on && r.note == 64 && r.at == swapAt) off64 = true;
            if (!r.on && r.note == 60 && r.at < 36000) early60 = true;
            if (!r.on && r.note == 60 && r.at == 36000) off60 = true;
        }
        check(off64 && !early60 && off60, "swap: removed note released, kept note ends at its new length");
        rig.run(12000, {128}); // beat 2.5: 72 sounding (60 ended at 1.5)
        check(rig.player.activeNoteCount() == 1, "72 sounding");
        std::vector<seqplay::Event> longer = { seqNote(2.0, 1.0, 72) };
        rig.store->set(longer.data(), 1, 8.0);
        const int64_t loopAt = rig.clock;
        rig.run(128, {128});
        bool off72 = false;
        for (const SeqRec& r : rig.out) if (!r.on && r.note == 72 && r.at == loopAt) off72 = true;
        check(off72 && rig.player.activeNoteCount() == 0, "loop length change releases");
        allocs += rig.allocs;
    }

    // 5. Mute releases and plays nothing; unmute plays the next event. Clearing releases.
    {
        SeqRig rig;
        std::vector<seqplay::Event> ev = { seqNote(0.0, 0.75, 60), seqNote(1.0, 0.75, 62) };
        rig.store->set(ev.data(), 2, 2.0);
        rig.transport.start();
        rig.run(6000, {128});
        rig.store->setMuted(true);
        const int64_t muteAt = rig.clock;
        rig.run(48000, {128});
        bool offAtMute = false;
        int onsMuted = 0;
        for (const SeqRec& r : rig.out) {
            if (!r.on && r.note == 60 && r.at == muteAt) offAtMute = true;
            if (r.on && r.at > muteAt) ++onsMuted;
        }
        check(offAtMute && onsMuted == 0, "mute");
        rig.store->setMuted(false);
        rig.run(48000, {128});
        int onsAfter = 0;
        for (const SeqRec& r : rig.out) if (r.on && r.at > muteAt) ++onsAfter;
        check(onsAfter == 2, "unmute plays the next events (" + std::to_string(onsAfter) + ")");
        rig.run(6000, {128}); // 60 sounding again
        rig.store->clear();
        rig.run(128, {128});
        check(rig.player.activeNoteCount() == 0, "clear releases");
        allocs += rig.allocs;
    }

    // 6. Count-in and a single pass: nothing before fromBeat (the loop's tail must not play under
    //    the count), nothing at or after endBeat; skipBefore skips one pass; the engine's
    //    allNotesOff releases.
    {
        SeqRig rig;
        std::vector<seqplay::Event> ev = { seqNote(0.0, 0.5, 60), seqNote(3.0, 0.5, 63) };
        ev[0].skipBefore = 4.0; // already sounded in pass 0
        rig.store->set(ev.data(), 2, 4.0, 0.0, 8.0);
        rig.transport.setPosition(-4.0);
        rig.transport.start();
        rig.run(24000 * 14, {333});
        std::vector<double> beats;
        for (const SeqRec& r : rig.out) if (r.on) beats.push_back(r.beat);
        check(beats == std::vector<double>({3.0, 4.0, 7.0}), "fromBeat / endBeat / skipBefore: got " + std::to_string(beats.size()));
        SeqRig held;
        std::vector<seqplay::Event> one = { seqNote(0.0, 2.0, 60) };
        held.store->set(one.data(), 1, 4.0);
        held.transport.start();
        held.run(1000, {128});
        held.player.requestReleaseAll();
        held.run(128, {128});
        check(held.player.activeNoteCount() == 0 && !held.out.back().on, "allNotesOff releases");
        allocs += rig.allocs + held.allocs;
    }

    // 7. Overlapping instances of one pitch: the release goes out once, when the last one ends.
    {
        SeqRig rig;
        std::vector<seqplay::Event> ev = { seqNote(0.0, 2.0, 60), seqNote(1.0, 2.0, 60) };
        rig.store->set(ev.data(), 2, 4.0);
        rig.transport.start();
        rig.run(24000 * 4 - 1000, {256});
        std::vector<std::pair<int64_t, bool>> seen;
        for (const SeqRec& r : rig.out) seen.push_back({r.at, r.on});
        const std::vector<std::pair<int64_t, bool>> want = {{0, true}, {24000, true}, {72000, false}};
        check(seen == want, "overlap: " + std::to_string(seen.size()) + " outputs");
        allocs += rig.allocs;
    }

    // 8. The store: limits, sanitising, sorting.
    {
        auto store = std::make_unique<seqplay::SequenceStore>();
        std::vector<seqplay::Event> big((size_t)seqplay::kMaxEvents + 1, seqNote(0.0, 1.0, 60));
        check(!store->set(big.data(), (int)big.size(), 4.0).ok, "over the cap refused");
        big.pop_back();
        check(store->set(big.data(), (int)big.size(), 4.0).accepted == seqplay::kMaxEvents, "the cap itself");
        std::vector<seqplay::Event> mixed = { seqNote(3.0, 1.0, 60), seqNote(-0.1, 1.0, 60), seqNote(4.0, 1.0, 60),
                                              seqNote(1.0, 0.0, 60), seqNote(1.0, NAN, 60), seqDrum(1.0, 9),
                                              seqNote(0.5, 1.0, 200), seqDrum(2.0, 8), seqNote(1.0, 1.0, 61, 0) };
        const seqplay::SetResult r = store->set(mixed.data(), (int)mixed.size(), 4.0);
        check(r.ok && r.accepted == 3 && r.dropped == 6, "sanitise: accepted " + std::to_string(r.accepted));
        const seqplay::Sequence* s = store->acquire();
        check(s && s->count == 3 && s->events[0].start == 1.0 && s->events[0].velocity == 1 && s->events[1].start == 2.0
              && s->events[2].start == 3.0, "sorted, velocity clamped");
        store->release();
        check(!store->set(mixed.data(), 1, 0.0).ok && !store->set(mixed.data(), 1, NAN).ok, "bad loop length");
        check(store->clear() && store->eventCount() == 0, "clear");
    }

    // 9. Writers racing the audio thread: 3000 swaps against a reader that must only ever see a
    //    whole sequence (every event of a version carries that version's note and count).
    {
        auto store = std::make_unique<seqplay::SequenceStore>();
        std::atomic<bool> stop{false};
        std::atomic<int> torn{0}, reads{0};
        std::thread reader([&] {
            while (!stop.load()) {
                const seqplay::Sequence* s = store->acquire();
                if (s) {
                    const int expectNote = (int)(s->version % 100);
                    const int expectCount = 1 + (int)(s->version % 50);
                    if (s->count != expectCount) torn.fetch_add(1);
                    for (int i = 0; i < s->count; ++i) if (s->events[i].note != expectNote) { torn.fetch_add(1); break; }
                    reads.fetch_add(1);
                }
                store->release();
            }
        });
        std::vector<seqplay::Event> ev;
        for (uint64_t v = 1; v <= 3000; ++v) {
            ev.assign((size_t)(1 + v % 50), seqNote(0.0, 1.0, (int)(v % 100)));
            for (size_t i = 0; i < ev.size(); ++i) ev[i].start = 0.01 * (double)i;
            if (!store->set(ev.data(), (int)ev.size(), 4.0).ok) torn.fetch_add(1000);
        }
        stop.store(true);
        reader.join();
        check(torn.load() == 0 && reads.load() > 0, "swaps: torn " + std::to_string(torn.load()));
    }

    // 10. A block too full for its starts never loses a release: 128 notes at beat 0 (length 0.1)
    //     and 400 drum hits at 0.05–0.09, 4096-sample blocks. Every note-on gets its note-off.
    {
        auto store = std::make_unique<seqplay::SequenceStore>();
        std::vector<seqplay::Event> ev;
        for (int n = 0; n < 128; ++n) ev.push_back(seqNote(0.0, 0.1, n));
        for (int i = 0; i < 400; ++i) ev.push_back(seqDrum(0.05 + 0.04 * i / 400.0, i % 9));
        check(store->set(ev.data(), (int)ev.size(), 4.0).ok, "full block: set");
        auto player = std::make_unique<seqplay::SequencePlayer>();
        player->setStore(store.get());
        int ons[128] = {0}, offs[128] = {0};
        auto sink = [&](const seqplay::Output& o) { if (o.kind == seqplay::EventKind::Note) (o.on ? ons : offs)[o.note]++; };
        const int N = 4096;
        double beat = 0.0;
        for (int b = 0; b < 4; ++b) {
            player->process(beat, 120.0, true, false, 48000.0, N, sink);
            beat += N * 120.0 / 60.0 / 48000.0;
        }
        int stuck = 0, started = 0;
        for (int n = 0; n < 128; ++n) { started += ons[n]; stuck += ons[n] != offs[n] ? 1 : 0; }
        check(stuck == 0 && started > 0 && player->activeNoteCount() == 0,
              "full block: " + std::to_string(stuck) + " notes without a note-off");
        player->process(beat, 120.0, false, false, 48000.0, N, sink);
        check(player->activeNoteCount() == 0, "full block: nothing held after stop");
    }

    // 11. Bounded work: loops under 1/64 beat are refused; a block spanning thousands of passes
    //     (or a transport far past 2^53 passes) walks at most kMaxPassesPerBlock of them.
    {
        auto store = std::make_unique<seqplay::SequenceStore>();
        seqplay::Event e = seqNote(0.0, 1e-10, 60);
        check(!store->set(&e, 1, 1e-9).ok && !store->set(&e, 1, seqplay::kMinLoopBeats * 0.5).ok, "tiny loop refused");
        check(store->set(&e, 1, seqplay::kMinLoopBeats).ok, "1/64 loop taken");
        e.skipBefore = 1e300; // every firing skipped: only the pass walk costs
        check(store->set(&e, 1, seqplay::kMinLoopBeats).ok, "1/64 loop, skipped");
        auto player = std::make_unique<seqplay::SequencePlayer>();
        player->setStore(store.get());
        auto sink = [&](const seqplay::Output&) {};
        const auto t0 = std::chrono::steady_clock::now();
        // 1e6 bpm: one 4096-sample block is 1422 beats = 91000 passes.
        player->process(0.0, 1.0e6, true, false, 48000.0, 4096, sink);
        player->process(1422.2, 1.0e6, true, false, 48000.0, 4096, sink);
        // Past 2^53 passes: pass + 1.0 no longer moves.
        player->process(1.0e17, 120.0, true, true, 48000.0, 4096, sink);
        player->process(1.0e17 + 0.17, 120.0, true, false, 48000.0, 4096, sink);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        check(ms < 50.0, "pass walk bounded (" + std::to_string(ms) + " ms)");
    }

    check(allocs == 0, "allocations " + std::to_string(allocs));
    const bool pass = failed == 0;
    std::printf("seq   sequence player: exact offsets, wrap, tempo, jump, swap, mute, count-in, overlap, store  %-4s"
                " %d failed checks%s\n", pass ? "PASS" : "FAIL", failed, detail.c_str());
    std::fflush(stdout);
    return pass;
}

// Through PolySynthProcessor: a sequence note sounds from its sample, a drum hit starts on its
// sample, the player's held note and the sequence's note of the same pitch never cut each other,
// the engine's allNotesOff and a stop end the sequence's voices, and with the arpeggiator on the
// sequence's notes feed it. No allocation.
static bool runSequenceVoices() {
    const double sr = 48000.0;
    int failed = 0;
    std::string detail;
    auto check = [&](bool ok, const std::string& what) { if (!ok) { ++failed; detail += " [" + what + "]"; } };
    struct Rig {
        TransportEngine transport;
        PolySynthProcessor synth;
        std::unique_ptr<seqplay::SequenceStore> store = std::make_unique<seqplay::SequenceStore>();
        seqplay::NoteLog log;
        Rig(double sr) {
            synth.setTransport(&transport);
            synth.setRateAndBufferSizeDetails(sr, 512);
            synth.prepareToPlay(sr, 512);
            synth.setWaveform(Waveform::Square);
            synth.setAttack(1.0);
            synth.setRelease(1.0);
            synth.setMasterLevel(0.5);
            synth.setSequenceStore(store.get());
            synth.setSequenceNoteLog(&log);
        }
    };
    uint64_t allocs = 0;
    auto render = [&](Rig& rig, int total, std::vector<float>& out, int block = 471,
                      const std::function<void(int)>& before = {}) {
        juce::AudioBuffer<float> buf(2, 1024);
        juce::MidiBuffer midi;
        out.assign((size_t)total, 0.0f);
        for (int done = 0; done < total;) {
            const int n = std::min(block, total - done);
            if (before) before(done);
            buf.setSize(2, n, false, false, true);
            gRenderAllocs.store(0); gRenderFrees.store(0);
            tCountAllocs = true;
            rig.synth.processBlock(buf, midi);
            tCountAllocs = false;
            allocs += gRenderAllocs.load() + gRenderFrees.load();
            std::memcpy(&out[(size_t)done], buf.getReadPointer(0), (size_t)n * sizeof(float));
            done += n;
        }
    };
    auto firstAbove = [](const std::vector<float>& x, int from, float level) {
        for (int i = from; i < (int)x.size(); ++i) if (std::fabs(x[(size_t)i]) > level) return i;
        return -1;
    };
    // 1. Onsets: a note at beat 0.3 (sample 7200), a drum hit at beat 0.7 (16800); 471-sample blocks.
    {
        Rig rig(sr);
        std::vector<seqplay::Event> ev = { seqNote(0.3, 0.2, 69, 127) };
        rig.store->set(ev.data(), 1, 4.0);
        rig.transport.start();
        rig.log.listening.store(true);
        std::vector<float> out;
        render(rig, 14400, out);
        const int onset = firstAbove(out, 0, 0.0f);
        check(onset >= 7200 && onset <= 7202, "note onset " + std::to_string(onset));
        int nonZeroBefore = 0;
        for (int i = 0; i < 7200; ++i) nonZeroBefore += out[(size_t)i] != 0.0f ? 1 : 0;
        check(nonZeroBefore == 0, "silent before the note");
        // Released at beat 0.5 (12000): silent again within the 1 ms release (+ the fade tail).
        int lastLoud = -1;
        for (int i = 0; i < 14400; ++i) if (std::fabs(out[(size_t)i]) > 1e-4f) lastLoud = i;
        check(lastLoud >= 12000 && lastLoud < 12000 + 200, "released at its end (" + std::to_string(lastLoud) + ")");
        int logged = 0;
        rig.log.drain([&](const seqplay::NoteLog::Entry& e) { logged += (e.note == 69 && std::fabs(e.atBeat - (e.on ? 0.3 : 0.5)) < 1e-9) ? 1 : 0; });
        check(logged == 2, "note log on + off");
        check(rig.synth.noteQueue().lastNoteStarted.load() == 69, "lastNoteStarted");

        Rig drums(sr);
        std::vector<seqplay::Event> hit = { seqDrum(0.7, 0, 127) };
        drums.store->set(hit.data(), 1, 4.0);
        drums.transport.start();
        render(drums, 19200, out);
        const int hitAt = firstAbove(out, 0, 0.0f);
        check(hitAt >= 16800 && hitAt <= 16802, "drum onset " + std::to_string(hitAt));
    }
    // 2. Holds: the player holds 60; the sequence plays 60 for half a beat; its release leaves the
    //    player's voice sounding. Then the sequence holds 64 and the player's 64 on/off leaves it.
    {
        Rig rig(sr);
        std::vector<seqplay::Event> ev = { seqNote(0.25, 0.25, 60), seqNote(1.0, 2.0, 64) };
        rig.store->set(ev.data(), 2, 4.0);
        std::vector<float> out;
        rig.synth.noteOn(60, 100);
        rig.transport.start();
        render(rig, 18000, out); // past the sequence's 60 (6000..12000)
        check(rig.synth.diag.activeVoices.load() == 1, "player's 60 still sounding after the sequence's release ("
              + std::to_string(rig.synth.diag.activeVoices.load()) + ")");
        rig.synth.noteOff(60);
        render(rig, 12000, out); // to beat 1.25: the sequence's 64 sounding
        check(rig.synth.diag.activeVoices.load() == 1, "sequence 64 sounding");
        rig.synth.noteOn(64, 100);
        render(rig, 2400, out);
        rig.synth.noteOff(64);
        render(rig, 4800, out);
        check(rig.synth.diag.activeVoices.load() == 1, "the player's 64 off leaves the sequence's 64 ("
              + std::to_string(rig.synth.diag.activeVoices.load()) + ")");
        // The engine's allNotesOff ends the sequence's note too.
        rig.synth.allNotesOff();
        render(rig, 2400, out);
        check(rig.synth.diag.activeVoices.load() == 0 && rig.synth.sequence().activeNoteCount() == 0, "allNotesOff");
        // Stop with a note sounding: nothing left.
        render(rig, 48000 * 2, out); // next pass: 60, then 64 from beat 5
        rig.transport.stop();
        render(rig, 2400, out);
        check(rig.synth.diag.activeVoices.load() == 0, "stop leaves no voice");
    }
    // 3. Arp on: the sequence's notes feed it (as notes sent with noteOn did); arp steps sound.
    {
        Rig rig(sr);
        ArpSettings s = ArpSettings::defaults();
        s.enabled = true; s.gate = 0.5f;
        rig.synth.arp().setSettings(s);
        std::vector<seqplay::Event> ev = { seqNote(0.0, 1.0, 60), seqNote(0.0, 1.0, 64) };
        rig.store->set(ev.data(), 2, 4.0);
        rig.transport.start();
        std::vector<float> out;
        render(rig, 12000, out);
        check(rig.synth.arp().isActive(), "arp running on the sequence's notes");
        int onsets = 0;
        for (int k = 0; k < 2; ++k) onsets += firstAbove(out, k * 6000, 0.0f) >= k * 6000 ? 1 : 0;
        check(onsets == 2, "arp steps sound");
        render(rig, 24000, out); // past beat 1: the sequence released them
        check(!rig.synth.arp().isActive() || rig.synth.arp().getSettings().latch, "arp stops after the release");
    }
    // 4. Arp on, one pitch from two sources: the sequence's release of 60 leaves the pad's 60 held
    //    in the arp; the pad's release of 64 leaves the sequence's 64.
    {
        Rig rig(sr);
        ArpSettings s = ArpSettings::defaults();
        s.enabled = true; s.gate = 0.5f;
        rig.synth.arp().setSettings(s);
        std::vector<seqplay::Event> ev = { seqNote(0.25, 0.25, 60), seqNote(1.0, 2.0, 64) };
        rig.store->set(ev.data(), 2, 4.0);
        std::vector<float> out;
        rig.synth.noteOn(60, 100);
        rig.transport.start();
        render(rig, 18000, out); // past the sequence's 60 (beats 0.25..0.5)
        check(rig.synth.arp().isActive() && rig.synth.arp().heldCount() == 1,
              "arp: the sequence's 60 off leaves the pad's 60 (" + std::to_string(rig.synth.arp().heldCount()) + ")");
        rig.synth.noteOff(60);
        render(rig, 12000, out); // beat 1.25: only the sequence's 64
        check(rig.synth.arp().heldCount() == 1, "arp: the pad's 60 off");
        rig.synth.noteOn(64, 100);
        render(rig, 2400, out);
        rig.synth.noteOff(64);
        render(rig, 4800, out);
        check(rig.synth.arp().isActive() && rig.synth.arp().heldCount() == 1,
              "arp: the pad's 64 off leaves the sequence's 64 (" + std::to_string(rig.synth.arp().heldCount()) + ")");
        render(rig, 48000, out); // past beat 3: the sequence's 64 ends
        check(rig.synth.arp().heldCount() == 0 && !rig.synth.arp().isActive(), "arp: both released");
    }
    // 5. Arp latch: notes only the sequence held do not latch (a stop leaves the arp silent, the idle
    //    stop is not held off); the pad's notes still latch.
    {
        Rig rig(sr);
        ArpSettings s = ArpSettings::defaults();
        s.enabled = true; s.gate = 0.5f; s.latch = true;
        rig.synth.arp().setSettings(s);
        std::vector<seqplay::Event> ev = { seqNote(0.0, 2.0, 60), seqNote(0.0, 2.0, 67) };
        rig.store->set(ev.data(), 2, 4.0);
        rig.transport.start();
        std::vector<float> out;
        render(rig, 12000, out);
        check(rig.synth.arp().heldCount() == 2, "latch: sequence notes held");
        rig.transport.stop();
        render(rig, 2400, out);
        check(rig.synth.arp().heldCount() == 0 && !rig.synth.arp().isActive(), "latch: stop leaves nothing in the arp ("
              + std::to_string(rig.synth.arp().heldCount()) + ")");
        render(rig, 4800, out);
        float tail = 0.0f;
        for (int i = 2400; i < 4800; ++i) tail = std::max(tail, std::fabs(out[(size_t)i]));
        check(tail == 0.0f, "latch: silent after the stop");
        rig.synth.noteOn(62, 100);
        render(rig, 2400, out);
        rig.synth.noteOff(62);
        render(rig, 2400, out);
        check(rig.synth.arp().heldCount() == 1 && rig.synth.arp().isActive(), "latch: the pad's note still latches");
        rig.synth.allNotesOff();
        render(rig, 2400, out);
    }
    check(allocs == 0, "allocations " + std::to_string(allocs));
    const bool pass = failed == 0;
    std::printf("seqv  sequence through PolySynthProcessor: onsets, drum hit, shared holds, allNotesOff, stop, arp  %-4s"
                " %d failed checks%s\n", pass ? "PASS" : "FAIL", failed, detail.c_str());
    std::fflush(stdout);
    return pass;
}

// The background policy's decisions (ios/BackgroundPolicy.h).
static bool runBackgroundPolicy() {
    int failed = 0;
    std::string detail;
    auto check = [&](bool ok, const std::string& what) { if (!ok) { ++failed; detail += " [" + what + "]"; } };
    using namespace bgpolicy;
    Policy unset;
    Policy keep; keep.set = true; keep.keepAlive = true; keep.idleStopSeconds = 30.0;
    Policy pause = keep; pause.keepAlive = false;
    Activity idle, playing, voice, held, seqHeld, arp;
    playing.transportPlaying = true;
    voice.voicesSounding = 1;
    held.inputHolds = 1;
    seqHeld.sequenceNotes = 2;
    arp.arpRunning = true;
    check(!isBusy(idle) && isBusy(playing) && isBusy(voice) && isBusy(held) && isBusy(seqHeld) && isBusy(arp), "busy");
    {
        IdleTracker t;
        check(!t.check(keep, idle, 100.0) && t.idleStopAt() < 0, "foreground never stops");
        check(!t.enterBackground(keep, 100.0) && t.state() == State::Background, "keepAlive: no pause");
        check(!t.check(keep, playing, 110.0) && t.idleStopAt() == 140.0, "busy pushes the stop");
        check(!t.check(keep, idle, 139.9), "idle 29.9 s");
        check(t.check(keep, idle, 140.0) && t.state() == State::IdleStopped && t.idleStopAt() < 0, "idle 30 s: stop");
        check(!t.check(keep, idle, 200.0), "stopped once");
        t.markRestarted(300.0);
        check(t.state() == State::Background && t.check(keep, idle, 330.0), "restarted in the background, idle again");
        check(t.enterForeground() && t.state() == State::Foreground, "foreground after a stop: restart");
        check(!t.enterForeground(), "no restart when it was not stopped");
    }
    {
        IdleTracker t;
        check(t.enterBackground(pause, 0.0), "keepAlive off: pause on background");
        check(!t.check(pause, voice, 10.0) && t.check(pause, idle, 40.0), "then the idle stop");
    }
    {
        IdleTracker t;
        check(!t.enterBackground(unset, 0.0) && !t.check(unset, idle, 1e6) && t.idleStopAt() < 0, "unset: nothing");
    }
    check(sanitizeIdleStopSeconds(NAN) == 30.0 && sanitizeIdleStopSeconds(0.0) == 1.0 && sanitizeIdleStopSeconds(1e9) == 3600.0,
          "sanitize");
    // wantsExclusiveSession(current, nowPlayingSet, transportPlaying, inBackground, pausedByRemote)
    check(!wantsExclusiveSession(false, false, true, true, true), "exclusive: never without Now Playing");
    check(!wantsExclusiveSession(false, true, true, false, false), "exclusive: foreground Play does not flip it");
    check(!wantsExclusiveSession(false, true, false, true, false), "exclusive: not on entering the background stopped");
    check(wantsExclusiveSession(false, true, true, true, false), "exclusive: in the background while playing");
    check(wantsExclusiveSession(true, true, false, true, true), "exclusive: kept after a lock-screen pause");
    check(!wantsExclusiveSession(true, true, false, true, false), "exclusive: dropped when stopped otherwise");
    check(wantsExclusiveSession(true, true, true, false, false), "exclusive: kept in the foreground while playing");
    check(!wantsExclusiveSession(true, true, false, false, false), "exclusive: dropped on stop in the foreground");
    check(std::string(stateName(State::IdleStopped)) == "idleStopped", "names");
    const bool pass = failed == 0;
    std::printf("bg    background policy: busy, idle stop after 30 s, pause, unset, restart, exclusive session  %-4s"
                " %d failed checks%s\n", pass ? "PASS" : "FAIL", failed, detail.c_str());
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
    if (want("btf")) unexpected += runBackingTrackFollow() ? 0 : 1;
    if (want("bsf")) unexpected += runBackingTrackSwapFade() ? 0 : 1;
    if (want("btr")) unexpected += runBackingTrackRegion() ? 0 : 1;
    if (want("btl")) unexpected += runBackingTrackLimits() ? 0 : 1;
    if (want("bsr")) unexpected += runBackingTrackSwapInAcquire() ? 0 : 1;
    if (want("bts")) unexpected += runBackingTrackSwapStress() ? 0 : 1;
    if (want("aux0")) unexpected += runAuxSilent() ? 0 : 1;
    if (want("rsa")) unexpected += runAuxRebuildState() ? 0 : 1;
    if (want("an")) unexpected += runAnalysis() ? 0 : 1;
    if (want("arp")) unexpected += runArp() ? 0 : 1;
    if (want("arpv")) unexpected += runArpVoices() ? 0 : 1;
    if (want("sp")) unexpected += runSpectrum() ? 0 : 1;
    if (want("dr")) unexpected += runDrums() ? 0 : 1;
    if (want("drs")) unexpected += runDrumsInSynth() ? 0 : 1;
    if (want("drb")) unexpected += runDrumBudget() ? 0 : 1;
    if (want("drm")) unexpected += runMidTom() ? 0 : 1;
    if (want("drd")) unexpected += runDrumDefaultsMatchJs() ? 0 : 1;
    if (want("lat")) unexpected += runLatencyStats() ? 0 : 1;
    if (want("stall")) unexpected += runStallWindow() ? 0 : 1;
    if (want("stlog")) unexpected += runStallLog() ? 0 : 1;
    if (want("rate")) unexpected += runDisplayRate() ? 0 : 1;
    if (want("bsel")) unexpected += runDeviceFormat() ? 0 : 1;
    if (want("broute")) unexpected += runDeviceRoutes() ? 0 : 1;
    if (want("mip")) unexpected += runMidiParsers() ? 0 : 1;
    if (want("mim")) unexpected += runMidiMap() ? 0 : 1;
    if (want("mih")) unexpected += runMidiHolds() ? 0 : 1;
    if (want("mic")) unexpected += runMidiEntry() ? 0 : 1;
    if (want("own")) unexpected += runOwnerToken() ? 0 : 1;
    if (want("sm")) unexpected += runSamplerCore() ? 0 : 1;
    if (want("smb")) unexpected += runSamplerBudget() ? 0 : 1;
    if (want("sml")) unexpected += runSamplerLoader() ? 0 : 1;
    if (want("sms")) unexpected += runSamplerSwapStress() ? 0 : 1;
    if (want("smq")) unexpected += runSamplerInSynth() ? 0 : 1;
    if (want("smd")) unexpected += runSamplerDefaultsMatchJs() ? 0 : 1;
    if (want("seq")) unexpected += runSequence() ? 0 : 1;
    if (want("seqv")) unexpected += runSequenceVoices() ? 0 : 1;
    if (want("bg")) unexpected += runBackgroundPolicy() ? 0 : 1;
    std::printf("\npeak/zero/1st-sound are over the 'note held' window; synth# = PolySynth::processBlock calls\n"
                "(whole case), synthPk = peak inside the synth before the graph.\n"
                "Case (i) logs jasserts from Voice::prepare(0 Hz) during doSetup; expected, it is re-prepared.\n");

    juce::shutdownJuce_GUI();
    std::printf("%d unexpected result(s) (g2 is expected to FAIL)\n", unexpected);
    return unexpected == 0 ? 0 : 1;
}
