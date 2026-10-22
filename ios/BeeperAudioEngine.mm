#import "BeeperAudioEngine.h"
#import <AVFoundation/AVFoundation.h>
#include "JuceConfig.h"
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "PolySynthProcessor.h"
#include "TransportEngine.h"
#include <atomic>
#include <cmath>
#include <algorithm>

// Ensure MessageManager exists before any JUCE static initializers.
// On iOS, __attribute__((constructor)) runs on the main thread for
// statically-linked frameworks (which Expo modules are).
__attribute__((constructor))
static void initJuceMessageManager() {
    juce::MessageManager::getInstance();
}

// Lightweight AudioIODeviceCallback that drives an AudioProcessorGraph.
class GraphPlayer : public juce::AudioIODeviceCallback {
public:
    void setProcessor(juce::AudioProcessor* p) { processor = p; }

    // Any thread: ask the audio thread to mix a 440 Hz test tone into the
    // device output (after the graph) for durationMs.
    void requestTestTone(double durationMs) {
        if (!(durationMs > 0.0)) return;
        toneRequestMs.store((int)std::min(durationMs, 10000.0), std::memory_order_relaxed);
    }

    // Diagnostics: written on the audio thread, read from any thread (relaxed).
    std::atomic<uint64_t> callbackCount{0};
    std::atomic<uint64_t> graphProcessCount{0};
    std::atomic<uint64_t> nanCount{0};       // blocks with a non-finite output sample
    std::atomic<float> outputPeak{0.0f};     // decaying hold of block max |x| (graph output)
    std::atomic<uint64_t> toneSamplesMixed{0}; // cumulative test-tone samples mixed into output

    void audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                          int numInputChannels,
                                          float* const* outputChannelData,
                                          int numOutputChannels,
                                          int numSamples,
                                          const juce::AudioIODeviceCallbackContext& context) override {
        juce::ignoreUnused(inputChannelData, numInputChannels, context);
        callbackCount.fetch_add(1, std::memory_order_relaxed);

        if (!processor) {
            for (int i = 0; i < numOutputChannels; ++i)
                juce::FloatVectorOperations::clear(outputChannelData[i], numSamples);
        } else {
            buffer.setSize(numOutputChannels, numSamples, false, false, true);
            buffer.clear();
            juce::MidiBuffer midi;
            processor->processBlock(buffer, midi);
            graphProcessCount.fetch_add(1, std::memory_order_relaxed);
            for (int i = 0; i < numOutputChannels; ++i)
                juce::FloatVectorOperations::copy(outputChannelData[i], buffer.getReadPointer(i), numSamples);
        }

        // Measure graph output (before the test tone is mixed in).
        float blockPeak = 0.0f;
        bool nonFinite = false;
        for (int ch = 0; ch < numOutputChannels; ++ch) {
            const float* data = outputChannelData[ch];
            if (data == nullptr) continue;
            for (int s = 0; s < numSamples; ++s) {
                const float v = data[s];
                if (!std::isfinite(v)) nonFinite = true;
                else blockPeak = std::max(blockPeak, std::fabs(v));
            }
        }
        if (nonFinite) nanCount.fetch_add(1, std::memory_order_relaxed);
        const float held = outputPeak.load(std::memory_order_relaxed) * diagPeakDecay(numSamples, deviceSampleRate);
        outputPeak.store(std::max(blockPeak, held), std::memory_order_relaxed);

        mixTestTone(outputChannelData, numOutputChannels, numSamples);
    }

    void audioDeviceAboutToStart(juce::AudioIODevice* device) override {
        const double deviceSr = device->getCurrentSampleRate();
        if (deviceSr > 0.0) deviceSampleRate = deviceSr;
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
    // Audio thread only.
    void mixTestTone(float* const* outputChannelData, int numOutputChannels, int numSamples) {
        const int requestedMs = toneRequestMs.exchange(0, std::memory_order_relaxed);
        if (requestedMs > 0)
            toneSamplesRemaining = (int)(requestedMs * deviceSampleRate / 1000.0);
        if (toneSamplesRemaining <= 0 || numOutputChannels <= 0) return;

        const double twoPi = juce::MathConstants<double>::twoPi;
        const double increment = twoPi * 440.0 / deviceSampleRate;
        const int n = std::min(toneSamplesRemaining, numSamples);
        for (int s = 0; s < n; ++s) {
            const float v = 0.2f * (float)std::sin(tonePhase);
            tonePhase += increment;
            if (tonePhase >= twoPi) tonePhase -= twoPi;
            for (int ch = 0; ch < numOutputChannels; ++ch)
                if (outputChannelData[ch] != nullptr)
                    outputChannelData[ch][s] += v;
        }
        toneSamplesRemaining -= n;
        toneSamplesMixed.fetch_add((uint64_t)n, std::memory_order_relaxed);
    }

    juce::AudioProcessor* processor = nullptr;
    juce::AudioBuffer<float> buffer;

    double deviceSampleRate = 48000.0;   // set in audioDeviceAboutToStart
    std::atomic<int> toneRequestMs{0};   // any thread → audio thread
    int toneSamplesRemaining = 0;        // audio thread only
    double tonePhase = 0.0;              // audio thread only
};

@interface BeeperAudioEngine ()
@property (nonatomic, assign) bool isReady;
@property (nonatomic, readwrite) int setupStep;
@property (nonatomic, readwrite, copy) NSString *lastError;
@property (nonatomic, copy) BeatCallback beatCallback;
@property (nonatomic, strong) dispatch_source_t beatTimer;
@end

// Raw pointers — no JUCE smart pointers at file scope.
static juce::AudioDeviceManager* sDeviceManager = nullptr;
static GraphPlayer* sPlayer = nullptr;
static juce::AudioProcessorGraph* sGraph = nullptr;
static PolySynthProcessor* sSynth = nullptr;
static TransportEngine* sTransport = nullptr;

@implementation BeeperAudioEngine

+ (instancetype)sharedInstance {
    static BeeperAudioEngine *instance = nil;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        instance = [[self alloc] init];
    });
    return instance;
}

- (instancetype)init {
    self = [super init];
    if (self) {
        _isReady = false;
        _setupStep = -1;
    }
    return self;
}

- (BOOL)setup {
    if (![NSThread isMainThread]) {
        NSLog(@"[BeeperAudioEngine] ERROR: setup() called off main thread, dispatching to main");
        __block BOOL ok = NO;
        dispatch_sync(dispatch_get_main_queue(), ^{ ok = [self setup]; });
        return ok;
    }

    if (self.isReady) {
        NSLog(@"[BeeperAudioEngine] Already initialized, skipping");
        return YES;
    }

    self.setupStep = -1;
    self.lastError = nil;

    @try {
        [self doSetup];
    } @catch (NSException *exception) {
        NSLog(@"[BeeperAudioEngine] EXCEPTION in setup: %@ — %@", exception.name, exception.reason);
        [self noteFatal:[NSString stringWithFormat:@"exception %@: %@", exception.name, exception.reason]];
        [self cleanup];
    }

    if (!self.isReady && self.lastError == nil)
        [self noteFatal:@"setup did not complete"];
    return self.isReady ? YES : NO;
}

// Records a problem that does not abort setup (behaviour unchanged), so that
// diagnostics can still surface it. Appends to lastError.
- (void)noteNonFatal:(NSString *)message {
    NSLog(@"[BeeperAudioEngine] %@", message);
    self.lastError = self.lastError ? [NSString stringWithFormat:@"%@; %@", self.lastError, message] : message;
}

// Records the error that aborted setup; placed first, earlier non-fatal notes kept after it.
- (void)noteFatal:(NSString *)message {
    self.lastError = self.lastError ? [NSString stringWithFormat:@"%@; %@", message, self.lastError] : message;
}

- (void)doSetup {
    self.setupStep = 0;
    NSLog(@"[BeeperAudioEngine] Step 0: initialiseJuce_GUI");
    juce::initialiseJuce_GUI();

    self.setupStep = 1;
    NSLog(@"[BeeperAudioEngine] Step 1: AVAudioSession (Playback)");
    NSError *sessionError = nil;
    NSError *activationError = nil;
    AVAudioSession *session = [AVAudioSession sharedInstance];
    [session setCategory:AVAudioSessionCategoryPlayback
             withOptions:AVAudioSessionCategoryOptionMixWithOthers
                   error:&sessionError];
    if (sessionError) {
        NSLog(@"[BeeperAudioEngine] AVAudioSession category error: %@", sessionError);
    }
    [session setActive:YES error:&activationError];
    if (activationError) {
        NSLog(@"[BeeperAudioEngine] AVAudioSession activation error: %@", activationError);
    }
    // Non-fatal (setup continues, as before); recorded so diagnostics can show it.
    if (sessionError) {
        [self noteNonFatal:[NSString stringWithFormat:@"step 1 (non-fatal): AVAudioSession setCategory: %@",
                            sessionError.localizedDescription]];
    }
    if (activationError) {
        [self noteNonFatal:[NSString stringWithFormat:@"step 1 (non-fatal): AVAudioSession setActive: %@",
                            activationError.localizedDescription]];
    }

    self.setupStep = 2;
    NSLog(@"[BeeperAudioEngine] Step 2: TransportEngine");
    sTransport = new TransportEngine();

    self.setupStep = 3;
    NSLog(@"[BeeperAudioEngine] Step 3: AudioDeviceManager");
    sDeviceManager = new juce::AudioDeviceManager();

    self.setupStep = 4;
    NSLog(@"[BeeperAudioEngine] Step 4: initialise(0, 2)");
    juce::String error = sDeviceManager->initialise(0, 2, nullptr, true);
    if (error.isNotEmpty()) {
        NSLog(@"[BeeperAudioEngine] AudioDeviceManager init failed: %s", error.toRawUTF8());
        NSString *message = [NSString stringWithUTF8String:error.toRawUTF8()];
        [self noteFatal:[NSString stringWithFormat:@"AudioDeviceManager::initialise failed: %@",
                         message ?: @"(unprintable error)"]];
        [self cleanup];
        return;
    }
    if (sDeviceManager->getCurrentAudioDevice() == nullptr) {
        [self noteNonFatal:@"step 4 (non-fatal): initialise returned no error but no device is open"];
    }

    double sr = sDeviceManager->getAudioDeviceSetup().sampleRate;
    int bufSize = sDeviceManager->getAudioDeviceSetup().bufferSize;
    self.setupStep = 5;
    NSLog(@"[BeeperAudioEngine] Step 5: AudioProcessorGraph (sr=%.0f, buf=%d)", sr, bufSize);

    sGraph = new juce::AudioProcessorGraph();
    sGraph->setPlayConfigDetails(0, 2, sr, bufSize);

    self.setupStep = 6;
    NSLog(@"[BeeperAudioEngine] Step 6: PolySynthProcessor");
    auto synthProcessor = std::make_unique<PolySynthProcessor>();
    sSynth = synthProcessor.get();
    sSynth->setTransport(sTransport);

    self.setupStep = 7;
    NSLog(@"[BeeperAudioEngine] Step 7: addNode (synth)");
    auto synthNode = sGraph->addNode(std::move(synthProcessor));

    self.setupStep = 8;
    NSLog(@"[BeeperAudioEngine] Step 8: addNode (output)");
    auto outputNode = sGraph->addNode(
        std::make_unique<juce::AudioProcessorGraph::AudioGraphIOProcessor>(
            juce::AudioProcessorGraph::AudioGraphIOProcessor::audioOutputNode));

    self.setupStep = 9;
    NSLog(@"[BeeperAudioEngine] Step 9: connections");
    bool connectedL = sGraph->addConnection({{synthNode->nodeID, 0}, {outputNode->nodeID, 0}});
    bool connectedR = sGraph->addConnection({{synthNode->nodeID, 1}, {outputNode->nodeID, 1}});
    if (!connectedL || !connectedR) {
        [self noteNonFatal:[NSString stringWithFormat:@"step 9 (non-fatal): addConnection failed (L=%d, R=%d)",
                            connectedL ? 1 : 0, connectedR ? 1 : 0]];
    }

    self.setupStep = 10;
    NSLog(@"[BeeperAudioEngine] Step 10: prepareToPlay");
    sGraph->prepareToPlay(sr, bufSize);

    self.setupStep = 11;
    NSLog(@"[BeeperAudioEngine] Step 11: GraphPlayer + addAudioCallback");
    sPlayer = new GraphPlayer();
    sPlayer->setProcessor(sGraph);
    sDeviceManager->addAudioCallback(sPlayer);

    self.isReady = true;
    NSLog(@"[BeeperAudioEngine] DONE — audio stack initialized (sr=%.0f, buf=%d)", sr, bufSize);
}

- (void)cleanup {
    NSLog(@"[BeeperAudioEngine] Cleaning up after failure...");
    if (sDeviceManager && sPlayer) sDeviceManager->removeAudioCallback(sPlayer);
    delete sPlayer; sPlayer = nullptr;
    sSynth = nullptr;
    delete sGraph; sGraph = nullptr;
    if (sDeviceManager) { sDeviceManager->closeAudioDevice(); delete sDeviceManager; sDeviceManager = nullptr; }
    delete sTransport; sTransport = nullptr;
    self.isReady = false;
}

- (void)shutdown {
    if (![NSThread isMainThread]) return;
    if (!self.isReady) return;

    NSLog(@"[BeeperAudioEngine] Shutting down...");
    [self stopBeatTimer];
    if (sSynth) sSynth->allNotesOff();
    [self cleanup];
    NSLog(@"[BeeperAudioEngine] Shutdown complete");
}

// ── Diagnostics ──
// Called from the JS thread (sync Function). Audio-thread state is read via
// relaxed atomics; AVAudioSession is read on the calling thread.

- (NSDictionary<NSString *, id> *)diagnostics {
    NSMutableDictionary<NSString *, id> *d = [NSMutableDictionary dictionary];
    const bool ready = self.isReady;

    d[@"isReady"] = @(ready);
    d[@"setupStep"] = @(self.setupStep);
    d[@"lastError"] = self.lastError ?: @"";

    NSString *deviceName = @"";
    NSString *deviceType = @"";
    double sampleRate = 0.0;
    int bufferSize = 0;
    int outputLatencySamples = 0;
    if (ready && sDeviceManager) {
        if (juce::AudioIODevice* device = sDeviceManager->getCurrentAudioDevice()) {
            deviceName = [NSString stringWithUTF8String:device->getName().toRawUTF8()] ?: @"";
            deviceType = [NSString stringWithUTF8String:device->getTypeName().toRawUTF8()] ?: @"";
            sampleRate = device->getCurrentSampleRate();
            bufferSize = device->getCurrentBufferSizeSamples();
            outputLatencySamples = device->getOutputLatencyInSamples();
        }
    }
    d[@"deviceName"] = deviceName;
    d[@"deviceType"] = deviceType;
    d[@"sampleRate"] = @(sampleRate);
    d[@"bufferSize"] = @(bufferSize);
    d[@"outputLatencySamples"] = @(outputLatencySamples);

    GraphPlayer* player = ready ? sPlayer : nullptr;
    PolySynthProcessor* synth = ready ? sSynth : nullptr;
    const auto relaxed = std::memory_order_relaxed;

    d[@"callbackCount"] = @(player ? (double)player->callbackCount.load(relaxed) : 0.0);
    d[@"graphProcessCount"] = @(player ? (double)player->graphProcessCount.load(relaxed) : 0.0);
    d[@"outputPeak"] = @(player ? (double)player->outputPeak.load(relaxed) : 0.0);
    d[@"nanCount"] = @(player ? (double)player->nanCount.load(relaxed) : 0.0);
    d[@"toneSamplesMixed"] = @(player ? (double)player->toneSamplesMixed.load(relaxed) : 0.0);

    d[@"synthProcessCount"] = @(synth ? (double)synth->diag.processCount.load(relaxed) : 0.0);
    d[@"synthPeak"] = @(synth ? (double)synth->diag.peak.load(relaxed) : 0.0);
    d[@"notesQueued"] = @(synth ? (double)synth->diag.notesQueued.load(relaxed) : 0.0);
    d[@"notesConsumed"] = @(synth ? (double)synth->diag.notesConsumed.load(relaxed) : 0.0);
    d[@"notesDropped"] = @(synth ? (double)synth->diag.notesDropped.load(relaxed) : 0.0);
    d[@"activeVoices"] = @(synth ? synth->diag.activeVoices.load(relaxed) : 0);
    d[@"lastNoteToRenderMs"] = @(synth ? synth->diag.lastNoteToRenderMs.load(relaxed) : -1.0);

    AVAudioSession *session = [AVAudioSession sharedInstance];
    d[@"sessionCategory"] = session.category ?: @"";
    d[@"sessionSampleRate"] = @(session.sampleRate);
    d[@"sessionIOBufferMs"] = @(session.IOBufferDuration * 1000.0);
    d[@"sessionOutputLatencyMs"] = @(session.outputLatency * 1000.0);
    d[@"otherAudioPlaying"] = @([session isOtherAudioPlaying] ? YES : NO);

    NSMutableArray<NSString *> *routeOutputs = [NSMutableArray array];
    for (AVAudioSessionPortDescription *port in session.currentRoute.outputs) {
        if (port.portType) [routeOutputs addObject:port.portType];
    }
    d[@"routeOutputs"] = routeOutputs;

    return d;
}

- (BOOL)playTestTone:(double)durationMs {
    if (!self.isReady || !sPlayer || !sDeviceManager) return NO;
    if (!(durationMs > 0.0)) return NO;
    juce::AudioIODevice* device = sDeviceManager->getCurrentAudioDevice();
    if (device == nullptr || !device->isPlaying()) return NO;
    sPlayer->requestTestTone(durationMs);
    return YES;
}

// ── Synth forwarding ──

- (void)noteOn:(int)note velocity:(int)velocity {
    if (!self.isReady || !sSynth) return;
    sSynth->noteOn(note, velocity);
}

- (void)noteOff:(int)note {
    if (!self.isReady || !sSynth) return;
    sSynth->noteOff(note);
}

- (void)allNotesOff {
    if (!self.isReady || !sSynth) return;
    sSynth->allNotesOff();
}

- (void)setWaveform:(NSString *)waveform {
    if (!self.isReady || !sSynth) return;
    if ([waveform isEqualToString:@"square"]) sSynth->setWaveform(Waveform::Square);
    else if ([waveform isEqualToString:@"saw"]) sSynth->setWaveform(Waveform::Saw);
    else if ([waveform isEqualToString:@"triangle"]) sSynth->setWaveform(Waveform::Triangle);
    else sSynth->setWaveform(Waveform::Sine);
}

- (void)setFilterCutoff:(double)hz { if (self.isReady && sSynth) sSynth->setFilterCutoff(hz); }
- (void)setFilterResonance:(double)q { if (self.isReady && sSynth) sSynth->setFilterResonance(q); }
- (void)setAttack:(double)ms { if (self.isReady && sSynth) sSynth->setAttack(ms); }
- (void)setDecay:(double)ms { if (self.isReady && sSynth) sSynth->setDecay(ms); }
- (void)setSustain:(double)level { if (self.isReady && sSynth) sSynth->setSustain(level); }
- (void)setRelease:(double)ms { if (self.isReady && sSynth) sSynth->setRelease(ms); }
- (void)setDetune:(double)cents { if (self.isReady && sSynth) sSynth->setDetune(cents); }
- (void)setMasterLevel:(double)level { if (self.isReady && sSynth) sSynth->setMasterLevel(level); }

// ── Transport ──

- (void)startTransport { if (self.isReady && sTransport) sTransport->start(); }
- (void)stopTransport { if (self.isReady && sTransport) sTransport->stop(); }
- (void)setTempo:(double)bpm { if (self.isReady && sTransport) sTransport->setTempo(bpm); }
- (void)setTransportPosition:(double)beat { if (self.isReady && sTransport) sTransport->setPosition(beat); }
- (double)getTransportPosition { return (self.isReady && sTransport) ? sTransport->getPosition() : 0.0; }
- (double)getTempo { return (self.isReady && sTransport) ? sTransport->getTempo() : 120.0; }
- (BOOL)isTransportPlaying { return (self.isReady && sTransport) ? sTransport->isPlaying() : NO; }

// ── Beat events ──

- (void)setBeatCallback:(BeatCallback)callback { self.beatCallback = callback; }

- (void)startBeatTimer {
    if (self.beatTimer) return;
    if (!sSynth) return;

    self.beatTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
    dispatch_source_set_timer(self.beatTimer, DISPATCH_TIME_NOW, 16 * NSEC_PER_MSEC, 1 * NSEC_PER_MSEC);
    __weak BeeperAudioEngine *weakSelf = self;
    dispatch_source_set_event_handler(self.beatTimer, ^{
        BeeperAudioEngine *strongSelf = weakSelf;
        if (!strongSelf || !sSynth) return;
        BeatCallback cb = strongSelf.beatCallback;
        if (sSynth->beatState.crossed.exchange(false) && cb) {
            cb(sSynth->beatState.beat.load(), sSynth->beatState.bpm.load());
        }
    });
    dispatch_resume(self.beatTimer);
}

- (void)stopBeatTimer {
    if (self.beatTimer) {
        dispatch_source_cancel(self.beatTimer);
        self.beatTimer = nil;
    }
    self.beatCallback = nil;
}

- (void)dealloc { [self shutdown]; }

@end
