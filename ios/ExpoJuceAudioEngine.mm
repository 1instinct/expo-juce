// JuceConfig.h first: it is only macros + C headers, valid before any Objective-C import, and it
// must precede Apple's headers for __ASSERT_MACROS_DEFINE_VERSIONS_WITHOUT_UNDERSCORES to apply.
#include "JuceConfig.h"
#import "ExpoJuceAudioEngine.h"
#import <AVFoundation/AVFoundation.h>
#import <UIKit/UIKit.h>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "PolySynthProcessor.h"
#include "FxChainProcessor.h"
#include "AuxProcessor.h"
#include "BackingTrack.h"
#include "AnalysisRing.h"
#include "TransportEngine.h"
#include "Arpeggiator.h"
#include "DrumMachine.h"
#include "SpectrumAnalyser.h"
#include <atomic>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <mutex>
#include <thread>

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
static const int64_t kDeadDeviceCheckMs = 150; // callbackCount must advance within this window
static const int64_t kRebuildRetryDelayMs = 1000;
static const int kMaxRebuildAttempts = 3;     // failed rebuild: first attempt + 2 retries, then "failed"

@interface ExpoJuceAudioEngine ()
@property (nonatomic, assign) bool isReady;
@property (nonatomic, readwrite) int setupStep;
@property (atomic, readwrite, copy) NSString *lastError;
@property (nonatomic, copy) BeatCallback beatCallback;
@property (nonatomic, strong) dispatch_source_t beatTimer;
// Options of the last setup call (reused by media-services-reset recovery).
@property (nonatomic, assign) int requestedBufferSize;
@property (nonatomic, assign) double requestedSampleRate;
// Session safeguard / recovery (main thread only). Counters are cumulative since launch.
@property (nonatomic, assign) BOOL observingSession;
@property (nonatomic, assign) BOOL isReassertingSession;
@property (atomic, assign) BOOL isRecovering;      // atomic: engineState is read off the main thread
@property (nonatomic, assign) int sessionReassertCount;
@property (nonatomic, assign) int interruptionCount;
@property (nonatomic, assign) int mediaResetCount;
@property (atomic, copy) NSString *lastRecoveryError; // atomic: diagnostics reads it off the main thread
// Failed-rebuild retry state (main thread writes; atomic flags are read by diagnostics).
@property (atomic, assign) BOOL rebuildRetryPending;   // a retry is scheduled
@property (atomic, assign) BOOL engineFailed;          // setup failed / rebuild retries exhausted
@property (nonatomic, assign) int rebuildAttempts;     // attempts in the current recovery
@property (nonatomic, assign) int recoveryGeneration;  // bumped to cancel scheduled retries
@property (nonatomic, copy) BeatCallback recoveryBeatCallback; // beat callback kept across retries
// Main thread: the beat timer was asked for (public startBeatTimer: listeners exist) and not
// stopped since (public stopBeatTimer: the last listener left). Internal stops (rebuild,
// shutdown) keep it; a successful rebuild restarts the timer only if it is still set.
@property (nonatomic, assign) BOOL beatTimerWanted;
// Synth settings + transport state taken before a rebuild's first teardown and kept across
// failed attempts; restored when the engine is built again (by a retry or an explicit setup).
@property (nonatomic, assign) BOOL hasRecoveryState;
@property (nonatomic, assign) SynthSettings recoverySynthSettings;
@property (nonatomic, assign) TransportState recoveryTransportState;
// Main thread: a deferred free of retired backing-track buffers is scheduled.
@property (nonatomic, assign) BOOL backingTrackCollectScheduled;
- (void)scheduleBackingTrackCollect;
// Main thread: the beat timer itself, without changing beatTimerWanted.
- (void)resumeBeatTimer;
- (void)cancelBeatTimer;
@end

// Raw pointers — no JUCE smart pointers at file scope.
static juce::AudioDeviceManager* sDeviceManager = nullptr;
static GraphPlayer* sPlayer = nullptr;
static juce::AudioProcessorGraph* sGraph = nullptr;
static PolySynthProcessor* sSynth = nullptr;
static FxChainProcessor* sFx = nullptr;   // owned by sGraph, like sSynth
static AuxProcessor* sAux = nullptr;      // owned by sGraph
static TransportEngine* sTransport = nullptr;

// ── Engine lifecycle lock ──
// Guards the pointers above (and isReady) between the lifecycle code, which runs only on the
// main thread and takes the lock blocking (setup / teardown / rebuild / device restart), and the
// users that dereference them (JS thread, and JucePadView on the main thread). Note input
// (noteOn / noteOff / allNotesOff) does not use it: it goes through sNoteQueue below.
// Rules that keep it deadlock-free:
//  - Lifecycle sections take it at exactly one level (setupWithBufferSize, shutdown, dead-device reopen,
//    rebuild, device restart); everything they call under it is a *Locked helper that never
//    locks. No ObjC method that takes the lock is called while it is held.
//  - Users hold it only for a few atomic stores/loads; they never wait on the main thread or
//    call a locking method while holding it.
//  - Users on the main thread lock blocking (the main thread cannot be inside a lifecycle
//    section at the same time, so the only other holder is a short user section). Users on
//    other threads use try_lock: if the lifecycle holds it, the call is dropped (safe default
//    returned) and counted in lockBusyDrops. A short user section on another thread is waited
//    out with a bounded retry so a parameter change is not dropped for a microsecond of contention.
// The audio thread never takes this lock.
static std::mutex sEngineLock;
static std::atomic<bool> sLifecycleBusy{false};       // true while a lifecycle section holds sEngineLock
static std::atomic<uint64_t> sLockBusyDrops{0};
static std::atomic<uint64_t> sDeadDeviceRestarts{0};

// ── Note input ──
// One queue for the life of the process (never deleted, unlike the synth), so producers need
// no lock: note-ons go into its ring, releases are marks the synth applies every block (see
// NoteEventQueue.h). A release can therefore not be dropped by lock contention, a full ring, or
// a device reopen (the synth applies it on the first block after the device restarts).
// Note-ons are accepted only while the engine is ready (sAcceptingNotes); setup discards
// note-ons left in the ring before the new synth is attached, so nothing stale plays.
static NoteEventQueue sNoteQueue;
static std::atomic<bool> sAcceptingNotes{false};

// ── Effect settings ──
// Also one for the life of the process: every FxChainProcessor the engine builds reads its
// settings from here (atomics, see FxChainProcessor.h). setEffectEnabled / setEffectParam write
// it without the engine lock, so a change is never dropped and is never lost when it is made
// before setup, during a rebuild, or while a failed rebuild waits for its retry: the next chain
// simply starts with it. For the same reason it is not part of the rebuild recovery state.
static FxParamStore sFxParams;

// ── Aux (metronome + backing track) and analysis ──
// Same pattern: metronome / backing-track options, the backing track itself (decoded source +
// the buffer the audio thread plays) and the output analysis ring live for the whole process,
// so they need no engine lock, work before setup and survive rebuilds. The backing track is
// decoded and resampled on sBackingTrackQueue (serial, never the audio or main thread); buffers
// it replaces are freed on the main thread once the audio thread has moved past them
// (BackingTrack::collect, scheduleBackingTrackCollect).
static AuxSettingsStore sAuxSettings; // atomics only
// Holds JUCE buffers: heap-allocated and never freed, like the other JUCE objects here (nothing
// JUCE is destroyed by static destruction, after JUCE has shut down).
static BackingTrack& sBackingTrack = *new BackingTrack();
static AnalysisRing sAnalysis;        // atomics only
// Spectrum of the analysis ring, computed in -analysis on the calling thread (one FFT per call).
// Holds JUCE buffers: heap-allocated and never freed, like sBackingTrack. sSpectrumBusy keeps two
// concurrent -analysis calls off the shared FFT scratch (the second one reuses the last bands).
static SpectrumAnalyser& sSpectrum = *new SpectrumAnalyser();
static std::atomic_flag sSpectrumBusy = ATOMIC_FLAG_INIT;
static float sLastBands[SpectrumAnalyser::kBands] = {};

// ── Arpeggiator options ──
// Same pattern as the aux settings: the process-lifetime store every setArpeggiator writes (no
// lock, no engine needed), attached to each synth the engine builds, whose audio thread snapshots
// it once per block. Nothing is copied into the synth, so a setArpeggiator during a rebuild (the
// window withSynth would drop) is seen by the next block like any other.
static ArpSettingsStore sArpOptions;

// ── Drum parameters ──
// Same pattern again: per-voice and kit parameters as relaxed atomics in a process-lifetime store
// (no lock, works before setup, survives rebuilds), attached to each synth's drum part, which reads
// it once per block. Drum hits themselves go through sNoteQueue like note-ons.
static DrumParamStore sDrumParams;

static dispatch_queue_t backingTrackQueue() {
    static dispatch_queue_t queue;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        queue = dispatch_queue_create("expo-juce.backing-track", DISPATCH_QUEUE_SERIAL);
    });
    return queue;
}

// RAII: holds sEngineLock for a lifecycle section (main thread, blocking) and flags it busy.
struct LifecycleLock {
    std::lock_guard<std::mutex> guard;
    LifecycleLock() : guard(sEngineLock) { sLifecycleBusy.store(true, std::memory_order_relaxed); }
    ~LifecycleLock() { sLifecycleBusy.store(false, std::memory_order_relaxed); }
    LifecycleLock(const LifecycleLock&) = delete;
    LifecycleLock& operator=(const LifecycleLock&) = delete;
};

// Acquires sEngineLock for a user call (see rules above). Returns false (and counts a drop) if
// the lock could not be taken; the caller must then return its safe default.
static bool acquireUserLock(std::unique_lock<std::mutex>& lock) {
    if ([NSThread isMainThread]) {
        // Lifecycle sections run only on the main thread, so a busy flag seen here means this is
        // a re-entrant call from inside one (e.g. a nested run loop): locking would self-deadlock.
        if (sLifecycleBusy.load(std::memory_order_relaxed)) {
            sLockBusyDrops.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        lock.lock();
        return true;
    }
    if (lock.try_lock()) return true;
    for (int attempt = 0; attempt < 64 && !sLifecycleBusy.load(std::memory_order_relaxed); ++attempt) {
        std::this_thread::yield();
        if (lock.try_lock()) return true;
    }
    sLockBusyDrops.fetch_add(1, std::memory_order_relaxed);
    return false;
}

// Runs `body` with sSynth under sEngineLock (no-op if dropped or not ready). `body` must not
// call any ExpoJuceAudioEngine method that takes the lock.
static void withSynth(ExpoJuceAudioEngine *engine, void (^body)(PolySynthProcessor *synth)) {
    std::unique_lock<std::mutex> lock(sEngineLock, std::defer_lock);
    if (!acquireUserLock(lock)) return;
    if (!engine.isReady || !sSynth) return;
    body(sSynth);
}

// Runs `body` with sTransport under sEngineLock (no-op if dropped or not ready). Same rule.
static void withTransport(ExpoJuceAudioEngine *engine, void (^body)(TransportEngine *transport)) {
    std::unique_lock<std::mutex> lock(sEngineLock, std::defer_lock);
    if (!acquireUserLock(lock)) return;
    if (!engine.isReady || !sTransport) return;
    body(sTransport);
}

@implementation ExpoJuceAudioEngine

+ (instancetype)sharedInstance {
    static ExpoJuceAudioEngine *instance = nil;
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
        // The aux node asks for the track at the device rate when it is prepared (setup, rebuild,
        // device restart with a new rate): decode the file again at that rate off that thread,
        // then free the old buffer.
        sBackingTrack.setRateListener([](double) {
            dispatch_async(backingTrackQueue(), ^{
                sBackingTrack.reload();
                [[ExpoJuceAudioEngine sharedInstance] scheduleBackingTrackCollect];
            });
        });
    }
    return self;
}

- (BOOL)setup {
    return [self setupWithBufferSize:kDefaultBufferSize sampleRate:kDefaultSampleRate];
}

- (BOOL)setupWithBufferSize:(int)bufferSize sampleRate:(double)sampleRate {
    if (![NSThread isMainThread]) {
        NSLog(@"[ExpoJuceAudioEngine] ERROR: setup() called off main thread, dispatching to main");
        __block BOOL ok = NO;
        dispatch_sync(dispatch_get_main_queue(), ^{ ok = [self setupWithBufferSize:bufferSize sampleRate:sampleRate]; });
        return ok;
    }

    if (self.isReady) {
        NSLog(@"[ExpoJuceAudioEngine] Already initialized, skipping");
        return YES;
    }

    self.requestedBufferSize = (bufferSize > 0) ? std::min(bufferSize, 4096) : kDefaultBufferSize;
    self.requestedSampleRate = (sampleRate > 0.0) ? sampleRate : kDefaultSampleRate;

    BOOL ok = NO;
    {
        LifecycleLock lifecycle;
        ok = [self setupEngineLocked];
        // Setup after a failed rebuild (retry pending or given up) continues that recovery.
        // Note-ons are accepted only once the restored settings are in place.
        if (ok) {
            [self restoreRecoveryStateLocked];
            sAcceptingNotes.store(true, std::memory_order_release);
        }
    }
    if (ok) {
        [self registerSessionObservers];
        // An explicit setup supersedes any pending rebuild retry.
        self.recoveryGeneration = self.recoveryGeneration + 1;
        self.rebuildRetryPending = NO;
        self.engineFailed = NO;
    } else if (!self.rebuildRetryPending) {
        self.engineFailed = YES;
    }
    return ok;
}

// Main thread, caller holds sEngineLock (LifecycleLock). Builds the engine from the requested
// options; on failure tears down what was built. Calls only non-locking helpers.
- (BOOL)setupEngineLocked {
    self.setupStep = -1;
    self.lastError = nil;

    @try {
        [self doSetup];
    } @catch (NSException *exception) {
        NSLog(@"[ExpoJuceAudioEngine] EXCEPTION in setup: %@ — %@", exception.name, exception.reason);
        [self noteFatal:[NSString stringWithFormat:@"exception %@: %@", exception.name, exception.reason]];
        [self teardownEngineLocked];
    }

    if (!self.isReady && self.lastError == nil)
        [self noteFatal:@"setup did not complete"];
    return self.isReady ? YES : NO;
}

// Records a problem that does not abort setup (behaviour unchanged), so that
// diagnostics can still surface it. Appends to lastError.
- (void)noteNonFatal:(NSString *)message {
    NSLog(@"[ExpoJuceAudioEngine] %@", message);
    self.lastError = self.lastError ? [NSString stringWithFormat:@"%@; %@", self.lastError, message] : message;
}

// Records the error that aborted setup; placed first, earlier non-fatal notes kept after it.
- (void)noteFatal:(NSString *)message {
    self.lastError = self.lastError ? [NSString stringWithFormat:@"%@; %@", message, self.lastError] : message;
}

// Caller holds sEngineLock (via setupEngineLocked).
- (void)doSetup {
    self.setupStep = 0;
    NSLog(@"[ExpoJuceAudioEngine] Step 0: initialiseJuce_GUI");
    juce::initialiseJuce_GUI();

    self.setupStep = 1;
    NSLog(@"[ExpoJuceAudioEngine] Step 1: AVAudioSession (Playback)");
    NSError *sessionError = nil;
    NSError *activationError = nil;
    AVAudioSession *session = [AVAudioSession sharedInstance];
    [session setCategory:AVAudioSessionCategoryPlayback
             withOptions:AVAudioSessionCategoryOptionMixWithOthers
                   error:&sessionError];
    if (sessionError) {
        NSLog(@"[ExpoJuceAudioEngine] AVAudioSession category error: %@", sessionError);
    }
    [session setActive:YES error:&activationError];
    if (activationError) {
        NSLog(@"[ExpoJuceAudioEngine] AVAudioSession activation error: %@", activationError);
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
    NSLog(@"[ExpoJuceAudioEngine] Step 2: TransportEngine");
    sTransport = new TransportEngine();

    self.setupStep = 3;
    NSLog(@"[ExpoJuceAudioEngine] Step 3: AudioDeviceManager");
    sDeviceManager = new juce::AudioDeviceManager();

    self.setupStep = 4;
    NSLog(@"[ExpoJuceAudioEngine] Step 4: initialise(0, 2)");
    juce::String error = sDeviceManager->initialise(0, 2, nullptr, true);
    if (error.isNotEmpty()) {
        NSLog(@"[ExpoJuceAudioEngine] AudioDeviceManager init failed: %s", error.toRawUTF8());
        NSString *message = [NSString stringWithUTF8String:error.toRawUTF8()];
        [self noteFatal:[NSString stringWithFormat:@"AudioDeviceManager::initialise failed: %@",
                         message ?: @"(unprintable error)"]];
        [self teardownEngineLocked];
        return;
    }
    if (sDeviceManager->getCurrentAudioDevice() != nullptr) {
        [self applyPreferredDeviceSetupWithBufferSize:preferredBuffer sampleRate:preferredRate];
    }
    juce::AudioIODevice* device = sDeviceManager->getCurrentAudioDevice();
    if (device == nullptr || !device->isPlaying()) {
        [self noteFatal:@"no running audio device after applying/restoring device setup"];
        [self teardownEngineLocked];
        return;
    }

    double sr = sDeviceManager->getAudioDeviceSetup().sampleRate;
    int bufSize = sDeviceManager->getAudioDeviceSetup().bufferSize;
    self.setupStep = 5;
    NSLog(@"[ExpoJuceAudioEngine] Step 5: AudioProcessorGraph (sr=%.0f, buf=%d)", sr, bufSize);

    sGraph = new juce::AudioProcessorGraph();
    sGraph->setPlayConfigDetails(0, 2, sr, bufSize);

    self.setupStep = 6;
    NSLog(@"[ExpoJuceAudioEngine] Step 6: PolySynthProcessor");
    auto synthProcessor = std::make_unique<PolySynthProcessor>();
    sSynth = synthProcessor.get();
    sSynth->setTransport(sTransport);
    // Nothing drains sNoteQueue until the player is attached (step 11), so this thread may act
    // as its consumer here: note-ons left from before (a failed or replaced engine) are dropped.
    sNoteQueue.discardPending();
    sSynth->setNoteQueue(&sNoteQueue);
    sSynth->setAnalysis(&sAnalysis);
    sSynth->arp().setSettingsStore(&sArpOptions); // the live options, before and across rebuilds
    sSynth->drums().setParamStore(&sDrumParams);  // same for the drum part

    self.setupStep = 7;
    NSLog(@"[ExpoJuceAudioEngine] Step 7: addNode (synth, fx chain)");
    auto synthNode = sGraph->addNode(std::move(synthProcessor));
    // Effects chain between the synth and the output. Its settings come from the process-lifetime
    // sFxParams, so whatever the app set before (or across a rebuild) is in effect from the
    // first block. Sources that must bypass the effects connect to the output node directly
    // (the graph sums every connection into an input channel).
    auto fxProcessor = std::make_unique<FxChainProcessor>();
    sFx = fxProcessor.get();
    sFx->setParamStore(&sFxParams);
    sFx->setTransport(sTransport);
    auto fxNode = sGraph->addNode(std::move(fxProcessor));
    // Aux source (metronome + backing track): straight into the output, not through the effects.
    auto auxProcessor = std::make_unique<AuxProcessor>();
    sAux = auxProcessor.get();
    sAux->setSettings(&sAuxSettings);
    sAux->setBackingTrack(&sBackingTrack);
    sAux->setTransport(sTransport);
    sAux->setAnalysis(&sAnalysis);
    auto auxNode = sGraph->addNode(std::move(auxProcessor));

    self.setupStep = 8;
    NSLog(@"[ExpoJuceAudioEngine] Step 8: addNode (output)");
    auto outputNode = sGraph->addNode(
        std::make_unique<juce::AudioProcessorGraph::AudioGraphIOProcessor>(
            juce::AudioProcessorGraph::AudioGraphIOProcessor::audioOutputNode));

    self.setupStep = 9;
    NSLog(@"[ExpoJuceAudioEngine] Step 9: connections");
    // synth → fx → output
    bool connectedL = sGraph->addConnection({{synthNode->nodeID, 0}, {fxNode->nodeID, 0}});
    bool connectedR = sGraph->addConnection({{synthNode->nodeID, 1}, {fxNode->nodeID, 1}});
    if (!connectedL || !connectedR) {
        [self noteNonFatal:[NSString stringWithFormat:@"step 9 (non-fatal): addConnection synth->fx failed (L=%d, R=%d)",
                            connectedL ? 1 : 0, connectedR ? 1 : 0]];
    }
    bool fxConnectedL = sGraph->addConnection({{fxNode->nodeID, 0}, {outputNode->nodeID, 0}});
    bool fxConnectedR = sGraph->addConnection({{fxNode->nodeID, 1}, {outputNode->nodeID, 1}});
    if (!fxConnectedL || !fxConnectedR) {
        [self noteNonFatal:[NSString stringWithFormat:@"step 9 (non-fatal): addConnection fx->output failed (L=%d, R=%d)",
                            fxConnectedL ? 1 : 0, fxConnectedR ? 1 : 0]];
    }
    // aux → output (summed with the fx output by the graph)
    bool auxConnectedL = sGraph->addConnection({{auxNode->nodeID, 0}, {outputNode->nodeID, 0}});
    bool auxConnectedR = sGraph->addConnection({{auxNode->nodeID, 1}, {outputNode->nodeID, 1}});
    if (!auxConnectedL || !auxConnectedR) {
        [self noteNonFatal:[NSString stringWithFormat:@"step 9 (non-fatal): addConnection aux->output failed (L=%d, R=%d)",
                            auxConnectedL ? 1 : 0, auxConnectedR ? 1 : 0]];
    }

    self.setupStep = 10;
    NSLog(@"[ExpoJuceAudioEngine] Step 10: prepareToPlay");
    sGraph->prepareToPlay(sr, bufSize);

    self.setupStep = 11;
    NSLog(@"[ExpoJuceAudioEngine] Step 11: GraphPlayer + addAudioCallback");
    sPlayer = new GraphPlayer();
    sPlayer->setProcessor(sGraph);
    sPlayer->setAnalysis(&sAnalysis);
    sDeviceManager->addAudioCallback(sPlayer);

    self.isReady = true;
    sAnalysis.setActive(true);
    NSLog(@"[ExpoJuceAudioEngine] DONE — audio stack initialized (sr=%.0f, buf=%d)", sr, bufSize);
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
    NSLog(@"[ExpoJuceAudioEngine] Step 4: setAudioDeviceSetup(sr=%.0f, buf=%d; requested %d)", sampleRate, chosen, bufferSize);
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

// Main thread, caller holds sEngineLock. Deletes everything doSetup built (any subset).
- (void)teardownEngineLocked {
    NSLog(@"[ExpoJuceAudioEngine] Tearing down engine...");
    sAcceptingNotes.store(false, std::memory_order_release);
    self.isReady = false;
    sAnalysis.setActive(false);
    if (sDeviceManager && sPlayer) sDeviceManager->removeAudioCallback(sPlayer);
    delete sPlayer; sPlayer = nullptr;
    sSynth = nullptr;
    sFx = nullptr;
    sAux = nullptr;
    delete sGraph; sGraph = nullptr; // ~AuxProcessor releases its backing-track reader slot
    [self scheduleBackingTrackCollect];
    if (sDeviceManager) { sDeviceManager->closeAudioDevice(); delete sDeviceManager; sDeviceManager = nullptr; }
    delete sTransport; sTransport = nullptr;
}

// Main thread, caller holds sEngineLock. Snapshot of what the app configured (synth parameters,
// tempo / position / play state), which a teardown would otherwise reset to the defaults.
// Effect settings are deliberately not captured: they live in sFxParams, which outlives the
// engine and is attached to the rebuilt chain (restoring a snapshot taken here would undo
// changes the app makes while a failed rebuild waits for its retry).
- (void)captureRecoveryStateLocked {
    if (!sSynth || !sTransport) return;
    self.recoverySynthSettings = sSynth->getSettings();
    self.recoveryTransportState = sTransport->getState();
    self.hasRecoveryState = YES;
}

// Main thread, caller holds sEngineLock, engine just built. Applied before the lock is
// released, so no caller ever sees the defaults. Held notes are not replayed.
- (void)restoreRecoveryStateLocked {
    if (!self.hasRecoveryState || !sSynth || !sTransport) return;
    // The snapshot's arp options may predate a setArpeggiator made while a failed rebuild waited
    // for its retry; sArpOptions (which applySettings would write) always holds the latest.
    SynthSettings settings = self.recoverySynthSettings;
    settings.arp = sArpOptions.get();
    sSynth->applySettings(settings);
    sTransport->restoreState(self.recoveryTransportState);
    self.hasRecoveryState = NO;
}

- (void)shutdown {
    if (![NSThread isMainThread]) return;
    [self unregisterSessionObservers];
    // Cancel any scheduled rebuild retry; an explicit shutdown leaves the engine uninitialized.
    self.recoveryGeneration = self.recoveryGeneration + 1;
    self.rebuildRetryPending = NO;
    self.engineFailed = NO;
    self.recoveryBeatCallback = nil;
    self.hasRecoveryState = NO;
    // Also before setup (or after a failed one): the timer may run without an engine.
    [self cancelBeatTimer];
    if (!self.isReady) return;

    NSLog(@"[ExpoJuceAudioEngine] Shutting down...");
    {
        LifecycleLock lifecycle;
        sNoteQueue.pushAllNotesOff();
        [self teardownEngineLocked];
    }
    NSLog(@"[ExpoJuceAudioEngine] Shutdown complete");
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
    [center addObserver:self
               selector:@selector(handleAppDidBecomeActive:)
                   name:UIApplicationDidBecomeActiveNotification
                 object:nil];
    self.observingSession = YES;
}

- (void)unregisterSessionObservers {
    if (!self.observingSession) return;
    NSNotificationCenter *center = [NSNotificationCenter defaultCenter];
    [center removeObserver:self name:AVAudioSessionRouteChangeNotification object:nil];
    [center removeObserver:self name:AVAudioSessionInterruptionNotification object:nil];
    [center removeObserver:self name:AVAudioSessionMediaServicesWereResetNotification object:nil];
    [center removeObserver:self name:UIApplicationDidBecomeActiveNotification object:nil];
    self.observingSession = NO;
}

- (void)noteRecoveryError:(NSString *)message {
    NSLog(@"[ExpoJuceAudioEngine] recovery: %@", message);
    self.lastRecoveryError = message;
}

- (void)handleSessionRouteChange:(NSNotification *)notification {
    NSNumber *reasonValue = notification.userInfo[AVAudioSessionRouteChangeReasonKey];
    if (reasonValue == nil) return;
    const AVAudioSessionRouteChangeReason reason = (AVAudioSessionRouteChangeReason)reasonValue.unsignedIntegerValue;
    if (reason != AVAudioSessionRouteChangeReasonCategoryChange) return;
    __weak ExpoJuceAudioEngine *weakSelf = self;
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
    NSLog(@"[ExpoJuceAudioEngine] session category changed to %@; re-asserting Playback", category);

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
    __weak ExpoJuceAudioEngine *weakSelf = self;
    if (type == AVAudioSessionInterruptionTypeBegan) {
        dispatch_async(dispatch_get_main_queue(), ^{
            ExpoJuceAudioEngine *strongSelf = weakSelf;
            if (strongSelf == nil) return;
            strongSelf.interruptionCount = strongSelf.interruptionCount + 1;
            NSLog(@"[ExpoJuceAudioEngine] audio session interruption began");
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
    NSLog(@"[ExpoJuceAudioEngine] audio session interruption ended; reactivating");
    NSError *activationError = nil;
    [[AVAudioSession sharedInstance] setActive:YES error:&activationError];
    if (activationError) {
        [self noteRecoveryError:[NSString stringWithFormat:@"interruption ended setActive: %@",
                                 activationError.localizedDescription]];
    }
    [self restartDeviceIfStopped:@"interruption"];
    [self scheduleDeadDeviceCheck:@"interruption"];
}

- (void)handleAppDidBecomeActive:(NSNotification *)notification {
    __weak ExpoJuceAudioEngine *weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        [weakSelf scheduleDeadDeviceCheck:@"app became active"];
    });
}

// ── Dead-device check ──
// After an interruption (or returning to the foreground) the device can report isPlaying while
// its render callback never runs again. Main thread: snapshot GraphPlayer::callbackCount, and if
// it has not advanced kDeadDeviceCheckMs later, close + reopen the device; if it is still not
// advancing after another kDeadDeviceCheckMs, rebuild the engine.

- (void)scheduleDeadDeviceCheck:(NSString *)context {
    if (!self.isReady || self.isRecovering || sPlayer == nullptr) return;
    GraphPlayer *player = sPlayer;
    const uint64_t before = player->callbackCount.load(std::memory_order_relaxed);
    __weak ExpoJuceAudioEngine *weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, kDeadDeviceCheckMs * (int64_t)NSEC_PER_MSEC),
                   dispatch_get_main_queue(), ^{
        [weakSelf checkDeviceProgress:player since:before context:context restarted:NO];
    });
}

// Main thread. `player` is only compared with sPlayer (never dereferenced unless it still is
// sPlayer), so a rebuild in between simply ends the check.
- (void)checkDeviceProgress:(GraphPlayer *)player
                      since:(uint64_t)before
                    context:(NSString *)context
                  restarted:(BOOL)restarted {
    if (!self.isReady || self.isRecovering || sDeviceManager == nullptr) return;
    if (sPlayer == nullptr || sPlayer != player) return;
    if (player->callbackCount.load(std::memory_order_relaxed) != before) return; // alive

    if (!restarted) {
        sDeadDeviceRestarts.fetch_add(1, std::memory_order_relaxed);
        NSLog(@"[ExpoJuceAudioEngine] audio callback stalled after %@; reopening device", context);
        {
            LifecycleLock lifecycle;
            if (sDeviceManager->getCurrentAudioDevice() != nullptr)
                sDeviceManager->closeAudioDevice();
            sDeviceManager->restartLastAudioDevice();
        }
        const uint64_t afterRestart = player->callbackCount.load(std::memory_order_relaxed);
        __weak ExpoJuceAudioEngine *weakSelf = self;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, kDeadDeviceCheckMs * (int64_t)NSEC_PER_MSEC),
                       dispatch_get_main_queue(), ^{
            [weakSelf checkDeviceProgress:player since:afterRestart context:context restarted:YES];
        });
        return;
    }

    [self noteRecoveryError:[NSString stringWithFormat:@"audio callback still stalled after reopening (%@); rebuilding engine",
                             context]];
    [self rebuildEngine:context];
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

    NSLog(@"[ExpoJuceAudioEngine] device not running after %@; reopening", context);
    {
        LifecycleLock lifecycle;
        if (sDeviceManager->getCurrentAudioDevice() != nullptr)
            sDeviceManager->closeAudioDevice();
        sDeviceManager->restartLastAudioDevice();
    }
    if ([self isDeviceRunning]) return;

    [self noteRecoveryError:[NSString stringWithFormat:@"device did not restart after %@; rebuilding engine", context]];
    [self rebuildEngine:context];
}

- (void)handleSessionMediaServicesReset:(NSNotification *)notification {
    __weak ExpoJuceAudioEngine *weakSelf = self;
    // Delayed so JUCE's own handler has finished with the old device before it is deleted.
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, kRecoveryDelayMs * (int64_t)NSEC_PER_MSEC),
                   dispatch_get_main_queue(), ^{
        ExpoJuceAudioEngine *strongSelf = weakSelf;
        if (strongSelf == nil) return;
        strongSelf.mediaResetCount = strongSelf.mediaResetCount + 1;
        [strongSelf rebuildEngine:@"media services reset"];
    });
}

// Main thread. Full teardown + setup with the last options, under one hold of sEngineLock (JS
// callers are dropped meanwhile instead of seeing a half-built engine). Keeps the beat callback
// and timer, the session observers, the synth settings and the transport state (tempo,
// position, playing). On failure, retries every kRebuildRetryDelayMs up to kMaxRebuildAttempts
// attempts in total, then reports engineState "failed".
- (void)rebuildEngine:(NSString *)context {
    if (self.isRecovering || self.rebuildRetryPending) return;
    if (!self.isReady) return;
    self.recoveryBeatCallback = self.beatCallback;
    {
        LifecycleLock lifecycle;
        [self captureRecoveryStateLocked];
    }
    self.rebuildAttempts = 0;
    [self performRebuildAttempt:context];
}

// Main thread. One rebuild attempt (see rebuildEngine:). Never called with sEngineLock held.
- (void)performRebuildAttempt:(NSString *)context {
    self.isRecovering = YES;
    self.rebuildRetryPending = NO;
    self.rebuildAttempts = self.rebuildAttempts + 1;
    NSLog(@"[ExpoJuceAudioEngine] rebuilding engine (%@), attempt %d", context, self.rebuildAttempts);

    // Beat timer stop is main-thread work outside the lock (the callback is restored below).
    [self cancelBeatTimer];
    BOOL ok = NO;
    {
        LifecycleLock lifecycle;
        sNoteQueue.pushAllNotesOff();
        [self teardownEngineLocked];
        ok = [self setupEngineLocked];
        if (ok) {
            [self restoreRecoveryStateLocked];
            sAcceptingNotes.store(true, std::memory_order_release);
        }
    }

    if (ok) {
        [self registerSessionObservers]; // no-op if still registered
        self.beatCallback = self.recoveryBeatCallback;
        // Only if listeners still want it (the last one may have left during a retry wait).
        if (self.beatTimerWanted) [self resumeBeatTimer];
        self.recoveryBeatCallback = nil;
        self.engineFailed = NO;
        self.isRecovering = NO;
        return;
    }

    // Failed: the session/app observers stay registered (rebuild never unregisters them).
    [self registerSessionObservers];
    if (self.rebuildAttempts >= kMaxRebuildAttempts) {
        [self noteRecoveryError:[NSString stringWithFormat:@"rebuild after %@ failed (attempt %d of %d, giving up): step %d: %@",
                                 context, self.rebuildAttempts, kMaxRebuildAttempts,
                                 self.setupStep, self.lastError ?: @"unknown error"]];
        self.engineFailed = YES;
        self.isRecovering = NO;
        return;
    }

    [self noteRecoveryError:[NSString stringWithFormat:@"rebuild after %@ failed (attempt %d of %d, retrying in %lld ms): step %d: %@",
                             context, self.rebuildAttempts, kMaxRebuildAttempts, kRebuildRetryDelayMs,
                             self.setupStep, self.lastError ?: @"unknown error"]];
    self.rebuildRetryPending = YES;
    self.isRecovering = NO;
    const int generation = self.recoveryGeneration;
    __weak ExpoJuceAudioEngine *weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, kRebuildRetryDelayMs * (int64_t)NSEC_PER_MSEC),
                   dispatch_get_main_queue(), ^{
        ExpoJuceAudioEngine *strongSelf = weakSelf;
        if (strongSelf == nil) return;
        // Cancelled by shutdown or superseded by an explicit setup.
        if (!strongSelf.rebuildRetryPending || strongSelf.recoveryGeneration != generation) return;
        if (strongSelf.isReady) { // set up again by other means meanwhile
            strongSelf.rebuildRetryPending = NO;
            return;
        }
        [strongSelf performRebuildAttempt:context];
    });
}

// ── Diagnostics ──
// Called from the JS thread (sync Function). Engine pointers are read under sEngineLock
// (try_lock: while the lifecycle holds it, engine fields report their defaults and
// lockBusyDrops counts it). Audio-thread state is read via relaxed atomics; AVAudioSession is
// read on the calling thread, outside the lock.

- (NSDictionary<NSString *, id> *)diagnostics {
    NSMutableDictionary<NSString *, id> *d = [NSMutableDictionary dictionary];

    bool ready = false;
    NSString *deviceName = @"";
    NSString *deviceType = @"";
    double sampleRate = 0.0;
    int bufferSize = 0;
    int outputLatencySamples = 0;
    double callbackCount = 0.0, graphProcessCount = 0.0, outputPeak = 0.0, nanCount = 0.0, toneSamplesMixed = 0.0;
    double synthProcessCount = 0.0, synthPeak = 0.0, notesQueued = 0.0, notesConsumed = 0.0, notesDropped = 0.0;
    int activeVoices = 0;
    double lastNoteToRenderMs = -1.0, lastTouchToRenderMs = -1.0;
    double fxProcessCount = 0.0, fxLimiterGain = 1.0, fxNonFiniteResets = 0.0;
    double metronomeClicks = 0.0, backingTrackPositionSec = -1.0, backingTrackTempoRatio = 1.0;
    double drumPeak = 0.0;
    {
        std::unique_lock<std::mutex> lock(sEngineLock, std::defer_lock);
        if (acquireUserLock(lock)) {
            ready = self.isReady;
            if (ready && sDeviceManager) {
                if (juce::AudioIODevice* device = sDeviceManager->getCurrentAudioDevice()) {
                    deviceName = [NSString stringWithUTF8String:device->getName().toRawUTF8()] ?: @"";
                    deviceType = [NSString stringWithUTF8String:device->getTypeName().toRawUTF8()] ?: @"";
                    sampleRate = device->getCurrentSampleRate();
                    bufferSize = device->getCurrentBufferSizeSamples();
                    outputLatencySamples = device->getOutputLatencyInSamples();
                }
            }
            GraphPlayer* player = ready ? sPlayer : nullptr;
            PolySynthProcessor* synth = ready ? sSynth : nullptr;
            const auto relaxed = std::memory_order_relaxed;
            if (player) {
                callbackCount = (double)player->callbackCount.load(relaxed);
                graphProcessCount = (double)player->graphProcessCount.load(relaxed);
                outputPeak = (double)player->outputPeak.load(relaxed);
                nanCount = (double)player->nanCount.load(relaxed);
                toneSamplesMixed = (double)player->toneSamplesMixed.load(relaxed);
            }
            if (synth) {
                synthProcessCount = (double)synth->diag.processCount.load(relaxed);
                synthPeak = (double)synth->diag.peak.load(relaxed);
                activeVoices = synth->diag.activeVoices.load(relaxed);
                lastNoteToRenderMs = synth->diag.lastNoteToRenderMs.load(relaxed);
                lastTouchToRenderMs = synth->diag.lastTouchToRenderMs.load(relaxed);
                drumPeak = (double)synth->drums().diag.peak.load(relaxed);
            }
            if (FxChainProcessor* fx = ready ? sFx : nullptr) {
                fxProcessCount = (double)fx->diag.processCount.load(relaxed);
                fxLimiterGain = (double)fx->diag.limiterGain.load(relaxed);
                fxNonFiniteResets = (double)fx->diag.nonFiniteResets.load(relaxed);
            }
            if (AuxProcessor* aux = ready ? sAux : nullptr) {
                metronomeClicks = (double)aux->diag.clicks.load(relaxed);
                backingTrackPositionSec = aux->diag.trackPositionSec.load(relaxed);
                backingTrackTempoRatio = aux->diag.trackTempoRatio.load(relaxed);
            }
        }
    }
    // Note counters: cumulative since launch, read from the process-lifetime queue (no lock).
    notesQueued = (double)sNoteQueue.notesQueued.load(std::memory_order_relaxed);
    notesConsumed = (double)sNoteQueue.notesConsumed.load(std::memory_order_relaxed);
    notesDropped = (double)sNoteQueue.notesDropped.load(std::memory_order_relaxed);

    d[@"isReady"] = @(ready);
    d[@"setupStep"] = @(self.setupStep);
    d[@"lastError"] = self.lastError ?: @"";

    d[@"deviceName"] = deviceName;
    d[@"deviceType"] = deviceType;
    d[@"sampleRate"] = @(sampleRate);
    d[@"bufferSize"] = @(bufferSize);
    d[@"outputLatencySamples"] = @(outputLatencySamples);

    d[@"callbackCount"] = @(callbackCount);
    d[@"graphProcessCount"] = @(graphProcessCount);
    d[@"outputPeak"] = @(outputPeak);
    d[@"nanCount"] = @(nanCount);
    d[@"toneSamplesMixed"] = @(toneSamplesMixed);

    d[@"synthProcessCount"] = @(synthProcessCount);
    d[@"synthPeak"] = @(synthPeak);
    d[@"notesQueued"] = @(notesQueued);
    d[@"notesConsumed"] = @(notesConsumed);
    d[@"notesDropped"] = @(notesDropped);
    d[@"activeVoices"] = @(activeVoices);
    d[@"lastNoteToRenderMs"] = @(lastNoteToRenderMs);
    d[@"lastTouchToRenderMs"] = @(lastTouchToRenderMs);

    d[@"fxProcessCount"] = @(fxProcessCount);
    d[@"fxLimiterGain"] = @(fxLimiterGain);
    d[@"fxNonFiniteResets"] = @(fxNonFiniteResets);
    d[@"metronomeClicks"] = @(metronomeClicks);
    d[@"backingTrackLoaded"] = @(sBackingTrack.hasTrack() ? YES : NO);
    d[@"backingTrackPositionSec"] = @(backingTrackPositionSec);
    d[@"backingTrackTempoRatio"] = @(backingTrackTempoRatio);
    // Drum hits the engine has started (cumulative since launch, from the process-lifetime queue).
    d[@"drumHits"] = @((double)sNoteQueue.drumHitsConsumed.load(std::memory_order_relaxed));
    d[@"drumPeak"] = @(drumPeak);

    // engineState: recovering (rebuild running or retry scheduled) > ready > failed > uninitialized.
    // `ready` is false when the lock was busy, which only happens during a lifecycle section.
    NSString *engineState = @"uninitialized";
    if (self.isRecovering || self.rebuildRetryPending || sLifecycleBusy.load(std::memory_order_relaxed))
        engineState = @"recovering";
    else if (ready)
        engineState = @"ready";
    else if (self.engineFailed)
        engineState = @"failed";
    d[@"engineState"] = engineState;
    d[@"lockBusyDrops"] = @((double)sLockBusyDrops.load(std::memory_order_relaxed));
    d[@"deadDeviceRestarts"] = @((double)sDeadDeviceRestarts.load(std::memory_order_relaxed));

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
    if (!(durationMs > 0.0)) return NO;
    std::unique_lock<std::mutex> lock(sEngineLock, std::defer_lock);
    if (!acquireUserLock(lock)) return NO;
    if (!self.isReady || !sPlayer || !sDeviceManager) return NO;
    juce::AudioIODevice* device = sDeviceManager->getCurrentAudioDevice();
    if (device == nullptr || !device->isPlaying()) return NO;
    sPlayer->requestTestTone(durationMs);
    return YES;
}

// ── Synth forwarding ──
// Notes go through sNoteQueue without the lock (see "Note input"). Every other call holds
// sEngineLock (acquireUserLock) while it dereferences sSynth / sTransport; if the lifecycle
// holds it, the call is dropped and the safe default returned.

- (void)noteOn:(int)note velocity:(int)velocity {
    if (!sAcceptingNotes.load(std::memory_order_acquire)) return;
    sNoteQueue.pushNoteOn(note, velocity);
}

- (void)noteOn:(int)note velocity:(int)velocity touchTimestamp:(double)touchTimestamp {
    // UITouch.timestamp is seconds on the mach_absolute_time clock; on Apple platforms
    // juce::Time::getHighResolutionTicks() is mach_absolute_time(), so convert with JUCE's rate.
    const juce::int64 touchTicks = (touchTimestamp > 0.0)
        ? juce::Time::secondsToHighResolutionTicks(touchTimestamp)
        : (juce::int64)0;
    if (!sAcceptingNotes.load(std::memory_order_acquire)) return;
    sNoteQueue.pushNoteOn(note, velocity, touchTicks);
}

// Releases are always recorded, ready or not: a release must reach a voice that is still
// sounding whatever the engine is doing (a mark with nothing to release is harmless).
- (void)noteOff:(int)note { sNoteQueue.pushNoteOff(note); }
- (void)allNotesOff { sNoteQueue.pushAllNotesOff(); }

- (void)setWaveform:(NSString *)waveform {
    Waveform wf = Waveform::Sine;
    if ([waveform isEqualToString:@"square"]) wf = Waveform::Square;
    else if ([waveform isEqualToString:@"saw"]) wf = Waveform::Saw;
    else if ([waveform isEqualToString:@"triangle"]) wf = Waveform::Triangle;
    std::unique_lock<std::mutex> lock(sEngineLock, std::defer_lock);
    if (!acquireUserLock(lock)) return;
    if (!self.isReady || !sSynth) return;
    sSynth->setWaveform(wf);
}

- (void)setFilterCutoff:(double)hz { withSynth(self, ^(PolySynthProcessor *synth) { synth->setFilterCutoff(hz); }); }
- (void)setFilterResonance:(double)q { withSynth(self, ^(PolySynthProcessor *synth) { synth->setFilterResonance(q); }); }
- (void)setAttack:(double)ms { withSynth(self, ^(PolySynthProcessor *synth) { synth->setAttack(ms); }); }
- (void)setDecay:(double)ms { withSynth(self, ^(PolySynthProcessor *synth) { synth->setDecay(ms); }); }
- (void)setSustain:(double)level { withSynth(self, ^(PolySynthProcessor *synth) { synth->setSustain(level); }); }
- (void)setRelease:(double)ms { withSynth(self, ^(PolySynthProcessor *synth) { synth->setRelease(ms); }); }
- (void)setDetune:(double)cents { withSynth(self, ^(PolySynthProcessor *synth) { synth->setDetune(cents); }); }
- (void)setMasterLevel:(double)level { withSynth(self, ^(PolySynthProcessor *synth) { synth->setMasterLevel(level); }); }

// ── Effects ──
// No engine lock: the settings are atomics in the process-lifetime sFxParams (see "Effect
// settings"), read by the running chain every chunk and by any chain built later.

// Float-stored values reported to JS rounded to 6 decimals (0.4f → 0.4, not 0.4000000059604645).
static double fxValueForJS(float value) {
    return std::round((double)value * 1e6) / 1e6;
}

- (BOOL)setEffectEnabled:(NSString *)effectId enabled:(BOOL)enabled {
    return sFxParams.setEnabled(effectId.UTF8String, enabled ? true : false) ? YES : NO;
}

- (BOOL)setEffectParam:(NSString *)effectId param:(NSString *)param value:(double)value {
    return sFxParams.setParam(effectId.UTF8String, param.UTF8String, value) ? YES : NO;
}

- (NSDictionary<NSString *, id> *)effectsState {
    const FxSettings settings = sFxParams.snapshot();
    NSMutableDictionary<NSString *, id> *state = [NSMutableDictionary dictionaryWithCapacity:kFxCount];
    for (int fx = 0; fx < kFxCount; ++fx) {
        const FxSpec& spec = kFxSpecs[fx];
        NSMutableDictionary<NSString *, NSNumber *> *params = [NSMutableDictionary dictionaryWithCapacity:spec.numParams];
        for (int p = 0; p < spec.numParams; ++p)
            params[@(spec.params[p].name)] = @(fxValueForJS(settings.values[fx][p]));
        state[@(spec.id)] = @{ @"enabled": @(settings.enabled[fx] ? YES : NO), @"params": params };
    }
    return state;
}

// ── Metronome / backing track ──
// Options: atomics in the process-lifetime sAuxSettings (no lock, remembered before setup and
// across rebuilds). Keys that are absent or of the wrong type are left unchanged.

static NSNumber *numberFrom(NSDictionary<NSString *, id> *options, NSString *key) {
    id value = options[key];
    if (![value isKindOfClass:[NSNumber class]]) return nil;
    return std::isfinite([(NSNumber *)value doubleValue]) ? (NSNumber *)value : nil;
}

- (BOOL)setMetronome:(NSDictionary<NSString *, id> *)options {
    // An unknown style rejects the whole call (nothing applied), like an unknown effect id.
    id style = options[@"style"];
    int styleIndex = -1;
    if (style != nil) {
        if (![style isKindOfClass:[NSString class]]) return NO;
        styleIndex = metronomeStyleIndex(((NSString *)style).UTF8String);
        if (styleIndex < 0) return NO;
    }
    if (NSNumber *v = numberFrom(options, @"enabled")) sAuxSettings.setMetronomeEnabled(v.boolValue);
    if (styleIndex >= 0) sAuxSettings.setMetronomeStyle(styleIndex);
    if (NSNumber *v = numberFrom(options, @"level")) sAuxSettings.setMetronomeLevel(v.doubleValue);
    if (NSNumber *v = numberFrom(options, @"accent")) sAuxSettings.setMetronomeAccent(v.boolValue);
    if (NSNumber *v = numberFrom(options, @"beatsPerBar")) sAuxSettings.setBeatsPerBar(v.doubleValue);
    return YES;
}

- (void)setBackingTrackOptions:(NSDictionary<NSString *, id> *)options {
    if (NSNumber *v = numberFrom(options, @"level")) sAuxSettings.setTrackLevel(v.doubleValue);
    if (NSNumber *v = numberFrom(options, @"loop")) sAuxSettings.setTrackLoop(v.boolValue);
    if (NSNumber *v = numberFrom(options, @"enabled")) sAuxSettings.setTrackEnabled(v.boolValue);
}

// file:// URL or absolute path → path; nil for anything else (remote URLs are not fetched).
static NSString *backingTrackPath(NSString *uri) {
    if ([uri hasPrefix:@"/"]) return uri;
    if ([uri hasPrefix:@"file://"]) {
        NSURL *url = [NSURL URLWithString:uri];
        if (url != nil && url.isFileURL && url.path.length > 0) return url.path;
        NSString *rest = [[uri substringFromIndex:7] stringByRemovingPercentEncoding];
        if ([rest hasPrefix:@"localhost/"]) rest = [rest substringFromIndex:9];
        return [rest hasPrefix:@"/"] ? rest : nil;
    }
    return nil;
}

- (void)loadBackingTrack:(NSString *)uri
                 gapless:(NSDictionary<NSString *, id> *)gapless
              completion:(void (^)(NSDictionary<NSString *, id> *result, NSString *errorCode, NSString *errorMessage))completion {
    NSString *path = uri != nil ? backingTrackPath(uri) : nil;
    if (path == nil) {
        completion(nil, @"ERR_BACKING_TRACK_URI", [NSString stringWithFormat:@"not a local file URI: %@", uri ?: @"(nil)"]);
        return;
    }
    BackingTrackSource source;
    source.path = path.UTF8String;
    if (gapless != nil) {
        NSNumber *loop = numberFrom(gapless, @"loopSamples");
        NSNumber *leading = numberFrom(gapless, @"leadingSamples");
        NSNumber *trailing = numberFrom(gapless, @"trailingSamples");
        if (loop != nil && leading != nil && trailing != nil) {
            source.gapless.loopSamples = loop.longLongValue;
            source.gapless.leadingSamples = leading.longLongValue;
            source.gapless.trailingSamples = trailing.longLongValue;
            source.hasGapless = source.gapless.loopSamples > 0;
        }
        // The file's own tempo rides in the same dictionary (older binaries ignore the key).
        if (NSNumber *bpm = numberFrom(gapless, @"sourceBpm")) source.sourceBpm = bpm.doubleValue;
    }
    // A newer load or an unload supersedes this one (checked while decoding and before publishing).
    const uint64_t generation = sBackingTrack.beginLoad();
    dispatch_async(backingTrackQueue(), ^{
        std::string error;
        BackingTrackInfo info;
        // Decoded straight at the device rate (known once the engine has been prepared).
        auto buffer = BackingTrack::load(source, sBackingTrack.wantedSampleRate(),
                                         [generation] { return !sBackingTrack.isCurrent(generation); }, info, error);
        if (buffer == nullptr) {
            NSString *full = [NSString stringWithUTF8String:error.c_str()] ?: @"ERR_BACKING_TRACK: decode failed";
            NSRange colon = [full rangeOfString:@": "];
            NSString *code = colon.location != NSNotFound ? [full substringToIndex:colon.location] : @"ERR_BACKING_TRACK";
            NSString *message = colon.location != NSNotFound ? [full substringFromIndex:NSMaxRange(colon)] : full;
            completion(nil, code, message);
            return;
        }
        if (!sBackingTrack.install(std::move(buffer), source, generation)) {
            completion(nil, @"ERR_BACKING_TRACK_SUPERSEDED", @"a newer load or unload replaced this one");
            return;
        }
        [self scheduleBackingTrackCollect];
        completion(@{
            @"durationSec": @(info.durationSec),
            @"sampleRate": @(info.sampleRate),
            @"channels": @(info.channels),
            @"loopStartSample": @((double)info.loopStartSample),
            @"loopSamples": @((double)info.loopSamples),
            @"decodedSamples": @((double)info.decodedSamples),
            @"regionRule": [NSString stringWithUTF8String:backingTrackRegionRuleName(info.regionRule)],
            @"decoder": [NSString stringWithUTF8String:info.formatName.c_str()] ?: @"",
            @"sourceBpm": @(info.sourceBpm),
        }, nil, nil);
    });
}

- (void)unloadBackingTrack {
    sBackingTrack.unload();
    [self scheduleBackingTrackCollect];
}

// Main thread. Frees replaced backing-track buffers the audio thread no longer uses; retries
// every 100 ms while some are still in use (e.g. the device is stopped mid-swap) or the holder
// is busy (collect never blocks the main thread).
- (void)scheduleBackingTrackCollect {
    if (![NSThread isMainThread]) {
        dispatch_async(dispatch_get_main_queue(), ^{ [self scheduleBackingTrackCollect]; });
        return;
    }
    if (self.backingTrackCollectScheduled) return;
    if (sBackingTrack.collect() == 0) return;
    self.backingTrackCollectScheduled = YES;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 100 * (int64_t)NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
        self.backingTrackCollectScheduled = NO;
        [self scheduleBackingTrackCollect];
    });
}

// ── Arpeggiator ──
// Options: sArpOptions, the process-lifetime store the running synth reads every block (no lock,
// works before setup). Absent keys and keys of the wrong number type are left unchanged; an
// unknown `mode` / `rate`, a non-string / non-array where one is expected, or a `pattern` that is
// not 1–16 booleans rejects the whole call (NO, nothing applied).

- (BOOL)setArpeggiator:(NSDictionary<NSString *, id> *)options {
    ArpSettings s = sArpOptions.get();
    id mode = options[@"mode"];
    if (mode != nil) {
        if (![mode isKindOfClass:[NSString class]]) return NO;
        const int index = arpModeIndex(((NSString *)mode).UTF8String);
        if (index < 0) return NO;
        s.mode = (ArpMode)index;
    }
    id rate = options[@"rate"];
    if (rate != nil) {
        if (![rate isKindOfClass:[NSString class]]) return NO;
        const int index = arpRateIndex(((NSString *)rate).UTF8String);
        if (index < 0) return NO;
        s.rate = (ArpRate)index;
    }
    id pattern = options[@"pattern"];
    if (pattern != nil) {
        if (![pattern isKindOfClass:[NSArray class]]) return NO;
        NSArray *steps = (NSArray *)pattern;
        if (steps.count < 1 || steps.count > 16) return NO;
        const int length = (int)steps.count;
        uint16_t mask = 0;
        for (int i = 0; i < length; ++i) {
            id step = steps[(NSUInteger)i];
            if (![step isKindOfClass:[NSNumber class]]) return NO;
            if ([(NSNumber *)step boolValue]) mask |= (uint16_t)(1u << i);
        }
        s.patternMask = mask;
        s.patternLength = length;
    }
    if (NSNumber *v = numberFrom(options, @"enabled")) s.enabled = v.boolValue;
    if (NSNumber *v = numberFrom(options, @"latch")) s.latch = v.boolValue;
    if (NSNumber *v = numberFrom(options, @"octaves")) s.octaves = (int)std::lround(v.doubleValue);
    if (NSNumber *v = numberFrom(options, @"gate")) s.gate = (float)v.doubleValue;
    if (NSNumber *v = numberFrom(options, @"swing")) s.swing = (float)v.doubleValue;
    if (NSNumber *v = numberFrom(options, @"accent")) s.accent = (float)v.doubleValue;
    if (NSNumber *v = numberFrom(options, @"ratchet")) s.ratchet = (int)std::lround(v.doubleValue);
    if (NSNumber *v = numberFrom(options, @"transposeSteps")) s.transposeSteps = (int)std::lround(v.doubleValue);
    if (NSNumber *v = numberFrom(options, @"transposeDistance")) s.transposeDistance = (int)std::lround(v.doubleValue);
    sArpOptions.set(s); // clamps; the synth's next block reads it
    return YES;
}

- (NSDictionary<NSString *, id> *)arpeggiatorState {
    const ArpSettings s = sArpOptions.get();
    NSMutableArray<NSNumber *> *pattern = [NSMutableArray arrayWithCapacity:(NSUInteger)s.patternLength];
    for (int i = 0; i < s.patternLength; ++i) [pattern addObject:@(((s.patternMask >> i) & 1) ? YES : NO)];
    return @{
        @"enabled": @(s.enabled ? YES : NO),
        @"mode": @(kArpModeNames[std::clamp((int)s.mode, 0, kArpModeCount - 1)]),
        @"rate": @(kArpRateNames[std::clamp((int)s.rate, 0, kArpRateCount - 1)]),
        @"octaves": @(s.octaves),
        @"gate": @(fxValueForJS(s.gate)),
        @"swing": @(fxValueForJS(s.swing)),
        @"latch": @(s.latch ? YES : NO),
        @"pattern": pattern,
        @"accent": @(fxValueForJS(s.accent)),
        @"ratchet": @(s.ratchet),
        @"transposeSteps": @(s.transposeSteps),
        @"transposeDistance": @(s.transposeDistance),
    };
}

// ── Drums ──
// Hits: through sNoteQueue without the lock, like noteOn (accepted only while the engine is
// ready). Parameters: sDrumParams, the process-lifetime store (no lock, works before setup).
// Absent keys, keys of the wrong type and non-finite numbers are left unchanged.

- (void)drumHit:(int)voice velocity:(int)velocity {
    if (!sAcceptingNotes.load(std::memory_order_acquire)) return;
    sNoteQueue.pushDrumHit(voice, velocity);
}

- (void)drumHit:(int)voice velocity:(int)velocity touchTimestamp:(double)touchTimestamp {
    const juce::int64 touchTicks = (touchTimestamp > 0.0)
        ? juce::Time::secondsToHighResolutionTicks(touchTimestamp)
        : (juce::int64)0;
    if (!sAcceptingNotes.load(std::memory_order_acquire)) return;
    sNoteQueue.pushDrumHit(voice, velocity, touchTicks);
}

- (BOOL)setDrumVoice:(int)voice params:(NSDictionary<NSString *, id> *)params {
    if (voice < 0 || voice >= kDrumVoices) return NO;
    for (int p = 0; p < kDrumParamCount; ++p) {
        if (NSNumber *v = numberFrom(params, @(kDrumParamNames[p]))) sDrumParams.setVoiceParam(voice, p, v.doubleValue);
    }
    return YES;
}

- (BOOL)setDrumKit:(NSDictionary<NSString *, id> *)params {
    if (NSNumber *v = numberFrom(params, @"level")) sDrumParams.setKitLevel(v.doubleValue);
    if (NSNumber *v = numberFrom(params, @"drive")) sDrumParams.setKitDrive(v.doubleValue);
    if (NSNumber *v = numberFrom(params, @"mute")) sDrumParams.setKitMute(v.boolValue ? true : false);
    return YES;
}

- (NSDictionary<NSString *, id> *)drumState {
    NSMutableArray<NSDictionary<NSString *, NSNumber *> *> *voices = [NSMutableArray arrayWithCapacity:kDrumVoices];
    for (int v = 0; v < kDrumVoices; ++v) {
        const DrumVoiceParams p = sDrumParams.voice(v);
        [voices addObject:@{
            @"tune": @(fxValueForJS(p.tune)),
            @"decay": @(fxValueForJS(p.decay)),
            @"tone": @(fxValueForJS(p.tone)),
            @"level": @(fxValueForJS(p.level)),
        }];
    }
    const DrumKitParams kit = sDrumParams.kit();
    return @{
        @"kit": @{
            @"level": @(fxValueForJS(kit.level)),
            @"mute": @(kit.mute ? YES : NO),
            @"drive": @(fxValueForJS(kit.drive)),
        },
        @"voices": voices,
    };
}

// ── Analysis ──
// Any thread, no lock: everything is read from the process-lifetime sAnalysis (levels and scope
// written by GraphPlayer, transport as of the last block written by the aux node, arp state
// written by the synth). The spectrum is one 2048-point FFT of the ring, computed here (the
// ring is inactive before setup: no FFT, 24 zeros). Zeros while the engine is not running.
- (NSDictionary<NSString *, id> *)analysis {
    const bool active = sAnalysis.isActive();
    float scope[AnalysisRing::kScopePoints] = {};
    float bands[SpectrumAnalyser::kBands] = {};
    AnalysisRing::Levels levels{0.0f, 0.0f};
    double sampleRate = 0.0;
    if (active) {
        sAnalysis.readScope(scope);
        levels = sAnalysis.readLevels();
        sampleRate = sAnalysis.sampleRate();
        if (!sSpectrumBusy.test_and_set(std::memory_order_acquire)) {
            float raw[AnalysisRing::kSize];
            sAnalysis.readRaw(raw);
            sSpectrum.analyse(raw, sampleRate, bands);
            std::memcpy(sLastBands, bands, sizeof(bands));
            sSpectrumBusy.clear(std::memory_order_release);
        } else {
            std::memcpy(bands, sLastBands, sizeof(bands)); // another caller is mid-FFT
        }
    }
    NSMutableArray<NSNumber *> *points = [NSMutableArray arrayWithCapacity:AnalysisRing::kScopePoints];
    for (int i = 0; i < AnalysisRing::kScopePoints; ++i) [points addObject:@(scope[i])];
    NSMutableArray<NSNumber *> *bandValues = [NSMutableArray arrayWithCapacity:SpectrumAnalyser::kBands];
    for (int i = 0; i < SpectrumAnalyser::kBands; ++i) [bandValues addObject:@(bands[i])];
    return @{
        @"peak": @(levels.peak),
        @"rms": @(levels.rms),
        @"scope": points,
        @"bands": bandValues,
        @"sampleRate": @(sampleRate),
        @"lastNote": @(sNoteQueue.lastNoteStarted.load(std::memory_order_relaxed)),
        @"bpm": @(active ? sAnalysis.bpm() : 120.0),
        @"beat": @(active ? sAnalysis.beat() : 0.0),
        @"playing": @(active && sAnalysis.playing() ? YES : NO),
        @"arp": @{
            @"active": @(active && sAnalysis.arpActive() ? YES : NO),
            @"step": @(active ? sAnalysis.arpStep() : 0),
            @"steps": @(active ? sAnalysis.arpSteps() : sArpOptions.get().patternLength),
            @"note": @(active ? sAnalysis.arpNote() : -1),
        },
    };
}

// ── Transport ──

- (void)startTransport { withTransport(self, ^(TransportEngine *transport) { transport->start(); }); }
- (void)stopTransport { withTransport(self, ^(TransportEngine *transport) { transport->stop(); }); }
- (void)setTempo:(double)bpm { withTransport(self, ^(TransportEngine *transport) { transport->setTempo(bpm); }); }
- (void)setTransportPosition:(double)beat { withTransport(self, ^(TransportEngine *transport) { transport->setPosition(beat); }); }

- (double)getTransportPosition {
    __block double position = 0.0;
    withTransport(self, ^(TransportEngine *transport) { position = transport->getPosition(); });
    return position;
}

- (double)getTempo {
    __block double tempo = 120.0;
    withTransport(self, ^(TransportEngine *transport) { tempo = transport->getTempo(); });
    return tempo;
}

- (BOOL)isTransportPlaying {
    __block BOOL playing = NO;
    withTransport(self, ^(TransportEngine *transport) { playing = transport->isPlaying() ? YES : NO; });
    return playing;
}

// ── Beat events ──
// -setBeatCallback: is the synthesized (copy) setter of the beatCallback property.
// Do not hand-write it as `self.beatCallback = callback` — that calls itself forever.
// The beat timer (and beatCallback) are main-thread state: start/stop called from another
// thread (Expo's OnStartObserving / OnStopObserving / OnDestroy) re-dispatch to main.
// The timer may start before setup (an observer subscribed first): it idles until a synth exists.
// Stopping it keeps beatCallback, so a later start (the next observer) delivers events again.

- (void)startBeatTimer {
    if (![NSThread isMainThread]) {
        __weak ExpoJuceAudioEngine *weakSelf = self;
        dispatch_async(dispatch_get_main_queue(), ^{ [weakSelf startBeatTimer]; });
        return;
    }
    self.beatTimerWanted = YES;
    [self resumeBeatTimer];
}

- (void)stopBeatTimer {
    if (![NSThread isMainThread]) {
        __weak ExpoJuceAudioEngine *weakSelf = self;
        dispatch_async(dispatch_get_main_queue(), ^{ [weakSelf stopBeatTimer]; });
        return;
    }
    self.beatTimerWanted = NO;
    [self cancelBeatTimer];
}

// Main thread. Creates the timer if it is not running (beatTimerWanted unchanged).
- (void)resumeBeatTimer {
    if (self.beatTimer) return;

    self.beatTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
    dispatch_source_set_timer(self.beatTimer, DISPATCH_TIME_NOW, 16 * NSEC_PER_MSEC, 1 * NSEC_PER_MSEC);
    __weak ExpoJuceAudioEngine *weakSelf = self;
    dispatch_source_set_event_handler(self.beatTimer, ^{
        ExpoJuceAudioEngine *strongSelf = weakSelf;
        if (!strongSelf || !sSynth) return;
        BeatCallback cb = strongSelf.beatCallback;
        if (sSynth->beatState.crossed.exchange(false) && cb) {
            const double beat = sSynth->beatState.beat.load();
            // Fractional transport position now (main thread: sTransport cannot change under us).
            const double position = sTransport ? sTransport->getPosition() : beat;
            cb(beat, sSynth->beatState.bpm.load(), position);
        }
    });
    dispatch_resume(self.beatTimer);
}

// Main thread. Cancels the timer, keeping beatTimerWanted and the callback.
- (void)cancelBeatTimer {
    if (self.beatTimer) {
        dispatch_source_cancel(self.beatTimer);
        self.beatTimer = nil;
    }
}

- (void)dealloc { [self shutdown]; }

@end
