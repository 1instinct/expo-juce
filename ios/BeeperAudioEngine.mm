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

#include "GraphPlayer.h"

// Ensure MessageManager exists before any JUCE static initializers.
// On iOS, __attribute__((constructor)) runs on the main thread for
// statically-linked frameworks (which Expo modules are).
__attribute__((constructor))
static void initJuceMessageManager() {
    juce::MessageManager::getInstance();
}

static const int kDefaultBufferSize = 128;
static const double kDefaultSampleRate = 48000.0;
static const int64_t kRecoveryDelayMs = 300; // let JUCE's own session handlers run first

@interface BeeperAudioEngine ()
@property (nonatomic, assign) bool isReady;
@property (nonatomic, readwrite) int setupStep;
@property (nonatomic, readwrite, copy) NSString *lastError;
@property (nonatomic, copy) BeatCallback beatCallback;
@property (nonatomic, strong) dispatch_source_t beatTimer;
// Options of the last setup call (reused by media-services-reset recovery).
@property (nonatomic, assign) int requestedBufferSize;
@property (nonatomic, assign) double requestedSampleRate;
// Session safeguard / recovery (main thread only). Counters are cumulative since launch.
@property (nonatomic, assign) BOOL observingSession;
@property (nonatomic, assign) BOOL isReassertingSession;
@property (nonatomic, assign) BOOL isRecovering;
@property (nonatomic, assign) int sessionReassertCount;
@property (nonatomic, assign) int interruptionCount;
@property (nonatomic, assign) int mediaResetCount;
@property (atomic, copy) NSString *lastRecoveryError; // atomic: diagnostics reads it off the main thread
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
        _requestedBufferSize = kDefaultBufferSize;
        _requestedSampleRate = kDefaultSampleRate;
    }
    return self;
}

- (BOOL)setup {
    return [self setupWithBufferSize:kDefaultBufferSize sampleRate:kDefaultSampleRate];
}

- (BOOL)setupWithBufferSize:(int)bufferSize sampleRate:(double)sampleRate {
    if (![NSThread isMainThread]) {
        NSLog(@"[BeeperAudioEngine] ERROR: setup() called off main thread, dispatching to main");
        __block BOOL ok = NO;
        dispatch_sync(dispatch_get_main_queue(), ^{ ok = [self setupWithBufferSize:bufferSize sampleRate:sampleRate]; });
        return ok;
    }

    if (self.isReady) {
        NSLog(@"[BeeperAudioEngine] Already initialized, skipping");
        return YES;
    }

    self.requestedBufferSize = (bufferSize > 0) ? std::min(bufferSize, 4096) : kDefaultBufferSize;
    self.requestedSampleRate = (sampleRate > 0.0) ? sampleRate : kDefaultSampleRate;
    self.setupStep = -1;
    self.lastError = nil;

    @try {
        [self doSetup];
        if (self.isReady) [self registerSessionObservers];
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

    // Preferred hardware format. JUCE's iOS device re-requests the buffer duration itself when it
    // opens (see setAudioDeviceSetup below); these preferences cover the first open.
    const int preferredBuffer = self.requestedBufferSize;
    const double preferredRate = self.requestedSampleRate;
    NSError *rateError = nil;
    [session setPreferredSampleRate:preferredRate error:&rateError];
    if (rateError) {
        [self noteNonFatal:[NSString stringWithFormat:@"step 1 (non-fatal): setPreferredSampleRate(%.0f): %@",
                            preferredRate, rateError.localizedDescription]];
    }
    NSError *durationError = nil;
    [session setPreferredIOBufferDuration:(NSTimeInterval)preferredBuffer / preferredRate error:&durationError];
    if (durationError) {
        [self noteNonFatal:[NSString stringWithFormat:@"step 1 (non-fatal): setPreferredIOBufferDuration(%d/%.0f): %@",
                            preferredBuffer, preferredRate, durationError.localizedDescription]];
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
    } else {
        [self applyPreferredDeviceSetupWithBufferSize:preferredBuffer sampleRate:preferredRate];
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

// Asks the open device for the preferred sample rate / buffer size through JUCE, so JUCE's own
// device restarts (route changes) keep it. AudioDeviceManager::chooseBestBufferSize only accepts a
// size listed in getAvailableBufferSizes() (otherwise it falls back to the device default, 256 on
// iOS), so the nearest listed size is requested. Non-fatal: on error the device is restored.
- (void)applyPreferredDeviceSetupWithBufferSize:(int)bufferSize sampleRate:(double)sampleRate {
    if (sDeviceManager == nullptr) return;
    juce::AudioIODevice* device = sDeviceManager->getCurrentAudioDevice();
    if (device == nullptr) return;

    const juce::Array<int> sizes = device->getAvailableBufferSizes();
    int chosen = bufferSize;
    if (sizes.size() > 0 && !sizes.contains(bufferSize)) {
        int best = sizes[0];
        for (int i = 1; i < sizes.size(); ++i) {
            const int candidate = sizes[i];
            const int candidateDistance = candidate > bufferSize ? candidate - bufferSize : bufferSize - candidate;
            const int bestDistance = best > bufferSize ? best - bufferSize : bufferSize - best;
            if (candidateDistance < bestDistance || (candidateDistance == bestDistance && candidate > best))
                best = candidate;
        }
        chosen = best;
    }

    const juce::AudioDeviceManager::AudioDeviceSetup original = sDeviceManager->getAudioDeviceSetup();
    juce::AudioDeviceManager::AudioDeviceSetup wanted = original;
    wanted.sampleRate = sampleRate;
    wanted.bufferSize = chosen;
    NSLog(@"[BeeperAudioEngine] Step 4: setAudioDeviceSetup(sr=%.0f, buf=%d; requested %d)", sampleRate, chosen, bufferSize);
    const juce::String error = sDeviceManager->setAudioDeviceSetup(wanted, true);
    if (error.isEmpty()) return;

    NSString *message = [NSString stringWithUTF8String:error.toRawUTF8()];
    [self noteNonFatal:[NSString stringWithFormat:@"step 4 (non-fatal): setAudioDeviceSetup(%.0f Hz, %d): %@",
                        sampleRate, chosen, message ?: @"(unprintable error)"]];
    juce::AudioIODevice* current = sDeviceManager->getCurrentAudioDevice();
    if (current == nullptr || !current->isPlaying()) {
        const juce::String restoreError = sDeviceManager->setAudioDeviceSetup(original, true);
        if (restoreError.isNotEmpty()) {
            NSString *restoreMessage = [NSString stringWithUTF8String:restoreError.toRawUTF8()];
            [self noteNonFatal:[NSString stringWithFormat:@"step 4 (non-fatal): restoring device setup: %@",
                                restoreMessage ?: @"(unprintable error)"]];
        }
    }
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
    [self unregisterSessionObservers];
    if (!self.isReady) return;

    NSLog(@"[BeeperAudioEngine] Shutting down...");
    [self stopBeatTimer];
    if (sSynth) sSynth->allNotesOff();
    [self cleanup];
    NSLog(@"[BeeperAudioEngine] Shutdown complete");
}

// ── Session safeguard + recovery ──
// JUCE's iOS device (juce_Audio_ios.cpp) also observes these notifications: on interruption
// began/ended it stops/starts its audio unit (handleStatusChange), on media-services reset it only
// restarts the old (now dead) unit, and on CategoryChange it does nothing. What is added here:
// re-assert our Playback category if something else changed it, reactivate the session and
// reopen the device if it is still stopped after an interruption, and rebuild the whole engine
// after a media-services reset. Notifications can arrive on any thread; all work runs on main.

- (void)registerSessionObservers {
    if (self.observingSession) return;
    NSNotificationCenter *center = [NSNotificationCenter defaultCenter];
    [center addObserver:self
               selector:@selector(handleSessionRouteChange:)
                   name:AVAudioSessionRouteChangeNotification
                 object:nil];
    [center addObserver:self
               selector:@selector(handleSessionInterruption:)
                   name:AVAudioSessionInterruptionNotification
                 object:nil];
    [center addObserver:self
               selector:@selector(handleSessionMediaServicesReset:)
                   name:AVAudioSessionMediaServicesWereResetNotification
                 object:nil];
    self.observingSession = YES;
}

- (void)unregisterSessionObservers {
    if (!self.observingSession) return;
    NSNotificationCenter *center = [NSNotificationCenter defaultCenter];
    [center removeObserver:self name:AVAudioSessionRouteChangeNotification object:nil];
    [center removeObserver:self name:AVAudioSessionInterruptionNotification object:nil];
    [center removeObserver:self name:AVAudioSessionMediaServicesWereResetNotification object:nil];
    self.observingSession = NO;
}

- (void)noteRecoveryError:(NSString *)message {
    NSLog(@"[BeeperAudioEngine] recovery: %@", message);
    self.lastRecoveryError = message;
}

- (void)handleSessionRouteChange:(NSNotification *)notification {
    NSNumber *reasonValue = notification.userInfo[AVAudioSessionRouteChangeReasonKey];
    if (reasonValue == nil) return;
    const AVAudioSessionRouteChangeReason reason = (AVAudioSessionRouteChangeReason)reasonValue.unsignedIntegerValue;
    if (reason != AVAudioSessionRouteChangeReasonCategoryChange) return;
    __weak BeeperAudioEngine *weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        [weakSelf reassertSessionCategoryIfNeeded];
    });
}

// Main thread. Another component (e.g. a recording or video library) switched the shared session
// to a category that cannot play our output; switch it back.
- (void)reassertSessionCategoryIfNeeded {
    if (!self.isReady || self.isReassertingSession) return;
    AVAudioSession *session = [AVAudioSession sharedInstance];
    NSString *category = session.category;
    if ([category isEqualToString:AVAudioSessionCategoryPlayback] ||
        [category isEqualToString:AVAudioSessionCategoryPlayAndRecord]) {
        return;
    }

    self.isReassertingSession = YES;
    self.sessionReassertCount = self.sessionReassertCount + 1;
    NSLog(@"[BeeperAudioEngine] session category changed to %@; re-asserting Playback", category);

    NSError *categoryError = nil;
    [session setCategory:AVAudioSessionCategoryPlayback
             withOptions:AVAudioSessionCategoryOptionMixWithOthers
                   error:&categoryError];
    if (categoryError) {
        [self noteRecoveryError:[NSString stringWithFormat:@"re-assert setCategory: %@",
                                 categoryError.localizedDescription]];
    }
    NSError *activationError = nil;
    [session setActive:YES error:&activationError];
    if (activationError) {
        [self noteRecoveryError:[NSString stringWithFormat:@"re-assert setActive: %@",
                                 activationError.localizedDescription]];
    }
    self.isReassertingSession = NO;

    [self restartDeviceIfStopped:@"category change"];
}

- (void)handleSessionInterruption:(NSNotification *)notification {
    NSNumber *typeValue = notification.userInfo[AVAudioSessionInterruptionTypeKey];
    if (typeValue == nil) return;
    const AVAudioSessionInterruptionType type = (AVAudioSessionInterruptionType)typeValue.unsignedIntegerValue;
    __weak BeeperAudioEngine *weakSelf = self;
    if (type == AVAudioSessionInterruptionTypeBegan) {
        dispatch_async(dispatch_get_main_queue(), ^{
            BeeperAudioEngine *strongSelf = weakSelf;
            if (strongSelf == nil) return;
            strongSelf.interruptionCount = strongSelf.interruptionCount + 1;
            NSLog(@"[BeeperAudioEngine] audio session interruption began");
        });
    } else if (type == AVAudioSessionInterruptionTypeEnded) {
        // Resume whether or not the system set AVAudioSessionInterruptionOptionShouldResume:
        // this is an instrument; it should be playable as soon as the interruption is over.
        // Delayed so JUCE's own handler (which restarts its audio unit) runs first.
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, kRecoveryDelayMs * (int64_t)NSEC_PER_MSEC),
                       dispatch_get_main_queue(), ^{
            [weakSelf recoverFromInterruption];
        });
    }
}

// Main thread.
- (void)recoverFromInterruption {
    if (!self.isReady || sDeviceManager == nullptr) return;
    NSLog(@"[BeeperAudioEngine] audio session interruption ended; reactivating");
    NSError *activationError = nil;
    [[AVAudioSession sharedInstance] setActive:YES error:&activationError];
    if (activationError) {
        [self noteRecoveryError:[NSString stringWithFormat:@"interruption ended setActive: %@",
                                 activationError.localizedDescription]];
    }
    [self restartDeviceIfStopped:@"interruption"];
}

- (BOOL)isDeviceRunning {
    if (sDeviceManager == nullptr) return NO;
    juce::AudioIODevice* device = sDeviceManager->getCurrentAudioDevice();
    return (device != nullptr && device->isPlaying()) ? YES : NO;
}

// Main thread. Reopens the JUCE device with its last setup if it is closed or stopped; if that
// does not get it running, rebuilds the whole engine.
- (void)restartDeviceIfStopped:(NSString *)context {
    if (!self.isReady || sDeviceManager == nullptr) return;
    if ([self isDeviceRunning]) return;

    NSLog(@"[BeeperAudioEngine] device not running after %@; reopening", context);
    if (sDeviceManager->getCurrentAudioDevice() != nullptr)
        sDeviceManager->closeAudioDevice();
    sDeviceManager->restartLastAudioDevice();
    if ([self isDeviceRunning]) return;

    [self noteRecoveryError:[NSString stringWithFormat:@"device did not restart after %@; rebuilding engine", context]];
    [self rebuildEngine:context];
}

- (void)handleSessionMediaServicesReset:(NSNotification *)notification {
    __weak BeeperAudioEngine *weakSelf = self;
    // Delayed so JUCE's own handler has finished with the old device before it is deleted.
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, kRecoveryDelayMs * (int64_t)NSEC_PER_MSEC),
                   dispatch_get_main_queue(), ^{
        BeeperAudioEngine *strongSelf = weakSelf;
        if (strongSelf == nil) return;
        strongSelf.mediaResetCount = strongSelf.mediaResetCount + 1;
        [strongSelf rebuildEngine:@"media services reset"];
    });
}

// Main thread. Full teardown + setup with the last options. Keeps the beat callback and timer.
- (void)rebuildEngine:(NSString *)context {
    if (self.isRecovering) return;
    if (!self.isReady) return;
    self.isRecovering = YES;
    NSLog(@"[BeeperAudioEngine] rebuilding engine (%@)", context);

    BeatCallback savedCallback = self.beatCallback;
    const BOOL timerWasRunning = (self.beatTimer != nil) ? YES : NO;
    const int bufferSize = self.requestedBufferSize;
    const double sampleRate = self.requestedSampleRate;

    [self shutdown];
    const BOOL ok = [self setupWithBufferSize:bufferSize sampleRate:sampleRate];
    if (ok) {
        self.beatCallback = savedCallback;
        if (timerWasRunning) [self startBeatTimer];
    } else {
        [self noteRecoveryError:[NSString stringWithFormat:@"rebuild after %@ failed: step %d: %@",
                                 context, self.setupStep, self.lastError ?: @"unknown error"]];
    }
    self.isRecovering = NO;
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
    d[@"lastTouchToRenderMs"] = @(synth ? synth->diag.lastTouchToRenderMs.load(relaxed) : -1.0);

    d[@"preferredBufferSize"] = @(self.requestedBufferSize);
    d[@"preferredSampleRate"] = @(self.requestedSampleRate);
    d[@"sessionReassertCount"] = @(self.sessionReassertCount);
    d[@"interruptionCount"] = @(self.interruptionCount);
    d[@"mediaResetCount"] = @(self.mediaResetCount);
    d[@"lastRecoveryError"] = self.lastRecoveryError ?: @"";

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

- (void)noteOn:(int)note velocity:(int)velocity touchTimestamp:(double)touchTimestamp {
    if (!self.isReady || !sSynth) return;
    // UITouch.timestamp is seconds on the mach_absolute_time clock; on Apple platforms
    // juce::Time::getHighResolutionTicks() is mach_absolute_time(), so convert with JUCE's rate.
    const juce::int64 touchTicks = (touchTimestamp > 0.0)
        ? juce::Time::secondsToHighResolutionTicks(touchTimestamp)
        : (juce::int64)0;
    sSynth->noteOn(note, velocity, touchTicks);
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
// -setBeatCallback: is the synthesized (copy) setter of the beatCallback property.
// Do not hand-write it as `self.beatCallback = callback` — that calls itself forever.

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
