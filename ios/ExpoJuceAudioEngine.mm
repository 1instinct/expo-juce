// JuceConfig.h first: it is only macros + C headers, valid before any Objective-C import, and it
// must precede Apple's headers for __ASSERT_MACROS_DEFINE_VERSIONS_WITHOUT_UNDERSCORES to apply.
#include "JuceConfig.h"
#import "ExpoJuceAudioEngine.h"
#import <AVFoundation/AVFoundation.h>
#import <CoreMIDI/CoreMIDI.h>
#import <MediaPlayer/MediaPlayer.h>
#import <QuartzCore/QuartzCore.h>
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
#include "LatencyStats.h"
#include "DisplayRate.h"
#include "DeviceFormat.h"
#include "MidiInput.h"
#include "ExpoJuceMidiInput.h"
#include "OwnerToken.h"
#include "SequencePlayer.h"
#include "BackgroundPolicy.h"
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>
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

static const int kDefaultBufferSize = devicefmt::kDefaultBufferSize;
static const double kDefaultSampleRate = devicefmt::kDefaultSampleRate;
static const int kMaxDeviceOpenLog = 8;       // diagnostics: the last device opens the engine made
static const int kMaxTouchPathEntries = 30;   // diagnostics: touchPath cap
static const int64_t kRecoveryDelayMs = 300; // let JUCE's own session handlers run first
static const int64_t kDeadDeviceCheckMs = 150; // callbackCount must advance within this window
static const int64_t kRebuildRetryDelayMs = 1000;
static const int kMaxRebuildAttempts = 3;     // failed rebuild: first attempt + 2 retries, then "failed"

@class ExpoJuceMainDisplayLink;

@interface ExpoJuceAudioEngine ()
// Absorbs CoreMIDI's one-time XPC initialisation on a background queue so it
// cannot block the main thread later. See the definition for why.
+ (void)warmCoreMidiOffMainThread;
@property (nonatomic, assign) bool isReady;
@property (nonatomic, readwrite) int setupStep;
@property (atomic, readwrite, copy) NSString *lastError;
@property (nonatomic, copy) BeatCallback beatCallback;
@property (nonatomic, strong) dispatch_source_t beatTimer;
// Options of the last setup call (reused by media-services-reset recovery).
@property (nonatomic, assign) int requestedBufferSize;
@property (nonatomic, assign) double requestedSampleRate;
// The rate the engine asks the device for: the session's current rate when it is a music rate
// (devicefmt::chooseDeviceSampleRate), read at setup and again after a route change or an
// interruption. Atomic: diagnostics read it off the main thread.
@property (atomic, assign) double targetSampleRate;
// The session's rate read at setup before the device was created. Atomic.
@property (atomic, assign) double hardwareSampleRate;
// The listed buffer size the engine last asked the device for. Atomic.
@property (atomic, assign) int chosenBufferSize;
// The size the device was last opened with (devicefmt::OpenResult::targetBufferSize; 0: unknown),
// which JUCE's iOS device keeps asking for when it restarts itself on a route change. Main thread.
@property (nonatomic, assign) int deviceTargetBufferSize;
// The device's listed buffer sizes, copied on the main thread after each open and route check
// (JUCE rebuilds its list on the main thread without the engine lock). Atomic copy.
@property (atomic, copy) NSArray<NSNumber *> *listedBufferSizes;
// Diagnostics: the last kMaxDeviceOpenLog device opens (main thread writes a new array; atomic copy).
@property (atomic, copy) NSArray<NSDictionary<NSString *, id> *> *deviceOpenLog;
// Diagnostics from the most recently attached / touched JucePadView (main thread writes;
// reportTouchPath: / reportLastTouch:).
@property (atomic, copy) NSArray<NSDictionary<NSString *, id> *> *touchPathReport;
@property (atomic, copy) NSDictionary<NSString *, id> *lastTouchReport;
// The main-thread display link (ExpoJuceMainDisplayLink): the stall probe, running while the
// engine is set up, and the display-rate hint. Created on first use, kept for the process. Main thread.
@property (nonatomic, strong) ExpoJuceMainDisplayLink *mainDisplayLink;
// A route-change format check is scheduled (main thread; coalesces bursts of notifications).
@property (nonatomic, assign) BOOL routeFormatCheckPending;
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

// Background policy, Now Playing and remote commands (0.9.0-rc.5). Main thread unless noted.
@property (atomic, assign) BOOL idleStopped;          // device closed and session deactivated by the idle stop
@property (nonatomic, assign) BOOL rebuildAfterIdleStop; // a media-services reset came while idle-stopped
@property (nonatomic, strong) dispatch_source_t backgroundCheckTimer;
@property (atomic, copy) NSString *backgroundStartError; // the last failed restart from the background
@property (nonatomic, copy) NSDictionary<NSString *, id> *nowPlayingInfo; // title, artist, album; nil = none
@property (nonatomic, strong) NSMutableDictionary<NSString *, id> *remoteCommandTargets; // name -> target
@property (atomic, copy) NSArray<NSString *> *enabledRemoteCommandNames;
- (void)transportPlayStateChanged;
- (void)updateNowPlayingInfo;
- (void)updateSessionExclusivity;
- (AVAudioSessionCategoryOptions)sessionCategoryOptions;
- (void)resetRuntimeState;
- (void)stopBackgroundCheck;
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
// Heap-allocated and never freed: a producer on another thread during process exit never finds
// it destroyed.
static NoteEventQueue& sNoteQueue = *new NoteEventQueue;
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

// ── Touch latency, main-thread stalls and the display rate ──
// Process lifetime, atomics only (LatencyStats.h, DisplayRate.h). sTouchLatency: the delivery
// window is written on the main thread (JucePadView), the others on the audio thread
// (PolySynthProcessor). sStalls, sStallLog and sDisplayRate: written by the main display link's
// callback on the main thread. Diagnostics read all of them off the main thread.
static TouchLatencyStats sTouchLatency;
static StallWindow sStalls;
static StallLog sStallLog;
static std::atomic<bool> sStallProbeRunning{false};
static displayrate::RateWindow sDisplayRate;
static std::atomic<bool> sDisplayLinkTicking{false}; // the link exists and is not paused
static std::atomic<int> sDisplayRateHint{0};         // the last hint asked for (sanitized)
static std::atomic<int> sMaxFramesPerSecond{0};      // the screen's maximum; 0 until first read

// Xruns of devices the engine has closed or reopened since the app's last setup (rebuilds keep
// counting; AudioDeviceManager's count restarts with every open). Written on the main thread
// under the lifecycle lock (and reset by setup); read by diagnostics.
static std::atomic<uint64_t> sXrunsCarried{0};

// Main thread, under the lifecycle lock: keep the current device's xruns before closing it.
// (Reopens through DeviceFormat.h report the xruns they dropped in OpenResult::xrunsBeforeOpen.)
static void carryXrunsLocked() {
    if (sDeviceManager == nullptr || sDeviceManager->getCurrentAudioDevice() == nullptr) return;
    const int count = sDeviceManager->getXRunCount();
    if (count > 0) sXrunsCarried.fetch_add((uint64_t)count, std::memory_order_relaxed);
}

// ── MIDI byte input (MidiInput.h, ExpoJuceMidiInput.h) ──
// Process lifetime like the note queue it feeds: native code may call expojuce_midi_input_bytes
// from any thread at any time, before setup and across rebuilds (note-ons play only while
// sAcceptingNotes). Heap-allocated and never freed, so a call during process exit never finds it
// destroyed. Events for JS are drained on sMidiInputQueue (serial), at most ~4 ms after the first
// one of a burst, into one onMidiInput batch; the handler is only touched on that queue.
static midiin::NoteQueueSink& sMidiInputSink = *new midiin::NoteQueueSink(sNoteQueue, &sAcceptingNotes);
static midiin::MidiInput& sMidiInput = *new midiin::MidiInput(sMidiInputSink);
static LatencyWindow sMidiInputLatency; // arrival -> render, written by the audio thread
static void (^sMidiInputHandler)(NSDictionary<NSString *, id> *) = nil; // midiInputQueue only
// The module that set the handler (a reload can create the new module before the old one is
// destroyed: the old one's clear must not remove the new handler) and whether its JS listens.
static OwnerToken sMidiInputHandlerOwner;
static std::atomic<bool> sMidiInputListening{false};
// The module instance that owns the engine's lifetime (see -claimModule / -releaseModule:).
static OwnerToken sModuleOwner;
static const int64_t kMidiInputFlushNs = 4 * (int64_t)NSEC_PER_MSEC;

static dispatch_queue_t midiInputQueue() {
    static dispatch_queue_t queue;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        dispatch_queue_attr_t attr = dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL,
                                                                             QOS_CLASS_USER_INTERACTIVE, 0);
        queue = dispatch_queue_create("expo-juce.midi-input", attr);
    });
    return queue;
}

static NSDictionary<NSString *, id> *midiInputEventDictionary(const midiin::Event& e) {
    NSMutableDictionary<NSString *, id> *d = [NSMutableDictionary dictionaryWithCapacity:12];
    const midiin::Message& m = e.message;
    switch (m.type) {
        case midiin::kNoteOn:
        case midiin::kNoteOff:
            d[@"type"] = m.type == midiin::kNoteOn ? @"noteOn" : @"noteOff";
            d[@"note"] = @(m.data1);
            d[@"velocity"] = @(m.data2);
            break;
        case midiin::kPolyPressure:
            d[@"type"] = @"polyPressure";
            d[@"note"] = @(m.data1);
            d[@"pressure"] = @(m.data2);
            break;
        case midiin::kControlChange:
            d[@"type"] = @"cc";
            d[@"controller"] = @(m.data1);
            d[@"value"] = @(m.data2);
            break;
        default:
            d[@"type"] = @"pitchBend";
            d[@"value"] = @(m.value);
            break;
    }
    d[@"channel"] = @(m.channel);
    d[@"arrival"] = @(e.arrival);
    d[@"epoch"] = @(e.epoch);
    d[@"played"] = @((e.flags & midiin::kEventPlayed) != 0);
    if (e.out >= 0) {
        d[@"part"] = (e.flags & midiin::kEventDrum) ? @"drums" : @"synth";
        d[@"out"] = @(e.out);
        if (m.type == midiin::kNoteOn) d[@"outVelocity"] = @(e.outVelocity);
    }
    if (e.flags & midiin::kEventLast) d[@"last"] = @YES;
    if (e.flags & midiin::kEventSynthetic) d[@"synthetic"] = @YES;
    return d;
}

// sMidiInputQueue: every event recorded so far, as one batch for the handler (dropped if none).
static void flushMidiInputEvents(void *) {
    @autoreleasepool {
        sMidiInput.beginDrain();
        void (^handler)(NSDictionary<NSString *, id> *) = sMidiInputHandler;
        if (handler == nil || !sMidiInputListening.load(std::memory_order_acquire)) {
            sMidiInput.drainEvents([](const midiin::Event&) {}); // nobody listens: drop them
            return;
        }
        NSMutableArray<NSDictionary<NSString *, id> *> *events = [NSMutableArray array];
        sMidiInput.drainEvents([&](const midiin::Event& e) { [events addObject:midiInputEventDictionary(e)]; });
        if (events.count == 0) return;
        handler(@{
            @"events": events,
            @"now": @(midiin::NoteQueueSink::nowSeconds()),
            @"epoch": @(sMidiInput.epoch()),
        });
    }
}

static void scheduleMidiInputFlush() {
    dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, kMidiInputFlushNs), midiInputQueue(), nullptr,
                     flushMidiInputEvents);
}

// ── Loop sequence (SequencePlayer.h) ──
// Process lifetime like the other stores: setSequence works before setup and survives rebuilds
// (each synth the engine builds plays it). Heap-allocated and never freed. sSequenceLog carries
// the notes the player fired to the drain timer below (only while JS listens).
static seqplay::SequenceStore& sSequence = *new seqplay::SequenceStore;
static seqplay::NoteLog& sSequenceLog = *new seqplay::NoteLog;
static const int64_t kSequenceDrainNs = 16 * (int64_t)NSEC_PER_MSEC;

// ── Engine events for JS (onSequenceNotes, onRemoteCommand, ...) ──
// One handler, set by the module that owns it (token, as for the MIDI input handler); touched only
// on engineEventQueue, where every event is delivered.
static void (^sEngineEventHandler)(NSString *, NSDictionary<NSString *, id> *) = nil;
static OwnerToken sEngineEventOwner;
static dispatch_source_t sSequenceDrainTimer = nil; // engineEventQueue only

// ── Background policy (BackgroundPolicy.h) ──
// Set from any thread (atomics), applied on the main thread, which alone owns sIdleTracker.
static std::atomic<bool> sPolicySet{false};
static std::atomic<bool> sPolicyKeepAlive{true};
static std::atomic<double> sPolicyIdleStopSeconds{bgpolicy::kDefaultIdleStopSeconds};
static bgpolicy::IdleTracker sIdleTracker;                // main thread
static std::atomic<int> sBackgroundState{0};              // bgpolicy::State, for diagnostics
static std::atomic<double> sIdleStopAt{-1.0};             // CACurrentMediaTime() seconds, -1 = none
static std::atomic<uint64_t> sIdleStops{0};
static std::atomic<double> sLastIdleStopSec{-1.0};
static std::atomic<bool> sInBackground{false};
static std::atomic<bool> sNowPlayingSet{false};
static std::atomic<bool> sSessionExclusive{false};        // the session is not mixable (Now Playing)

static bgpolicy::Policy currentBackgroundPolicy() {
    bgpolicy::Policy p;
    p.set = sPolicySet.load(std::memory_order_acquire);
    p.keepAlive = sPolicyKeepAlive.load(std::memory_order_relaxed);
    p.idleStopSeconds = sPolicyIdleStopSeconds.load(std::memory_order_relaxed);
    return p;
}

static dispatch_queue_t engineEventQueue() {
    static dispatch_queue_t queue;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        dispatch_queue_attr_t attr = dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL,
                                                                             QOS_CLASS_USER_INITIATED, 0);
        queue = dispatch_queue_create("expo-juce.engine-events", attr);
    });
    return queue;
}

// Any thread: delivers one event to the handler (if any) on engineEventQueue.
static void emitEngineEvent(NSString *name, NSDictionary<NSString *, id> *body) {
    dispatch_async(engineEventQueue(), ^{
        void (^handler)(NSString *, NSDictionary<NSString *, id> *) = sEngineEventHandler;
        if (handler != nil) handler(name, body);
    });
}

// engineEventQueue: the notes the sequence fired since the last drain, as one onSequenceNotes batch.
static void drainSequenceNotes() {
    @autoreleasepool {
        NSMutableArray<NSDictionary<NSString *, id> *> *notes = [NSMutableArray array];
        sSequenceLog.drain([&](const seqplay::NoteLog::Entry& e) {
            [notes addObject:@{
                @"note": @(e.note),
                @"kind": e.kind == seqplay::EventKind::Drum ? @"drum" : @"note",
                @"on": @(e.on ? YES : NO),
                @"atBeat": @(e.atBeat),
            }];
        });
        void (^handler)(NSString *, NSDictionary<NSString *, id> *) = sEngineEventHandler;
        if (notes.count == 0 || handler == nil || !sSequenceLog.listening.load(std::memory_order_relaxed)) return;
        handler(@"onSequenceNotes", @{ @"notes": notes, @"now": @(CACurrentMediaTime()) });
    }
}

// Any thread: starts / stops the drain timer (on engineEventQueue, so calls apply in order).
static void setSequenceDrainRunning(bool on) {
    dispatch_async(engineEventQueue(), ^{
        if (on && sSequenceDrainTimer == nil) {
            dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, engineEventQueue());
            dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, kSequenceDrainNs), (uint64_t)kSequenceDrainNs,
                                      2 * NSEC_PER_MSEC);
            dispatch_source_set_event_handler(timer, ^{
                drainSequenceNotes();
            });
            dispatch_resume(timer);
            sSequenceDrainTimer = timer;
        } else if (!on && sSequenceDrainTimer != nil) {
            dispatch_source_cancel(sSequenceDrainTimer);
            sSequenceDrainTimer = nil;
            sSequenceLog.drain([](const seqplay::NoteLog::Entry&) {}); // nobody listens: drop them
        }
    });
}

// The C entry point (ExpoJuceMidiInput.mm) feeds sMidiInput from static initialisation on.
__attribute__((unused)) static const bool sMidiInputInstalled = (midiin::installEntryPoint(&sMidiInput, scheduleMidiInputFlush), true);

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

// The main-thread display link: one CADisplayLink on the main run loop (common modes, so it also
// ticks while a scroll view tracks) with two jobs.
//  - Stall probe (while `probing`: from a successful setup until shutdown): each tick records the
//    time since the previous one in sStalls and sStallLog; a long gap means the main thread (where
//    touches are delivered) was busy.
//  - Display-rate hint (setDisplayRateHint): above 60 on a screen that goes faster, the link asks
//    for (80, hint, hint) clamped to the screen's maximum, which keeps the display at that rate;
//    otherwise it is held to 60, so the probe alone never raises the display rate.
// The link exists while either job wants it, and every tick measures its rate (sDisplayRate). The
// callback stores atomics only: no allocation, no lock. Paused while the app is inactive (created
// paused when it is not active); the gap across a pause is not counted. Main thread only.
@interface ExpoJuceMainDisplayLink : NSObject
- (void)setProbing:(BOOL)probing;
- (void)setRateHint:(int)hint;
@end

// The largest maximumFramesPerSecond of the app's connected screens; the main screen's when no
// window scene is connected yet (early in launch), so a hint is never dropped for want of a
// scene. Main thread.
static int mainThreadMaxFramesPerSecond() {
    NSInteger best = 0;
    for (UIScene *scene in [UIApplication sharedApplication].connectedScenes) {
        if ([scene isKindOfClass:[UIWindowScene class]])
            best = MAX(best, ((UIWindowScene *)scene).screen.maximumFramesPerSecond);
    }
    if (best == 0) best = [UIScreen mainScreen].maximumFramesPerSecond; // deprecated in iOS 26, still correct
    return (int)best;
}

@implementation ExpoJuceMainDisplayLink {
    CADisplayLink *_link;
    CFTimeInterval _last;
    BOOL _probing;
    int _hint;
    displayrate::RateRange _applied; // the range last applied to _link
}

- (void)setProbing:(BOOL)probing {
    _probing = probing;
    _last = 0;
    [self update];
}

- (void)setRateHint:(int)hint {
    _hint = hint;
    [self update];
}

// Starts, retunes or stops the link for the jobs it has now.
- (void)update {
    const int maxFps = mainThreadMaxFramesPerSecond();
    if (maxFps > 0) sMaxFramesPerSecond.store(maxFps, std::memory_order_relaxed);
    const displayrate::RateRange range = displayrate::rangeFor(_hint, maxFps);
    if (!_probing && !range.raised) {
        [self stopLink];
        return;
    }
    if (_link == nil) [self startLink];
    [self applyRange:range];
}

- (void)startLink {
    // The link retains its target until -stopLink invalidates it.
    _link = [CADisplayLink displayLinkWithTarget:self selector:@selector(tick:)];
    _last = 0;
    _applied = displayrate::RateRange{};
    sDisplayRate.clear();
    NSNotificationCenter *center = [NSNotificationCenter defaultCenter];
    [center addObserver:self selector:@selector(pause) name:UIApplicationWillResignActiveNotification object:nil];
    [center addObserver:self selector:@selector(resume) name:UIApplicationDidBecomeActiveNotification object:nil];
    if ([UIApplication sharedApplication].applicationState != UIApplicationStateActive) _link.paused = YES;
    [_link addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
    sDisplayLinkTicking.store(!_link.paused, std::memory_order_relaxed);
}

- (void)stopLink {
    if (_link == nil) return;
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [_link invalidate];
    _link = nil;
    _last = 0;
    sDisplayLinkTicking.store(false, std::memory_order_relaxed);
}

// No range (a 60 Hz screen, or its maximum unknown): the link keeps its default. A new range
// starts the measured rate afresh.
- (void)applyRange:(displayrate::RateRange)range {
    if (_link == nil || !(range.maximum > 0.0f)) return;
    if (range.minimum == _applied.minimum && range.maximum == _applied.maximum && range.preferred == _applied.preferred) return;
    _applied = range;
    sDisplayRate.clear();
    if (@available(iOS 15.0, *)) {
        _link.preferredFrameRateRange = CAFrameRateRangeMake(range.minimum, range.maximum, range.preferred);
    } else {
        _link.preferredFramesPerSecond = (NSInteger)std::lround(range.preferred);
    }
}

- (void)pause {
    _link.paused = YES;
    _last = 0;
    sDisplayLinkTicking.store(false, std::memory_order_relaxed);
}

- (void)resume {
    _last = 0;
    sDisplayRate.clear();
    _link.paused = NO;
    sDisplayLinkTicking.store(_link != nil, std::memory_order_relaxed);
}

- (void)tick:(CADisplayLink *)link {
    sDisplayRate.push(link.targetTimestamp - link.timestamp);
    if (!_probing) return;
    const CFTimeInterval now = CACurrentMediaTime();
    if (_last > 0) {
        const double gapMs = (now - _last) * 1000.0;
        sStalls.addGap(now, gapMs);
        sStallLog.addGap(now, gapMs);
    }
    _last = now;
}

@end

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
        _targetSampleRate = kDefaultSampleRate;
        // The aux node asks for the track at the device rate when it is prepared (setup, rebuild,
        // device restart with a new rate): decode the file again at that rate off that thread,
        // then free the old buffer.
        sBackingTrack.setRateListener([](double) {
            dispatch_async(backingTrackQueue(), ^{
                sBackingTrack.reload();
                [[ExpoJuceAudioEngine sharedInstance] scheduleBackingTrackCollect];
            });
        });
        [ExpoJuceAudioEngine warmCoreMidiOffMainThread];
        _remoteCommandTargets = [NSMutableDictionary dictionary];
        _enabledRemoteCommandNames = @[];
        // Background policy: app state for the whole life of the engine object (delivered on the
        // main thread; ignored while the engine is not set up).
        NSNotificationCenter *center = [NSNotificationCenter defaultCenter];
        [center addObserver:self selector:@selector(handleAppDidEnterBackground:)
                       name:UIApplicationDidEnterBackgroundNotification object:nil];
        [center addObserver:self selector:@selector(handleAppWillEnterForeground:)
                       name:UIApplicationWillEnterForegroundNotification object:nil];
    }
    return self;
}

// The first CoreMIDI call in a process pays for MIDIProcessXPC::InitOnce — a
// synchronous XPC handshake that launches midiserver and can take seconds. JUCE
// requires its MIDI calls on the message thread, so when that cost lands on a
// MIDI call it blocks the main thread.
//
// That initialisation is process-global and not thread-affine, so touching any
// cheap CoreMIDI query from a background queue first absorbs the cost there.
// Afterwards MIDIProcess::CheckInitialization on the main thread finds the
// connection already up and returns immediately.
//
// This is belt-and-braces: with midiDeviceListConnection now made lazily (see
// juce_AudioDeviceManager.h), a host that never uses MIDI devices never calls
// into CoreMIDI at all. This keeps the main thread clear for when one does —
// e.g. BLE MIDI. If the warm-up has not finished by then, behaviour is exactly
// what it is today, so this can only help.
+ (void)warmCoreMidiOffMainThread {
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
            // Return value deliberately unused; the call is made for the
            // initialisation it forces, not the count.
            (void)MIDIGetNumberOfSources();
        });
    });
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

    self.idleStopped = NO;
    self.requestedBufferSize = (bufferSize > 0) ? std::min(bufferSize, 4096) : kDefaultBufferSize;
    self.requestedSampleRate = (sampleRate > 0.0) ? sampleRate : kDefaultSampleRate;
    sXrunsCarried.store(0, std::memory_order_relaxed); // xrunCount counts from this setup

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
        [self startStallProbe];
        // An explicit setup supersedes any pending rebuild retry.
        self.recoveryGeneration = self.recoveryGeneration + 1;
        self.rebuildRetryPending = NO;
        self.engineFailed = NO;
    } else if (!self.rebuildRetryPending) {
        self.engineFailed = YES;
    }
    return ok;
}

// Main thread: the display link, created on first use.
- (ExpoJuceMainDisplayLink *)ensureMainDisplayLink {
    if (self.mainDisplayLink == nil) self.mainDisplayLink = [[ExpoJuceMainDisplayLink alloc] init];
    return self.mainDisplayLink;
}

// Main thread. The stall probe runs from a successful setup until shutdown (rebuilds keep it).
- (void)startStallProbe {
    [[self ensureMainDisplayLink] setProbing:YES];
    sStallProbeRunning.store(true, std::memory_order_relaxed);
}

// The link stays while the display-rate hint holds the display above 60.
- (void)stopStallProbe {
    sStallProbeRunning.store(false, std::memory_order_relaxed);
    [self.mainDisplayLink setProbing:NO];
}

// Any thread. The hint is stored now (diagnostics report it) and applied on the main thread,
// which reads the latest one, so a burst of calls ends on the last.
- (BOOL)setDisplayRateHint:(double)hz {
    if (!std::isfinite(hz)) return NO;
    sDisplayRateHint.store(displayrate::sanitizeHint(hz), std::memory_order_relaxed);
    if ([NSThread isMainThread]) {
        [self applyDisplayRateHint];
    } else {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self applyDisplayRateHint];
        });
    }
    return YES;
}

// Main thread.
- (void)applyDisplayRateHint {
    [[self ensureMainDisplayLink] setRateHint:sDisplayRateHint.load(std::memory_order_relaxed)];
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
             withOptions:[self sessionCategoryOptions]
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

    // Preferred hardware format. The rate is the session's current one, read now, after activation
    // and before JUCE creates its device (whose constructor leaves a 44.1 kHz preference behind):
    // the route's own rate on a fresh session, else the rate last preferred in this process. The
    // app's rate is only a hint for when the session reports no music rate. The IO buffer duration is
    // computed at that rate. JUCE's iOS device re-requests the buffer duration itself when it
    // opens (see applyPreferredDeviceSetup below); these preferences cover the first open.
    const int preferredBuffer = self.requestedBufferSize;
    self.hardwareSampleRate = session.sampleRate;
    self.targetSampleRate = devicefmt::chooseDeviceSampleRate(session.sampleRate, self.requestedSampleRate);
    const double preferredRate = self.targetSampleRate;
    NSString *preferenceError = [self applySessionPreferences];
    if (preferenceError) {
        [self noteNonFatal:[NSString stringWithFormat:@"step 1 (non-fatal): %@", preferenceError]];
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
        [self applyPreferredDeviceSetupLocked:@"setup" bufferSize:preferredBuffer sampleRate:preferredRate];
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
    sSynth->setTouchLatency(&sTouchLatency);      // process-lifetime windows, kept across rebuilds
    sSynth->setInputLatency(&sMidiInputLatency); // same, MIDI input arrival -> render
    sSynth->arp().setSettingsStore(&sArpOptions); // the live options, before and across rebuilds
    sSynth->drums().setParamStore(&sDrumParams);  // same for the drum part
    sSynth->setSequenceStore(&sSequence);         // the loop sequence, before and across rebuilds
    sSynth->setSequenceNoteLog(&sSequenceLog);

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

// Main thread. Sets the session's preferred sample rate (targetSampleRate) and the IO buffer
// duration for requestedBufferSize frames at that rate. Returns nil, or what failed (non-fatal).
- (NSString *)applySessionPreferences {
    AVAudioSession *session = [AVAudioSession sharedInstance];
    const double rate = self.targetSampleRate;
    const int frames = self.requestedBufferSize;
    NSMutableArray<NSString *> *problems = [NSMutableArray array];
    NSError *rateError = nil;
    [session setPreferredSampleRate:rate error:&rateError];
    if (rateError) {
        [problems addObject:[NSString stringWithFormat:@"setPreferredSampleRate(%.0f): %@", rate,
                             rateError.localizedDescription]];
    }
    const double duration = devicefmt::ioBufferDurationSeconds(frames, rate);
    if (duration > 0.0) {
        NSError *durationError = nil;
        [session setPreferredIOBufferDuration:(NSTimeInterval)duration error:&durationError];
        if (durationError) {
            [problems addObject:[NSString stringWithFormat:@"setPreferredIOBufferDuration(%d/%.0f): %@", frames, rate,
                                 durationError.localizedDescription]];
        }
    }
    return problems.count > 0 ? [problems componentsJoinedByString:@"; "] : nil;
}

// Main thread, under the lifecycle lock. Copies the device's listed buffer sizes for diagnostics.
- (void)refreshListedBufferSizesLocked {
    NSMutableArray<NSNumber *> *sizes = [NSMutableArray array];
    if (sDeviceManager != nullptr) {
        if (juce::AudioIODevice* device = sDeviceManager->getCurrentAudioDevice()) {
            for (int size : device->getAvailableBufferSizes()) [sizes addObject:@(size)];
        }
    }
    self.listedBufferSizes = sizes;
}

// Main thread, under the lifecycle lock (or during doSetup). Records the outcome of an
// applyPreferredSetup / reopenWithPreferredSetup call: carries the xruns of the device it
// reopened, remembers the size the device was opened with, and appends one entry to deviceOpenLog.
- (void)logDeviceOpen:(NSString *)context result:(const devicefmt::OpenResult &)result sampleRate:(double)sampleRate {
    if (result.xrunsBeforeOpen > 0) sXrunsCarried.fetch_add((uint64_t)result.xrunsBeforeOpen, std::memory_order_relaxed);
    if (result.opened) self.deviceTargetBufferSize = result.targetBufferSize;
    double grantedRate = 0.0;
    int granted = 0;
    if (sDeviceManager != nullptr) {
        if (juce::AudioIODevice* device = sDeviceManager->getCurrentAudioDevice()) {
            grantedRate = device->getCurrentSampleRate();
            granted = device->getCurrentBufferSizeSamples();
        }
    }
    [self refreshListedBufferSizesLocked];
    self.chosenBufferSize = result.chosenBufferSize;
    NSString *error = result.error.isEmpty() ? @"" : ([NSString stringWithUTF8String:result.error.toRawUTF8()] ?: @"(unprintable error)");
    NSDictionary<NSString *, id> *entry = @{
        @"context": context ?: @"",
        @"atMs": @(CACurrentMediaTime() * 1000.0),
        @"preferredBufferSize": @(self.requestedBufferSize),
        @"requestedBufferSize": @(result.chosenBufferSize),
        @"grantedBufferSize": @(granted),
        @"requestedSampleRate": @(sampleRate),
        @"grantedSampleRate": @(grantedRate),
        @"opened": @(result.opened ? YES : NO),
        @"newDevice": @(result.reopened ? YES : NO),
        @"error": error,
    };
    NSMutableArray<NSDictionary<NSString *, id> *> *log = [NSMutableArray arrayWithArray:self.deviceOpenLog ?: @[]];
    [log addObject:entry];
    while (log.count > (NSUInteger)kMaxDeviceOpenLog) [log removeObjectAtIndex:0];
    self.deviceOpenLog = log;
    NSLog(@"[ExpoJuceAudioEngine] device open (%@): sr %.0f -> %.0f, buf %d (listed %d) -> %d%@%@", context, sampleRate,
          grantedRate, self.requestedBufferSize, result.chosenBufferSize, granted, error.length ? @", error: " : @"", error);
}

// Main thread, caller holds sEngineLock (or is doSetup). Asks the open device for the preferred
// sample rate / buffer size through JUCE, so JUCE's own device restarts (route changes) keep it:
// the nearest size in the device's own list (devicefmt::applyPreferredSetup; any other size
// would silently become JUCE's default, 256 on iOS). A request equal to the device's current
// setup reopens nothing (or a new device if that setup is stale). Non-fatal: on error the device
// is restored.
- (void)applyPreferredDeviceSetupLocked:(NSString *)context bufferSize:(int)bufferSize sampleRate:(double)sampleRate {
    if (sDeviceManager == nullptr || sDeviceManager->getCurrentAudioDevice() == nullptr) return;
    const devicefmt::OpenResult result = devicefmt::applyPreferredSetup(*sDeviceManager, bufferSize, sampleRate);
    [self logDeviceOpen:context result:result sampleRate:sampleRate];
    if (result.error.isEmpty()) return;

    NSString *message = [NSString stringWithUTF8String:result.error.toRawUTF8()];
    NSString *problem = [NSString stringWithFormat:@"%@: setAudioDeviceSetup(%.0f Hz, %d): %@", context, sampleRate,
                         result.chosenBufferSize, message ?: @"(unprintable error)"];
    if (result.restoreError.isNotEmpty()) {
        NSString *restoreMessage = [NSString stringWithUTF8String:result.restoreError.toRawUTF8()];
        problem = [problem stringByAppendingFormat:@"; restoring device setup: %@", restoreMessage ?: @"(unprintable error)"];
    }
    if (self.isReady) [self noteRecoveryError:problem];
    else [self noteNonFatal:[@"step 4 (non-fatal): " stringByAppendingString:problem]];
}

// Main thread, caller holds sEngineLock, engine set up. Reopens the device as a new device object
// with the preferred format (session preferences first, then devicefmt::reopenWithPreferredSetup).
// JUCE's restartLastAudioDevice alone would come back at its default buffer size.
- (void)reopenDeviceWithPreferredFormatLocked:(NSString *)context {
    if (sDeviceManager == nullptr) return;
    NSString *preferenceError = [self applySessionPreferences];
    if (preferenceError) [self noteRecoveryError:[NSString stringWithFormat:@"%@: %@", context, preferenceError]];
    const devicefmt::OpenResult result =
        devicefmt::reopenWithPreferredSetup(*sDeviceManager, self.requestedBufferSize, self.targetSampleRate);
    [self logDeviceOpen:context result:result sampleRate:self.targetSampleRate];
    if (result.error.isNotEmpty()) {
        NSString *message = [NSString stringWithUTF8String:result.error.toRawUTF8()];
        [self noteRecoveryError:[NSString stringWithFormat:@"%@: reopening with the preferred format: %@", context,
                                 message ?: @"(unprintable error)"]];
    }
}

// Main thread. After a route change or an interruption: re-read the session's rate, reapply the
// session preferences, and if the running device has drifted from them ask it again. A device
// that is not running is left to restartDeviceIfStopped / the dead-device check.
//
// What the rate check can see: by the time it runs, JUCE's iOS device has restarted itself and
// asked the session for the rate it was opened with, so session.sampleRate is that rate whenever
// the new route can run it. The engine therefore moves to another rate only when the new route
// cannot run the current one (e.g. a 96 kHz-only interface); it does not discover that a route
// would prefer another rate it can also run. Voice rates (e.g. right after a call) are ignored.
// The buffer check: the nearest listed size on the (possibly new) route is not the size the device
// was opened with, which JUCE's restart keeps asking for (devicefmt::bufferChoiceDrifted).
- (void)ensurePreferredFormat:(NSString *)context {
    if (!self.isReady || self.isRecovering || sDeviceManager == nullptr) return;
    AVAudioSession *session = [AVAudioSession sharedInstance];
    // The rate in use is the fallback: a voice rate reported right after a call is not adopted.
    self.targetSampleRate = devicefmt::chooseDeviceSampleRate(session.sampleRate, self.targetSampleRate);
    NSString *preferenceError = [self applySessionPreferences];
    if (preferenceError) [self noteRecoveryError:[NSString stringWithFormat:@"%@: %@", context, preferenceError]];

    LifecycleLock lifecycle;
    [self refreshListedBufferSizesLocked];
    juce::AudioIODevice* device = sDeviceManager->getCurrentAudioDevice();
    if (device == nullptr || !device->isPlaying()) return;
    const bool rateDrifted = std::abs(device->getCurrentSampleRate() - self.targetSampleRate) > 0.5;
    const bool bufferDrifted = devicefmt::bufferChoiceDrifted(device->getAvailableBufferSizes(), self.requestedBufferSize,
                                                              self.deviceTargetBufferSize);
    if (!rateDrifted && !bufferDrifted) return;
    NSLog(@"[ExpoJuceAudioEngine] %@: device at %.0f Hz / %d, want %.0f Hz / %d; asking again", context,
          device->getCurrentSampleRate(), device->getCurrentBufferSizeSamples(), self.targetSampleRate,
          self.requestedBufferSize);
    [self applyPreferredDeviceSetupLocked:context bufferSize:self.requestedBufferSize sampleRate:self.targetSampleRate];
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
    carryXrunsLocked();
    self.deviceTargetBufferSize = 0;
    self.listedBufferSizes = @[];
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
    [self stopStallProbe];
    [self stopBackgroundCheck];
    self.idleStopped = NO;
    self.rebuildAfterIdleStop = NO;
    sSessionExclusive.store(false, std::memory_order_relaxed); // the next setup starts mixable
    self.routeFormatCheckPending = NO;
    if (!self.isReady) return;

    NSLog(@"[ExpoJuceAudioEngine] Shutting down...");
    {
        LifecycleLock lifecycle;
        sMidiInput.allNotesOff(); // releases the queue; the MIDI input forgets its holds
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
    __weak ExpoJuceAudioEngine *weakSelf = self;
    // Each case body that holds a block literal is braced: under ARC a block capturing a __weak
    // variable opens a cleanup scope, and clang rejects a later case label that jumps past it.
    switch (reason) {
        case AVAudioSessionRouteChangeReasonCategoryChange: {
            dispatch_async(dispatch_get_main_queue(), ^{
                [weakSelf reassertSessionCategoryIfNeeded];
            });
            return;
        }
        // The reasons JUCE's iOS device restarts itself for (asynchronously, keeping the size and
        // rate it was opened with). Afterwards, check the format again (ensurePreferredFormat).
        case AVAudioSessionRouteChangeReasonNewDeviceAvailable:
        case AVAudioSessionRouteChangeReasonOldDeviceUnavailable:
        case AVAudioSessionRouteChangeReasonOverride:
        case AVAudioSessionRouteChangeReasonWakeFromSleep:
        case AVAudioSessionRouteChangeReasonNoSuitableRouteForCategory:
        case AVAudioSessionRouteChangeReasonUnknown: {
            dispatch_async(dispatch_get_main_queue(), ^{
                [weakSelf scheduleRouteFormatCheck];
            });
            return;
        }
        default:
            return;
    }
}

// Main thread. One format check kRecoveryDelayMs after a burst of route changes (JUCE's own
// restart runs first).
- (void)scheduleRouteFormatCheck {
    if (!self.isReady || self.routeFormatCheckPending) return;
    self.routeFormatCheckPending = YES;
    __weak ExpoJuceAudioEngine *weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, kRecoveryDelayMs * (int64_t)NSEC_PER_MSEC),
                   dispatch_get_main_queue(), ^{
        ExpoJuceAudioEngine *strongSelf = weakSelf;
        if (strongSelf == nil || !strongSelf.routeFormatCheckPending) return;
        strongSelf.routeFormatCheckPending = NO;
        [strongSelf ensurePreferredFormat:@"route change"];
    });
}

// Main thread. Another component (e.g. a recording or video library) switched the shared session
// to a category that cannot play our output; switch it back.
- (void)reassertSessionCategoryIfNeeded {
    if (!self.isReady || self.isReassertingSession || self.idleStopped) return;
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
             withOptions:[self sessionCategoryOptions]
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
            [strongSelf handleInterruptionBegan];
        });
    } else if (type == AVAudioSessionInterruptionTypeEnded) {
        // The device is made playable again whether or not the system set
        // AVAudioSessionInterruptionOptionShouldResume: this is an instrument; it should be playable
        // as soon as the interruption is over. The transport stays stopped (the app decides).
        // Delayed so JUCE's own handler (which restarts its audio unit) runs first.
        emitEngineEvent(@"onAudioInterruption", @{ @"type": @"ended", @"playing": @NO });
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, kRecoveryDelayMs * (int64_t)NSEC_PER_MSEC),
                       dispatch_get_main_queue(), ^{
            [weakSelf recoverFromInterruption];
        });
    }
}

// Main thread. A call, Siri, or (with a non-mixable session) another app's audio took the session:
// stop the transport and release every note, so nothing sounds on (or resumes) afterwards.
- (void)handleInterruptionBegan {
    const BOOL wasPlaying = self.isReady ? [self isTransportPlaying] : NO;
    if (self.isReady) {
        [self stopTransport];
        [self allNotesOff];
    }
    emitEngineEvent(@"onAudioInterruption", @{ @"type": @"began", @"playing": @NO, @"wasPlaying": @(wasPlaying) });
    if (wasPlaying) emitEngineEvent(@"onTransportChange", @{ @"playing": @NO, @"reason": @"interruption" });
}

// Main thread.
- (void)recoverFromInterruption {
    if (!self.isReady || sDeviceManager == nullptr || self.idleStopped) return;
    NSLog(@"[ExpoJuceAudioEngine] audio session interruption ended; reactivating");
    NSError *activationError = nil;
    [[AVAudioSession sharedInstance] setActive:YES error:&activationError];
    if (activationError) {
        [self noteRecoveryError:[NSString stringWithFormat:@"interruption ended setActive: %@",
                                 activationError.localizedDescription]];
    }
    // Another session (a call, Siri) may have left the hardware at its own rate / IO buffer.
    [self ensurePreferredFormat:@"interruption"];
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
    if (!self.isReady || self.isRecovering || sPlayer == nullptr || self.idleStopped) return;
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
    if (!self.isReady || self.isRecovering || sDeviceManager == nullptr || self.idleStopped) return;
    if (sPlayer == nullptr || sPlayer != player) return;
    if (player->callbackCount.load(std::memory_order_relaxed) != before) return; // alive

    if (!restarted) {
        sDeadDeviceRestarts.fetch_add(1, std::memory_order_relaxed);
        NSLog(@"[ExpoJuceAudioEngine] audio callback stalled after %@; reopening device", context);
        {
            LifecycleLock lifecycle;
            [self reopenDeviceWithPreferredFormatLocked:[NSString stringWithFormat:@"dead device (%@)", context]];
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
    if (!self.isReady || sDeviceManager == nullptr || self.idleStopped) return;
    if ([self isDeviceRunning]) return;

    NSLog(@"[ExpoJuceAudioEngine] device not running after %@; reopening", context);
    {
        LifecycleLock lifecycle;
        [self reopenDeviceWithPreferredFormatLocked:[NSString stringWithFormat:@"stopped device (%@)", context]];
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
        // Idle-stopped in the background: rebuild when the device is wanted again.
        if (strongSelf.idleStopped) {
            strongSelf.rebuildAfterIdleStop = YES;
            return;
        }
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
        sMidiInput.allNotesOff(); // releases the queue; the MIDI input forgets its holds
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
    double xrunCount = (double)sXrunsCarried.load(std::memory_order_relaxed);
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
                    xrunCount += (double)std::max(0, sDeviceManager->getXRunCount());
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

    // Device format: what the engine asked for vs what it got (DeviceFormat.h).
    d[@"hardwareSampleRate"] = @(self.hardwareSampleRate);
    d[@"requestedSampleRate"] = @(self.targetSampleRate);
    d[@"grantedSampleRate"] = @(sampleRate);
    d[@"requestedBufferSize"] = @(self.chosenBufferSize);
    d[@"grantedBufferSize"] = @(bufferSize);
    d[@"availableBufferSizes"] = self.listedBufferSizes ?: @[];
    d[@"deviceOpens"] = self.deviceOpenLog ?: @[];
    d[@"xrunCount"] = @(xrunCount);

    // Touch → sound legs (LatencyStats.h) and the main-thread stall probe.
    NSMutableDictionary<NSString *, id> *touchLatency = [NSMutableDictionary dictionary];
    const auto leg = [](const LatencyWindow& window) -> NSDictionary<NSString *, id> * {
        const LatencySummary s = window.summary();
        return @{ @"last": @(s.last), @"p50": @(s.p50), @"p95": @(s.p95), @"max": @(s.max), @"count": @((double)s.count) };
    };
    touchLatency[@"touchDeliveryMs"] = leg(sTouchLatency.delivery);
    touchLatency[@"touchHandlerMs"] = leg(sTouchLatency.handler);
    touchLatency[@"queueToRenderMs"] = leg(sTouchLatency.queueToRender);
    touchLatency[@"touchToRenderMs"] = leg(sTouchLatency.touchToRender);
    touchLatency[@"bufferMs"] = @(sampleRate > 0.0 ? (double)bufferSize * 1000.0 / sampleRate : 0.0);
    touchLatency[@"outputLatencyMs"] = @(session.outputLatency * 1000.0);
    touchLatency[@"midiInputToRenderMs"] = leg(sMidiInputLatency);
    d[@"touchLatency"] = touchLatency;

    // MIDI byte input (MidiInput.h): its counters and the arrival -> render leg.
    const midiin::Stats midi = sMidiInput.stats();
    d[@"midiInput"] = @{
        @"enabled": @(sMidiInput.enabled()),
        @"mapVersion": @(sMidiInput.map().version()),
        @"packets": @((double)midi.packets),
        @"messages": @((double)midi.messages),
        @"played": @((double)midi.played),
        @"passed": @((double)midi.passed),
        @"dropped": @((double)midi.dropped),
        @"rejectedPackets": @((double)midi.rejectedPackets),
        @"eventsDropped": @((double)midi.eventsDropped),
        @"held": @(midi.held),
        @"epoch": @(sMidiInput.epoch()),
        @"midiInputToRenderMs": leg(sMidiInputLatency),
    };
    d[@"touchPath"] = self.touchPathReport ?: @[];
    d[@"lastTouch"] = self.lastTouchReport ?: @{};
    const double clockSec = CACurrentMediaTime();
    d[@"mainThreadMaxGapMs"] = @(sStallProbeRunning.load(std::memory_order_relaxed) ? sStalls.maxGapMs(clockSec) : 0.0);
    d[@"mainThreadStalls50"] = @((double)sStalls.stallCount());
    // The last StallLog::kSize stalls (start on the clockSec clock, length) and their cadence.
    StallRecord stalls[StallLog::kSize];
    const int stallCount = sStallLog.copy(stalls);
    NSMutableArray<NSDictionary<NSString *, id> *> *stallLog = [NSMutableArray arrayWithCapacity:(NSUInteger)stallCount];
    for (int k = 0; k < stallCount; ++k)
        [stallLog addObject:@{ @"atSec": @(stalls[k].atSec), @"ms": @(stalls[k].ms) }];
    d[@"mainThreadStallLog"] = stallLog;
    d[@"mainThreadStallIntervalP50Ms"] = @(StallLog::intervalP50Ms(stalls, stallCount));
    d[@"clockSec"] = @(clockSec);
    // Display rate (DisplayRate.h): the main display link's measured rate, 0 while it is not ticking.
    d[@"displayHz"] = @(sDisplayLinkTicking.load(std::memory_order_relaxed) ? sDisplayRate.medianHz() : 0.0);
    d[@"maxFramesPerSecond"] = @(sMaxFramesPerSecond.load(std::memory_order_relaxed));
    d[@"displayRateHint"] = @(sDisplayRateHint.load(std::memory_order_relaxed));

    // Loop sequence (SequencePlayer.h) and the background policy (BackgroundPolicy.h), 0.9.0-rc.5.
    const auto relaxed = std::memory_order_relaxed;
    d[@"sequence"] = @{
        @"events": @(sSequence.eventCount()),
        @"loopBeats": @(sSequence.loopBeats()),
        @"playing": @(sSequence.firing.load(relaxed) ? YES : NO),
        @"muted": @(sSequence.muted() ? YES : NO),
        @"activeNotes": @(sSequence.activeNotes.load(relaxed)),
        @"notesFired": @((double)sSequence.notesFired.load(relaxed)),
        @"lateFires": @((double)sSequence.lateFires.load(relaxed)),
        @"dropped": @((double)sSequence.dropped.load(relaxed)),
        @"version": @((double)sSequence.version()),
        @"listening": @(sSequenceLog.listening.load(relaxed) ? YES : NO),
        @"notesLost": @((double)sSequenceLog.lost.load(relaxed)),
    };
    d[@"backgroundState"] = @(bgpolicy::stateName((bgpolicy::State)sBackgroundState.load(relaxed)));
    d[@"idleStopAt"] = @(sIdleStopAt.load(relaxed)); // on the clockSec clock; -1 = none pending
    d[@"backgroundPolicy"] = @{
        @"set": @(sPolicySet.load(relaxed) ? YES : NO),
        @"keepAlive": @(sPolicyKeepAlive.load(relaxed) ? YES : NO),
        @"idleStopSeconds": @(sPolicyIdleStopSeconds.load(relaxed)),
    };
    d[@"idleStops"] = @((double)sIdleStops.load(relaxed));
    d[@"lastIdleStopAt"] = @(sLastIdleStopSec.load(relaxed));
    d[@"backgroundStartError"] = self.backgroundStartError ?: @"";
    d[@"nowPlaying"] = @{
        @"set": @(sNowPlayingSet.load(relaxed) ? YES : NO),
        @"exclusiveSession": @(sSessionExclusive.load(relaxed) ? YES : NO),
        @"remoteCommands": self.enabledRemoteCommandNames ?: @[],
    };

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

// The MIDI input's mark that another source holds the note goes on before the note is queued, so
// an input note-off racing this call never releases it; a note-on that was not queued undoes it.
- (void)noteOn:(int)note velocity:(int)velocity {
    if (!sAcceptingNotes.load(std::memory_order_acquire)) {
        [self wakeFromIdleStopIfNeeded:@"note"];
        return;
    }
    const int marked = std::clamp(note, 0, 127);
    const bool previous = sMidiInput.externalNoteOn(marked);
    if (!sNoteQueue.pushNoteOn(note, velocity)) sMidiInput.restoreExternal(marked, previous);
}

// UITouch.timestamp and CACurrentMediaTime() are both seconds on the mach_absolute_time clock
// (time since boot, not counting sleep); on Apple platforms juce::Time::getHighResolutionTicks()
// is mach_absolute_time(), so convert with JUCE's rate. 0 (or less) = unknown → 0 ticks.
static juce::int64 touchTicksFor(double seconds) {
    return seconds > 0.0 ? juce::Time::secondsToHighResolutionTicks(seconds) : (juce::int64)0;
}

// Delivery leg: touchesBegan entry − UITouch.timestamp. sTouchLatency.delivery has one writer, the
// main thread (JucePadView), so a call from any other thread is not recorded.
static void recordTouchDelivery(double touchTimestamp, double entryTimestamp) {
    if (touchTimestamp <= 0.0 || entryTimestamp <= 0.0 || ![NSThread isMainThread]) return;
    sTouchLatency.delivery.push((entryTimestamp - touchTimestamp) * 1000.0);
}

- (void)noteOn:(int)note velocity:(int)velocity touchTimestamp:(double)touchTimestamp {
    [self noteOn:note velocity:velocity touchTimestamp:touchTimestamp entryTimestamp:0.0];
}

- (void)noteOn:(int)note velocity:(int)velocity touchTimestamp:(double)touchTimestamp
    entryTimestamp:(double)entryTimestamp {
    const juce::int64 touchTicks = touchTicksFor(touchTimestamp);
    const juce::int64 entryTicks = touchTicksFor(entryTimestamp);
    if (sAcceptingNotes.load(std::memory_order_acquire)) {
        const int marked = std::clamp(note, 0, 127);
        const bool previous = sMidiInput.externalNoteOn(marked); // before the push (see noteOn)
        if (!sNoteQueue.pushNoteOn(note, velocity, touchTicks, entryTicks)) sMidiInput.restoreExternal(marked, previous);
    } else {
        [self wakeFromIdleStopIfNeeded:@"note"];
    }
    // After the push, so the handler leg (entry -> push stamp) does not include it.
    recordTouchDelivery(touchTimestamp, entryTimestamp);
}

// Main thread (JucePadView). See the header.
- (void)reportTouchPath:(NSArray<NSDictionary<NSString *, id> *> *)entries {
    if (entries.count > (NSUInteger)kMaxTouchPathEntries)
        entries = [entries subarrayWithRange:NSMakeRange(0, (NSUInteger)kMaxTouchPathEntries)];
    self.touchPathReport = entries;
}

- (void)reportLastTouch:(NSDictionary<NSString *, id> *)touch {
    self.lastTouchReport = touch;
}

// Releases are always recorded, ready or not: a release must reach a voice that is still
// sounding whatever the engine is doing (a mark with nothing to release is harmless).
- (void)noteOff:(int)note {
    sNoteQueue.pushNoteOff(note);
    sMidiInput.externalNoteOff(note);
}
// The MIDI input forgets its holds under its own lock, so none of its note-ons slips past this.
- (void)allNotesOff { sMidiInput.allNotesOff(); }

// ── MIDI byte input ──
// Lock-free for the map (MidiInput.h); the rest takes the input's short spinlock. Any thread,
// before setup and across rebuilds.

- (BOOL)setMidiInputMapActions:(NSArray<NSNumber *> *)actions velocity:(NSArray<NSNumber *> *)velocity {
    if (actions.count != 128 || (velocity != nil && velocity.count != 128)) return NO;
    int a[128], v[128];
    for (NSUInteger i = 0; i < 128; ++i) {
        a[i] = [actions[i] isKindOfClass:[NSNumber class]] ? actions[i].intValue : midiin::InputMap::kPass;
        v[i] = (velocity != nil && [velocity[i] isKindOfClass:[NSNumber class]]) ? velocity[i].intValue : (int)i;
    }
    sMidiInput.map().set(a, v);
    return YES;
}

- (void)setMidiInputEnabled:(BOOL)enabled { sMidiInput.setEnabled(enabled == YES); }

- (int)releaseMidiInputHolds { return (int)sMidiInput.releaseAll(); }

- (double)midiInputClock { return midiin::NoteQueueSink::nowSeconds(); }

- (int)midiInputBytes:(NSArray<NSNumber *> *)bytes format:(int)format arrival:(double)arrivalSeconds {
    uint8_t buffer[512];
    // Never truncated: a cut packet would parse as something else (one call is one packet).
    if (bytes.count > (NSUInteger)sizeof(buffer)) return -1;
    const NSUInteger count = bytes.count;
    for (NSUInteger i = 0; i < count; ++i)
        buffer[i] = [bytes[i] isKindOfClass:[NSNumber class]] ? (uint8_t)(bytes[i].intValue & 0xFF) : 0;
    if (self.idleStopped) {
        // Only what looks like a note-on wakes the engine (not clock or sensing bytes). A BLE-MIDI
        // timestamp byte can look like a status byte: a rare extra wake costs one idle period.
        for (NSUInteger i = 0; i + 2 < count; ++i) {
            if ((buffer[i] & 0xF0) == 0x90 && buffer[i + 1] < 0x80 && buffer[i + 2] > 0 && buffer[i + 2] < 0x80) {
                [self wakeFromIdleStopIfNeeded:@"midi input"];
                break;
            }
        }
    }
    return expojuce_midi_input_bytes(buffer, (size_t)count, format, arrivalSeconds);
}

- (uint64_t)setMidiInputHandler:(void (^)(NSDictionary<NSString *, id> *))handler {
    const uint64_t token = sMidiInputHandlerOwner.claim();
    sMidiInputListening.store(false, std::memory_order_release); // until the new owner's JS listens
    void (^copied)(NSDictionary<NSString *, id> *) = [handler copy];
    dispatch_async(midiInputQueue(), ^{
        if (sMidiInputHandlerOwner.isCurrent(token)) sMidiInputHandler = copied;
    });
    return token;
}

- (void)clearMidiInputHandler:(uint64_t)token {
    if (!sMidiInputHandlerOwner.release(token)) return; // a newer module set its own
    sMidiInputListening.store(false, std::memory_order_release);
    dispatch_async(midiInputQueue(), ^{
        if (sMidiInputHandlerOwner.current() == 0) sMidiInputHandler = nil;
    });
}

- (void)setMidiInputListening:(BOOL)listening token:(uint64_t)token {
    if (sMidiInputHandlerOwner.isCurrent(token)) sMidiInputListening.store(listening == YES, std::memory_order_release);
}

// A new module (launch or reload) starts without a display-rate hint: an old runtime's hint must
// not outlive it when the old module's release comes too late to clear it.
- (uint64_t)claimModule {
    const uint64_t token = sModuleOwner.claim();
    [self setDisplayRateHint:0];
    // Nor the old runtime's loop, background policy, Now Playing or remote commands.
    [self resetRuntimeState];
    return token;
}

- (void)releaseModule:(uint64_t)token {
    if (sModuleOwner.release(token)) {
        // The display-rate hint belongs to the runtime that set it (cleared first: shutdown can
        // take a while on the main thread). So do the loop and the lock-screen state.
        [self setDisplayRateHint:0];
        [self resetRuntimeState];
        [self stopBeatTimer];
        [self shutdown];
        return;
    }
    // A newer module (a reload) owns the engine and may have set it up already: leave it running
    // and only end the notes the old runtime left on.
    [self allNotesOff];
}

+ (void *)midiInputBytesFunction { return (void *)&expojuce_midi_input_bytes; }

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
    [self drumHit:voice velocity:velocity touchTimestamp:touchTimestamp entryTimestamp:0.0];
}

- (void)drumHit:(int)voice velocity:(int)velocity touchTimestamp:(double)touchTimestamp
    entryTimestamp:(double)entryTimestamp {
    const juce::int64 touchTicks = touchTicksFor(touchTimestamp);
    const juce::int64 entryTicks = touchTicksFor(entryTimestamp);
    if (sAcceptingNotes.load(std::memory_order_acquire)) sNoteQueue.pushDrumHit(voice, velocity, touchTicks, entryTicks);
    recordTouchDelivery(touchTimestamp, entryTimestamp); // after the push (see noteOn)
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

- (void)startTransport {
    [self wakeFromIdleStopIfNeeded:@"transport start"];
    withTransport(self, ^(TransportEngine *transport) { transport->start(); });
    [self transportPlayStateChanged];
}
- (void)stopTransport {
    withTransport(self, ^(TransportEngine *transport) { transport->stop(); });
    [self transportPlayStateChanged];
}
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

// ── Loop sequence ──
// setSequence copies, checks and sorts off the audio thread and swaps the whole sequence in
// (SequenceStore). Any thread but the audio thread, before setup and across rebuilds.

- (BOOL)setSequenceData:(const double *)data
                  count:(NSInteger)count
              loopBeats:(double)loopBeats
               fromBeat:(double)fromBeat
                endBeat:(double)endBeat {
    static constexpr NSInteger kStride = 6;
    if (count < 0 || count % kStride != 0 || (count > 0 && data == nullptr)) return NO;
    const NSInteger events = count / kStride;
    if (events > seqplay::kMaxEvents) return NO;
    std::vector<seqplay::Event> parsed((size_t)events);
    for (NSInteger i = 0; i < events; ++i) {
        const double* f = data + i * kStride;
        seqplay::Event& e = parsed[(size_t)i];
        e.start = f[0];
        e.length = f[1];
        // Out-of-range numbers become values sanitizeEvent rejects (255), never a wrapped note.
        e.note = (std::isfinite(f[2]) && f[2] >= 0.0 && f[2] <= 127.0) ? (uint8_t)std::lround(f[2]) : (uint8_t)255;
        e.velocity = std::isfinite(f[3]) ? (uint8_t)std::clamp(std::lround(f[3]), 1L, 127L) : (uint8_t)100;
        e.kind = f[4] == 1.0 ? seqplay::EventKind::Drum : (f[4] == 0.0 ? seqplay::EventKind::Note : (seqplay::EventKind)255);
        e.skipBefore = std::isnan(f[5]) ? -std::numeric_limits<double>::infinity() : f[5];
    }
    const seqplay::SetResult result = sSequence.set(parsed.data(), (int)events, loopBeats, fromBeat, endBeat);
    return result.ok ? YES : NO;
}

- (void)setSequenceMuted:(BOOL)muted { sSequence.setMuted(muted == YES); }

// ── Engine events ──

- (uint64_t)setEngineEventHandler:(void (^)(NSString *, NSDictionary<NSString *, id> *))handler {
    const uint64_t token = sEngineEventOwner.claim();
    // Sequence notes stay off until the new owner's JS asks for them.
    sSequenceLog.listening.store(false, std::memory_order_relaxed);
    setSequenceDrainRunning(false);
    void (^copied)(NSString *, NSDictionary<NSString *, id> *) = [handler copy];
    dispatch_async(engineEventQueue(), ^{
        if (sEngineEventOwner.isCurrent(token)) sEngineEventHandler = copied;
    });
    return token;
}

- (void)clearEngineEventHandler:(uint64_t)token {
    if (!sEngineEventOwner.release(token)) return; // a newer module set its own
    sSequenceLog.listening.store(false, std::memory_order_relaxed);
    setSequenceDrainRunning(false);
    dispatch_async(engineEventQueue(), ^{
        if (sEngineEventOwner.current() == 0) sEngineEventHandler = nil;
    });
}

- (void)setSequenceNotesListening:(BOOL)listening token:(uint64_t)token {
    if (!sEngineEventOwner.isCurrent(token)) return;
    sSequenceLog.listening.store(listening == YES, std::memory_order_relaxed);
    setSequenceDrainRunning(listening == YES);
}

// Any thread: the runtime that set the loop, the policy and the lock-screen state is gone (a new
// module, or the owner's release). The loop stops (its notes are released on the next block).
- (void)resetRuntimeState {
    sSequence.clear();
    sSequence.setMuted(false);
    sPolicySet.store(false, std::memory_order_release);
    sPolicyKeepAlive.store(true, std::memory_order_relaxed);
    sPolicyIdleStopSeconds.store(bgpolicy::kDefaultIdleStopSeconds, std::memory_order_relaxed);
    __weak ExpoJuceAudioEngine *weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        ExpoJuceAudioEngine *strongSelf = weakSelf;
        if (strongSelf == nil) return;
        [strongSelf stopBackgroundCheck];
        [strongSelf applyRemoteCommands:[NSSet set]];
        strongSelf.nowPlayingInfo = nil;
        sNowPlayingSet.store(false, std::memory_order_relaxed);
        [strongSelf updateNowPlayingInfo];
        [strongSelf updateSessionExclusivity];
    });
}

// ── Background policy ──
// Main thread. sIdleTracker decides (BackgroundPolicy.h); a 1 s timer samples what the engine is
// doing while the app is in the background with a policy set.

- (void)setBackgroundPolicyKeepAlive:(BOOL)keepAlive idleStopSeconds:(double)idleStopSeconds {
    sPolicyKeepAlive.store(keepAlive == YES, std::memory_order_relaxed);
    sPolicyIdleStopSeconds.store(bgpolicy::sanitizeIdleStopSeconds(idleStopSeconds), std::memory_order_relaxed);
    sPolicySet.store(true, std::memory_order_release);
    __weak ExpoJuceAudioEngine *weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        ExpoJuceAudioEngine *strongSelf = weakSelf;
        if (strongSelf == nil || !sInBackground.load(std::memory_order_relaxed)) return;
        [strongSelf startBackgroundCheck]; // set while already in the background
    });
}

- (void)publishBackgroundState {
    sBackgroundState.store((int)sIdleTracker.state(), std::memory_order_relaxed);
    sIdleStopAt.store(sIdleTracker.idleStopAt(), std::memory_order_relaxed);
}

- (void)handleAppDidEnterBackground:(NSNotification *)notification {
    sInBackground.store(true, std::memory_order_relaxed);
    const bgpolicy::Policy policy = currentBackgroundPolicy();
    const bool pause = sIdleTracker.enterBackground(policy, CACurrentMediaTime());
    [self publishBackgroundState];
    if (!self.isReady) return;
    if (pause) {
        // keepAlive off: what plays stops when the app leaves the screen.
        const BOOL wasPlaying = [self isTransportPlaying];
        [self stopTransport];
        [self allNotesOff];
        if (wasPlaying) emitEngineEvent(@"onTransportChange", @{ @"playing": @NO, @"reason": @"background" });
    }
    [self updateSessionExclusivity];
    if (policy.set) [self startBackgroundCheck];
    emitEngineEvent(@"onBackgroundState", @{ @"state": @(bgpolicy::stateName(sIdleTracker.state())) });
}

- (void)handleAppWillEnterForeground:(NSNotification *)notification {
    sInBackground.store(false, std::memory_order_relaxed);
    [self stopBackgroundCheck];
    const bool restart = sIdleTracker.enterForeground();
    [self publishBackgroundState];
    if (restart) [self restartAfterIdleStop:@"foreground"];
    [self updateSessionExclusivity];
    emitEngineEvent(@"onBackgroundState", @{ @"state": @(bgpolicy::stateName(sIdleTracker.state())) });
}

- (void)startBackgroundCheck {
    if (self.backgroundCheckTimer != nil) return;
    dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
    dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), NSEC_PER_SEC, NSEC_PER_SEC / 4);
    __weak ExpoJuceAudioEngine *weakSelf = self;
    dispatch_source_set_event_handler(timer, ^{
        [weakSelf backgroundCheckTick];
    });
    dispatch_resume(timer);
    self.backgroundCheckTimer = timer;
    [self backgroundCheckTick];
}

- (void)stopBackgroundCheck {
    if (self.backgroundCheckTimer == nil) return;
    dispatch_source_cancel(self.backgroundCheckTimer);
    self.backgroundCheckTimer = nil;
}

// Main thread, every second in the background: busy pushes the idle stop back; idle for the
// policy's seconds stops the device.
- (void)backgroundCheckTick {
    if (!sInBackground.load(std::memory_order_relaxed) || self.idleStopped) {
        [self stopBackgroundCheck];
        return;
    }
    if (self.isRecovering || self.rebuildRetryPending) return; // a rebuild is running; check again later
    bgpolicy::Activity activity;
    {
        std::unique_lock<std::mutex> lock(sEngineLock, std::defer_lock);
        if (!acquireUserLock(lock)) return;
        if (!self.isReady) {
            lock.unlock();
            [self stopBackgroundCheck];
            return;
        }
        activity.transportPlaying = sTransport != nullptr && sTransport->isPlaying();
        if (sSynth != nullptr) {
            activity.voicesSounding = sSynth->diag.activeVoices.load(std::memory_order_relaxed)
                + sSynth->drums().diag.activeVoices.load(std::memory_order_relaxed);
        }
    }
    activity.sequenceNotes = sSequence.activeNotes.load(std::memory_order_relaxed);
    activity.inputHolds = sMidiInput.stats().held;
    activity.arpRunning = sAnalysis.arpActive();
    const bool stop = sIdleTracker.check(currentBackgroundPolicy(), activity, CACurrentMediaTime());
    [self publishBackgroundState];
    if (stop) [self idleStop];
}

// Main thread: nothing has played for the policy's seconds in the background. Close the device
// and deactivate the session so iOS can suspend the app. Note-ons are refused until the restart
// (none can pile up and play at once afterwards).
- (void)idleStop {
    [self stopBackgroundCheck];
    if (!self.isReady || self.idleStopped) return;
    NSLog(@"[ExpoJuceAudioEngine] idle in the background: stopping the audio device");
    self.idleStopped = YES;
    sAcceptingNotes.store(false, std::memory_order_release);
    {
        LifecycleLock lifecycle;
        carryXrunsLocked();
        if (sDeviceManager != nullptr) sDeviceManager->closeAudioDevice();
    }
    NSError *error = nil;
    [[AVAudioSession sharedInstance] setActive:NO
                                   withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation
                                         error:&error];
    if (error) [self noteRecoveryError:[NSString stringWithFormat:@"idle stop setActive:NO: %@", error.localizedDescription]];
    sIdleStops.fetch_add(1, std::memory_order_relaxed);
    sLastIdleStopSec.store(CACurrentMediaTime(), std::memory_order_relaxed);
    sSessionExclusive.store(false, std::memory_order_relaxed); // reapplied on the restart
    [self updateNowPlayingInfo];
    emitEngineEvent(@"onBackgroundState", @{ @"state": @"idleStopped" });
}

// Main thread: reactivates the session and reopens the device after an idle stop (the existing
// recovery path: reopen, or rebuild if it will not run). From the background (a remote play) iOS
// may refuse the session: then the engine stays stopped and backgroundStartError says why.
- (BOOL)restartAfterIdleStop:(NSString *)context {
    if (!self.idleStopped) return YES;
    // Mixable to start: iOS refuses a non-mixable session started from the background more often.
    // updateSessionExclusivity below makes it exclusive again when Now Playing needs it.
    AVAudioSession *session = [AVAudioSession sharedInstance];
    NSError *categoryError = nil;
    NSError *activationError = nil;
    [session setCategory:AVAudioSessionCategoryPlayback withOptions:AVAudioSessionCategoryOptionMixWithOthers error:&categoryError];
    const BOOL active = [session setActive:YES error:&activationError];
    if (!active && sInBackground.load(std::memory_order_relaxed)) {
        self.backgroundStartError = [NSString stringWithFormat:@"%@: setActive: %@", context,
                                     activationError.localizedDescription ?: @"refused"];
        NSLog(@"[ExpoJuceAudioEngine] restart after idle stop refused (%@)", self.backgroundStartError);
        return NO;
    }
    sSessionExclusive.store(false, std::memory_order_relaxed);
    self.backgroundStartError = nil;
    self.idleStopped = NO;
    NSLog(@"[ExpoJuceAudioEngine] restarting the audio device after an idle stop (%@)", context);
    if (self.rebuildAfterIdleStop) {
        self.rebuildAfterIdleStop = NO;
        [self rebuildEngine:[NSString stringWithFormat:@"media services reset (%@)", context]];
    } else {
        [self restartDeviceIfStopped:[NSString stringWithFormat:@"idle stop (%@)", context]];
    }
    if (self.isReady) sAcceptingNotes.store(true, std::memory_order_release);
    [self scheduleDeadDeviceCheck:[NSString stringWithFormat:@"idle stop (%@)", context]];
    if (sInBackground.load(std::memory_order_relaxed)) {
        // Still in the background (a remote play, a key on a MIDI device): the idle timer starts over.
        sIdleTracker.markRestarted(CACurrentMediaTime());
        [self publishBackgroundState];
        if (sPolicySet.load(std::memory_order_acquire)) [self startBackgroundCheck];
        emitEngineEvent(@"onBackgroundState", @{ @"state": @(bgpolicy::stateName(sIdleTracker.state())) });
    }
    [self updateNowPlayingInfo];
    [self updateSessionExclusivity];
    return [self isDeviceRunning];
}

// Any thread: something wants sound while the engine is idle-stopped in the background (a note-on
// from JS or a MIDI device, a transport start). The device restarts on the main thread (best effort:
// iOS may refuse); what asked is not replayed, so the first note after an idle stop can be lost.
- (void)wakeFromIdleStopIfNeeded:(NSString *)context {
    if (!self.idleStopped) return;
    static std::atomic<bool> pending{false};
    if (pending.exchange(true, std::memory_order_acq_rel)) return;
    __weak ExpoJuceAudioEngine *weakSelf = self;
    NSString *what = [context copy];
    dispatch_async(dispatch_get_main_queue(), ^{
        pending.store(false, std::memory_order_release);
        ExpoJuceAudioEngine *strongSelf = weakSelf;
        if (strongSelf == nil || !strongSelf.idleStopped) return;
        [strongSelf restartAfterIdleStop:what];
    });
}

// ── Now Playing and remote commands ──
// Main thread for everything MediaPlayer. The session is made non-mixable only while Now Playing
// info is set and the transport plays or the app is in the background (bgpolicy::
// wantsExclusiveSession): iOS shows the info and routes the lock-screen controls only to an app
// whose session does not mix with others.

- (AVAudioSessionCategoryOptions)sessionCategoryOptions {
    return sSessionExclusive.load(std::memory_order_relaxed) ? (AVAudioSessionCategoryOptions)0
                                                             : AVAudioSessionCategoryOptionMixWithOthers;
}

- (BOOL)setNowPlaying:(NSDictionary<NSString *, id> *)info {
    NSDictionary<NSString *, id> *copied = nil;
    if (info != nil && ![info isKindOfClass:[NSNull class]]) {
        id title = info[@"title"];
        if (![title isKindOfClass:[NSString class]] || [(NSString *)title length] == 0) return NO;
        NSMutableDictionary<NSString *, id> *clean = [NSMutableDictionary dictionaryWithObject:title forKey:@"title"];
        for (NSString *key in @[ @"artist", @"album" ]) {
            id value = info[key];
            if ([value isKindOfClass:[NSString class]] && [(NSString *)value length] > 0) clean[key] = value;
        }
        copied = [clean copy];
    }
    sNowPlayingSet.store(copied != nil, std::memory_order_relaxed);
    __weak ExpoJuceAudioEngine *weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        ExpoJuceAudioEngine *strongSelf = weakSelf;
        if (strongSelf == nil) return;
        strongSelf.nowPlayingInfo = copied;
        [strongSelf updateNowPlayingInfo];
        [strongSelf updateSessionExclusivity];
    });
    return YES;
}

- (BOOL)enableRemoteCommands:(NSArray<NSString *> *)commands {
    NSSet<NSString *> *known = [NSSet setWithArray:@[ @"play", @"pause", @"togglePlayPause" ]];
    NSMutableSet<NSString *> *wanted = [NSMutableSet set];
    for (id name in commands ?: @[]) {
        if (![name isKindOfClass:[NSString class]] || ![known containsObject:name]) return NO;
        [wanted addObject:name];
    }
    __weak ExpoJuceAudioEngine *weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        [weakSelf applyRemoteCommands:wanted];
    });
    return YES;
}

static MPRemoteCommand *remoteCommandNamed(NSString *name) {
    MPRemoteCommandCenter *center = [MPRemoteCommandCenter sharedCommandCenter];
    if ([name isEqualToString:@"play"]) return center.playCommand;
    if ([name isEqualToString:@"pause"]) return center.pauseCommand;
    return center.togglePlayPauseCommand;
}

// Main thread: replaces the handlers (removes this engine's old targets first).
- (void)applyRemoteCommands:(NSSet<NSString *> *)wanted {
    for (NSString *name in @[ @"play", @"pause", @"togglePlayPause" ]) {
        MPRemoteCommand *command = remoteCommandNamed(name);
        id existing = self.remoteCommandTargets[name];
        if (existing != nil) {
            [command removeTarget:existing];
            [self.remoteCommandTargets removeObjectForKey:name];
        }
        if (![wanted containsObject:name]) {
            command.enabled = NO;
            continue;
        }
        __weak ExpoJuceAudioEngine *weakSelf = self;
        NSString *commandName = [name copy];
        id target = [command addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent *event) {
            ExpoJuceAudioEngine *strongSelf = weakSelf;
            if (strongSelf == nil) return MPRemoteCommandHandlerStatusCommandFailed;
            return [strongSelf handleRemoteCommand:commandName];
        }];
        if (target != nil) self.remoteCommandTargets[name] = target;
        command.enabled = YES;
    }
    self.enabledRemoteCommandNames = [[wanted allObjects] sortedArrayUsingSelector:@selector(compare:)];
}

// Main thread (MPRemoteCommandCenter calls its handlers there): the native transport, then the
// app's JS is told so its transport state follows.
- (MPRemoteCommandHandlerStatus)handleRemoteCommand:(NSString *)name {
    if (!self.isReady) return MPRemoteCommandHandlerStatusCommandFailed;
    const BOOL playing = [self isTransportPlaying];
    const BOOL play = [name isEqualToString:@"play"] || ([name isEqualToString:@"togglePlayPause"] && !playing);
    if (play) {
        if (self.idleStopped && ![self restartAfterIdleStop:@"remote play"]) {
            emitEngineEvent(@"onRemoteCommand", @{ @"command": name, @"playing": @NO, @"failed": @YES });
            return MPRemoteCommandHandlerStatusCommandFailed;
        }
        if (!playing) [self startTransport];
    } else if (playing) {
        [self stopTransport];
    }
    emitEngineEvent(@"onRemoteCommand", @{ @"command": name, @"playing": @(play) });
    if (play != playing) emitEngineEvent(@"onTransportChange", @{ @"playing": @(play), @"reason": @"remoteCommand" });
    return MPRemoteCommandHandlerStatusSuccess;
}

// Any thread: the transport started or stopped; the Now Playing rate and the session follow.
- (void)transportPlayStateChanged {
    if (!sNowPlayingSet.load(std::memory_order_relaxed) && !sSessionExclusive.load(std::memory_order_relaxed)) return;
    if ([NSThread isMainThread]) {
        [self updateNowPlayingInfo];
        [self updateSessionExclusivity];
        return;
    }
    __weak ExpoJuceAudioEngine *weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        ExpoJuceAudioEngine *strongSelf = weakSelf;
        [strongSelf updateNowPlayingInfo];
        [strongSelf updateSessionExclusivity];
    });
}

// Main thread.
- (void)updateNowPlayingInfo {
    MPNowPlayingInfoCenter *center = [MPNowPlayingInfoCenter defaultCenter];
    NSDictionary<NSString *, id> *info = self.nowPlayingInfo;
    if (info == nil) {
        center.nowPlayingInfo = nil;
        return;
    }
    const BOOL playing = self.isReady && !self.idleStopped && [self isTransportPlaying];
    NSMutableDictionary<NSString *, id> *now = [NSMutableDictionary dictionary];
    now[MPMediaItemPropertyTitle] = info[@"title"];
    if (info[@"artist"] != nil) now[MPMediaItemPropertyArtist] = info[@"artist"];
    if (info[@"album"] != nil) now[MPMediaItemPropertyAlbumTitle] = info[@"album"];
    now[MPNowPlayingInfoPropertyPlaybackRate] = @(playing ? 1.0 : 0.0);
    now[MPNowPlayingInfoPropertyIsLiveStream] = @YES; // a loop: no duration, no scrubbing
    now[MPNowPlayingInfoPropertyMediaType] = @(MPNowPlayingInfoMediaTypeAudio);
    center.nowPlayingInfo = now;
}

// Main thread: the session mixes with other audio unless Now Playing needs it not to.
- (void)updateSessionExclusivity {
    if (!self.isReady || self.idleStopped) return;
    const bool want = bgpolicy::wantsExclusiveSession(self.nowPlayingInfo != nil, [self isTransportPlaying] == YES,
                                                      sInBackground.load(std::memory_order_relaxed));
    if (want == sSessionExclusive.load(std::memory_order_relaxed)) return;
    sSessionExclusive.store(want, std::memory_order_relaxed);
    AVAudioSession *session = [AVAudioSession sharedInstance];
    NSError *categoryError = nil;
    [session setCategory:AVAudioSessionCategoryPlayback withOptions:[self sessionCategoryOptions] error:&categoryError];
    if (categoryError) {
        [self noteRecoveryError:[NSString stringWithFormat:@"session %@: setCategory: %@", want ? @"exclusive" : @"mixable",
                                 categoryError.localizedDescription]];
    }
    if (want) {
        NSError *activationError = nil;
        [session setActive:YES error:&activationError];
        if (activationError) {
            [self noteRecoveryError:[NSString stringWithFormat:@"session exclusive: setActive: %@",
                                     activationError.localizedDescription]];
        }
    }
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
