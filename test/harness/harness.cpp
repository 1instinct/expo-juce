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

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
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
void free(void* p) { __libc_free(p); }
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
};

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
    // ── doSetup replica (ios/ExpoJuceAudioEngine.mm, HEAD), steps 2 and 5–11 ──
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
    std::printf("rs    rebuild carry-over (settings, tempo 90, playing, pending seek)  %-4s synth %d transport %d"
                " continues %d (pos %.4f)\n", synthOk && transportOk && continuesOk ? "PASS" : "FAIL",
                synthOk, transportOk, continuesOk, newTransport.getPosition());
    std::fflush(stdout);
    return synthOk && transportOk && continuesOk;
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
    std::printf("\npeak/zero/1st-sound are over the 'note held' window; synth# = PolySynth::processBlock calls\n"
                "(whole case), synthPk = peak inside the synth before the graph.\n"
                "Case (i) logs jasserts from Voice::prepare(0 Hz) during doSetup; expected, it is re-prepared.\n");

    juce::shutdownJuce_GUI();
    std::printf("%d unexpected result(s) (g2 is expected to FAIL)\n", unexpected);
    return unexpected == 0 ? 0 : 1;
}
