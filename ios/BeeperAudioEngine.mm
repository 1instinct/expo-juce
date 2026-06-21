#import "BeeperAudioEngine.h"
#import <AVFoundation/AVFoundation.h>
#include "JuceConfig.h"
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "PolySynthProcessor.h"
#include "TransportEngine.h"

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
    }
    return self;
}

- (void)setup {
    if (![NSThread isMainThread]) {
        NSLog(@"[BeeperAudioEngine] ERROR: setup() called off main thread, dispatching to main");
        dispatch_sync(dispatch_get_main_queue(), ^{ [self setup]; });
        return;
    }

    if (self.isReady) {
        NSLog(@"[BeeperAudioEngine] Already initialized, skipping");
        return;
    }

    @try {
        [self doSetup];
    } @catch (NSException *exception) {
        NSLog(@"[BeeperAudioEngine] EXCEPTION in setup: %@ — %@", exception.name, exception.reason);
        [self cleanup];
    }
}

- (void)doSetup {
    NSLog(@"[BeeperAudioEngine] Step 0: initialiseJuce_GUI");
    juce::initialiseJuce_GUI();

    NSLog(@"[BeeperAudioEngine] Step 1: AVAudioSession (Playback)");
    NSError *sessionError = nil;
    AVAudioSession *session = [AVAudioSession sharedInstance];
    [session setCategory:AVAudioSessionCategoryPlayback
             withOptions:AVAudioSessionCategoryOptionMixWithOthers
                   error:&sessionError];
    if (sessionError) {
        NSLog(@"[BeeperAudioEngine] AVAudioSession category error: %@", sessionError);
    }
    [session setActive:YES error:&sessionError];
    if (sessionError) {
        NSLog(@"[BeeperAudioEngine] AVAudioSession activation error: %@", sessionError);
    }

    NSLog(@"[BeeperAudioEngine] Step 2: TransportEngine");
    sTransport = new TransportEngine();

    NSLog(@"[BeeperAudioEngine] Step 3: AudioDeviceManager");
    sDeviceManager = new juce::AudioDeviceManager();

    NSLog(@"[BeeperAudioEngine] Step 4: initialise(0, 2)");
    juce::String error = sDeviceManager->initialise(0, 2, nullptr, true);
    if (error.isNotEmpty()) {
        NSLog(@"[BeeperAudioEngine] AudioDeviceManager init failed: %s", error.toRawUTF8());
        [self cleanup];
        return;
    }

    double sr = sDeviceManager->getAudioDeviceSetup().sampleRate;
    int bufSize = sDeviceManager->getAudioDeviceSetup().bufferSize;
    NSLog(@"[BeeperAudioEngine] Step 5: AudioProcessorGraph (sr=%.0f, buf=%d)", sr, bufSize);

    sGraph = new juce::AudioProcessorGraph();
    sGraph->setPlayConfigDetails(0, 2, sr, bufSize);

    NSLog(@"[BeeperAudioEngine] Step 6: PolySynthProcessor");
    auto synthProcessor = std::make_unique<PolySynthProcessor>();
    sSynth = synthProcessor.get();
    sSynth->setTransport(sTransport);

    NSLog(@"[BeeperAudioEngine] Step 7: addNode (synth)");
    auto synthNode = sGraph->addNode(std::move(synthProcessor));

    NSLog(@"[BeeperAudioEngine] Step 8: addNode (output)");
    auto outputNode = sGraph->addNode(
        std::make_unique<juce::AudioProcessorGraph::AudioGraphIOProcessor>(
            juce::AudioProcessorGraph::AudioGraphIOProcessor::audioOutputNode));

    NSLog(@"[BeeperAudioEngine] Step 9: connections");
    sGraph->addConnection({{synthNode->nodeID, 0}, {outputNode->nodeID, 0}});
    sGraph->addConnection({{synthNode->nodeID, 1}, {outputNode->nodeID, 1}});

    NSLog(@"[BeeperAudioEngine] Step 10: prepareToPlay");
    sGraph->prepareToPlay(sr, bufSize);

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
