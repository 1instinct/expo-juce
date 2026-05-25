#import "BeeperAudioEngine.h"
#import <AVFoundation/AVFoundation.h>
#include "JuceConfig.h"
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "PolySynthProcessor.h"
#include "TransportEngine.h"

// Initialize JUCE at dylib load time — BEFORE any static C++ objects run.
// This is critical: without MessageManager, JUCE statics crash.
__attribute__((constructor))
static void initJuceEarly() {
    juce::MessageManager::getInstance();
    juce::initialiseJuce_GUI();
}

// Lightweight AudioIODeviceCallback that drives an AudioProcessorGraph.
// Replaces juce::AudioProcessorPlayer (from juce_audio_utils, which we don't use).
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

@interface BeeperAudioEngine ()
@property (nonatomic, assign) bool isReady;
@property (nonatomic, copy) BeatCallback beatCallback;
@property (nonatomic, strong) dispatch_source_t beatTimer;
@end

// C++ members stored as raw pointers (ObjC class can't have C++ members inline).
// IMPORTANT: No JUCE smart pointers or std::unique_ptr at file scope —
// their constructors/destructors engage JUCE internals before MessageManager is ready.
static juce::AudioDeviceManager* sDeviceManager = nullptr;
static GraphPlayer* sPlayer = nullptr;
static juce::AudioProcessorGraph* sGraph = nullptr;
static PolySynthProcessor* sSynth = nullptr; // owned by graph
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
    }
    return self;
}

- (void)setup {
    NSAssert([NSThread isMainThread], @"setup() must be called on main thread");

    if (self.isReady) {
        NSLog(@"[BeeperAudioEngine] Already initialized, skipping");
        return;
    }

    NSLog(@"[BeeperAudioEngine] Initializing JUCE audio stack...");

    // 0. Configure AVAudioSession before JUCE takes over
    NSError *sessionError = nil;
    AVAudioSession *session = [AVAudioSession sharedInstance];
    [session setCategory:AVAudioSessionCategoryPlayAndRecord
             withOptions:(AVAudioSessionCategoryOptionMixWithOthers |
                          AVAudioSessionCategoryOptionDefaultToSpeaker)
                   error:&sessionError];
    if (sessionError) {
        NSLog(@"[BeeperAudioEngine] AVAudioSession category error: %@", sessionError);
    }
    sessionError = nil;
    [session setActive:YES error:&sessionError];
    if (sessionError) {
        NSLog(@"[BeeperAudioEngine] AVAudioSession activation error: %@", sessionError);
    }

    // 1. Initialize MessageManager (attaches to existing CFRunLoop)
    juce::MessageManager::getInstance();

    // 2. Create transport
    sTransport = new TransportEngine();

    // 3. Create AudioDeviceManager
    sDeviceManager = new juce::AudioDeviceManager();

    // 4. Initialize device: 0 inputs, 2 outputs (stereo)
    juce::String error = sDeviceManager->initialise(0, 2, nullptr, true);
    if (error.isNotEmpty()) {
        NSLog(@"[BeeperAudioEngine] AudioDeviceManager init failed: %s", error.toRawUTF8());
        delete sDeviceManager;
        sDeviceManager = nullptr;
        delete sTransport;
        sTransport = nullptr;
        return;
    }

    // 5. Create AudioProcessorGraph
    sGraph = new juce::AudioProcessorGraph();
    sGraph->setPlayConfigDetails(0, 2, sDeviceManager->getAudioDeviceSetup().sampleRate,
                                  sDeviceManager->getAudioDeviceSetup().bufferSize);
    sGraph->prepareToPlay(sDeviceManager->getAudioDeviceSetup().sampleRate,
                           sDeviceManager->getAudioDeviceSetup().bufferSize);

    // 6. Add PolySynthProcessor node
    auto synthProcessor = std::make_unique<PolySynthProcessor>();
    sSynth = synthProcessor.get();
    sSynth->setTransport(sTransport);
    auto synthNode = sGraph->addNode(std::move(synthProcessor));

    // 7. Add output node
    auto outputNode = sGraph->addNode(
        std::make_unique<juce::AudioProcessorGraph::AudioGraphIOProcessor>(
            juce::AudioProcessorGraph::AudioGraphIOProcessor::audioOutputNode));

    // 8. Connect synth → output (stereo: channels 0 and 1)
    sGraph->addConnection({
        {synthNode->nodeID, 0},
        {outputNode->nodeID, 0}
    });
    sGraph->addConnection({
        {synthNode->nodeID, 1},
        {outputNode->nodeID, 1}
    });

    // 9. Create player and attach to device
    sPlayer = new GraphPlayer();
    sPlayer->setProcessor(sGraph.get());
    sDeviceManager->addAudioCallback(sPlayer);

    self.isReady = true;
    NSLog(@"[BeeperAudioEngine] JUCE audio stack initialized (sr=%.0f, buf=%d)",
          sDeviceManager->getAudioDeviceSetup().sampleRate,
          sDeviceManager->getAudioDeviceSetup().bufferSize);
}

- (void)shutdown {
    NSAssert([NSThread isMainThread], @"shutdown() must be called on main thread");

    if (!self.isReady) return;

    NSLog(@"[BeeperAudioEngine] Shutting down...");

    [self stopBeatTimer];

    if (sSynth) {
        sSynth->allNotesOff();
    }

    if (sDeviceManager && sPlayer) {
        sDeviceManager->removeAudioCallback(sPlayer);
    }

    if (sPlayer) {
        sPlayer->setProcessor(nullptr);
        delete sPlayer;
        sPlayer = nullptr;
    }

    sSynth = nullptr; // owned by graph, don't delete
    delete sGraph;
    sGraph = nullptr;

    if (sDeviceManager) {
        sDeviceManager->closeAudioDevice();
        delete sDeviceManager;
        sDeviceManager = nullptr;
    }

    if (sTransport) {
        delete sTransport;
        sTransport = nullptr;
    }

    self.isReady = false;
    NSLog(@"[BeeperAudioEngine] Shutdown complete");
}

// ── Synth forwarding ──

- (void)noteOn:(int)note velocity:(int)velocity {
    if (!self.isReady || !sSynth) {
        NSLog(@"[BeeperAudioEngine] noteOn ignored — engine not initialized. Call setup() first.");
        return;
    }
    sSynth->noteOn(note, velocity);
}

- (void)noteOff:(int)note {
    if (!self.isReady || !sSynth) {
        NSLog(@"[BeeperAudioEngine] noteOff ignored — engine not initialized.");
        return;
    }
    sSynth->noteOff(note);
}

- (void)allNotesOff {
    if (!self.isReady || !sSynth) return;
    sSynth->allNotesOff();
}

- (void)setWaveform:(NSString *)waveform {
    if (!self.isReady || !sSynth) return;
    if ([waveform isEqualToString:@"square"]) {
        sSynth->setWaveform(Waveform::Square);
    } else if ([waveform isEqualToString:@"saw"]) {
        sSynth->setWaveform(Waveform::Saw);
    } else if ([waveform isEqualToString:@"triangle"]) {
        sSynth->setWaveform(Waveform::Triangle);
    } else {
        sSynth->setWaveform(Waveform::Sine);
    }
}

- (void)setFilterCutoff:(double)hz {
    if (self.isReady && sSynth) sSynth->setFilterCutoff(hz);
}

- (void)setFilterResonance:(double)q {
    if (self.isReady && sSynth) sSynth->setFilterResonance(q);
}

- (void)setAttack:(double)ms {
    if (self.isReady && sSynth) sSynth->setAttack(ms);
}

- (void)setDecay:(double)ms {
    if (self.isReady && sSynth) sSynth->setDecay(ms);
}

- (void)setSustain:(double)level {
    if (self.isReady && sSynth) sSynth->setSustain(level);
}

- (void)setRelease:(double)ms {
    if (self.isReady && sSynth) sSynth->setRelease(ms);
}

- (void)setDetune:(double)cents {
    if (self.isReady && sSynth) sSynth->setDetune(cents);
}

- (void)setMasterLevel:(double)level {
    if (self.isReady && sSynth) sSynth->setMasterLevel(level);
}

// ── Transport ──

- (void)startTransport {
    if (self.isReady && sTransport) sTransport->start();
}

- (void)stopTransport {
    if (self.isReady && sTransport) sTransport->stop();
}

- (void)setTempo:(double)bpm {
    if (self.isReady && sTransport) sTransport->setTempo(bpm);
}

- (void)setTransportPosition:(double)beat {
    if (self.isReady && sTransport) sTransport->setPosition(beat);
}

- (double)getTransportPosition {
    return (self.isReady && sTransport) ? sTransport->getPosition() : 0.0;
}

- (double)getTempo {
    return (self.isReady && sTransport) ? sTransport->getTempo() : 120.0;
}

- (BOOL)isTransportPlaying {
    return (self.isReady && sTransport) ? sTransport->isPlaying() : NO;
}

// ── Beat events ──

- (void)setBeatCallback:(BeatCallback)callback {
    self.beatCallback = callback;
}

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

- (void)dealloc {
    [self shutdown];
}

@end
