#import "ExpoJuceTouchVelocity.h"
#import <CoreMotion/CoreMotion.h>
#import <QuartzCore/QuartzCore.h>
#import <UIKit/UIKit.h>
#import <os/lock.h>
#include "TouchVelocity.h"
#include <atomic>
#include <cmath>
#include <limits>
#include <vector>

// Requested accelerometer rate: 100 Hz is the practical iPhone ceiling.
static const double kRequestedHz = 100.0;
// Touches read per touch-down (more fingers landing in one call than this are ignored).
static const int kMaxTouches = 8;
// impactLate: filled this long after the touch (its window is 50 ms); given up after kLateGiveUpSec
// (the ring holds 2.56 s).
static const double kLateFillDelaySec = 0.08;
static const double kLateGiveUpSec = 2.0;

// ── Shared state ──────────────────────────────────────────────────────────────

// Written by the motion queue (serial), read on the main thread and by diagnostics. Lock-free.
static touchvel::AccelRing sRing;
static std::atomic<bool> sAccelRunning{false};
static std::atomic<bool> sAccelAvailable{false};
static std::atomic<bool> sForceCapable{false};
static std::atomic<int> sPadCount{0};

// Main thread only: what the touch handler reads (no lock on the note path).
struct MainState {
    bool enabled = false;
    touchvel::Settings settings;
    int pads = 0;
    bool background = false;
    bool observing = false;
    bool lateScheduled = false;
    touchvel::Estimate last;
    double lastTouchT = 0.0;
    bool hasLast = false;
    bool lastRecorded = false; // pushed to sRecent (after the note, by lastTouchSignals)
};
static MainState sMain;
static CMMotionManager *sMotion = nil;
static NSOperationQueue *sMotionQueue = nil;
static touchvel::AccelSample sScan[touchvel::AccelRing::kSize]; // main thread: touch-down scan

// The configuration as set (for diagnostics; written by setConfig on any thread).
struct ReportedConfig {
    bool enabled = false;
    touchvel::Settings settings;
    bool calibrationSet = false;
};
static os_unfair_lock sConfigLock = OS_UNFAIR_LOCK_INIT;
static ReportedConfig sReported;

// Recent estimates: the main thread pushes (after the note, never on the note path), diagnostics
// read; both fill impactLate. Held only to copy records in or out, never across a ring scan.
static os_unfair_lock sRecordLock = OS_UNFAIR_LOCK_INIT;
static touchvel::RecentLog sRecent;
static touchvel::AccelSample sLateScanMain[touchvel::AccelRing::kSize]; // main thread: impactLate scans

// ── Helpers ───────────────────────────────────────────────────────────────────

static void onMain(dispatch_block_t block) {
    if ([NSThread isMainThread]) {
        block();
    } else {
        dispatch_async(dispatch_get_main_queue(), block);
    }
}

static double numberOr(NSDictionary *dict, NSString *key, double fallback) {
    id value = dict[key];
    if (![value isKindOfClass:[NSNumber class]]) return fallback;
    const double v = [(NSNumber *)value doubleValue];
    return std::isfinite(v) ? v : fallback;
}

static NSString *curveName(touchvel::Curve curve) {
    switch (curve) {
        case touchvel::Curve::Soft: return @"soft";
        case touchvel::Curve::Hard: return @"hard";
        case touchvel::Curve::Normal: break;
    }
    return @"normal";
}

static id numberOrNull(bool valid, double value) {
    return valid && std::isfinite(value) ? (id)@(value) : (id)[NSNull null];
}

// onNoteOn `touch` (TouchSignals).
static NSMutableDictionary<NSString *, id> *signalsDictionary(const touchvel::Estimate &e) {
    NSMutableDictionary<NSString *, id> *d = [NSMutableDictionary dictionaryWithCapacity:12];
    d[@"radius"] = @(e.touch.radius);
    d[@"radiusTolerance"] = @(e.touch.radiusTolerance);
    d[@"force"] = @(e.touch.force);
    d[@"maxForce"] = @(e.touch.maxForce);
    d[@"impact"] = numberOrNull(e.impact.valid, e.impact.peak);
    d[@"accelAgeMs"] = numberOrNull(e.impact.hasSamples, e.impact.ageSec * 1000.0);
    d[@"strength"] = @(e.strength);
    return d;
}

static NSDictionary<NSString *, id> *calibrationDictionary(const touchvel::Calibration &c) {
    return @{
        @"radiusSoft": @(c.radiusSoft),
        @"radiusHard": @(c.radiusHard),
        @"impactSoft": @(c.impactSoft),
        @"impactHard": @(c.impactHard),
        @"radiusWeight": @(c.radiusWeight),
        @"impactWeight": @(c.impactWeight),
    };
}

// Fills impactLate of the records whose window has closed. sRecordLock is held only to copy the
// waiting records out and to write the results back; the ring scans run without it. buf: the
// caller's scan buffer (AccelRing::kSize samples, not shared with another thread). Returns whether
// any record still waits.
static bool fillLate(double now, touchvel::AccelSample *buf) {
    const int kMax = touchvel::RecentLog::kSize;
    uint64_t seqs[kMax];
    double touchTs[kMax];
    int count = 0;
    os_unfair_lock_lock(&sRecordLock);
    for (int i = 0; i < sRecent.size() && count < kMax; ++i) {
        const touchvel::Record &r = sRecent.at(i);
        if (r.lateDone) continue;
        seqs[count] = r.seq;
        touchTs[count] = r.touchT;
        ++count;
    }
    os_unfair_lock_unlock(&sRecordLock);
    if (count == 0) return false;

    touchvel::Impact results[kMax];
    bool done[kMax];
    bool waiting = false;
    for (int i = 0; i < count; ++i) {
        done[i] = true;
        if (now - touchTs[i] > kLateGiveUpSec) {
            results[i] = touchvel::Impact(); // the ring no longer covers it
            continue;
        }
        const int n = sRing.readSince(touchvel::scanFrom(touchTs[i]), buf, touchvel::AccelRing::kSize);
        results[i] = touchvel::computeImpactLate(buf, n, touchTs[i]);
        if (results[i].pending) {
            done[i] = false;
            waiting = true;
        }
    }

    os_unfair_lock_lock(&sRecordLock);
    for (int i = 0; i < count; ++i) {
        if (!done[i]) continue;
        touchvel::Record *r = sRecent.find(seqs[i]);
        if (r != nullptr && !r->lateDone) {
            r->late = results[i];
            r->lateDone = true;
        }
    }
    os_unfair_lock_unlock(&sRecordLock);
    return waiting;
}

// Main thread: fill impactLate shortly after a touch (off the note path), again while any waits.
static void scheduleLateFill() {
    if (sMain.lateScheduled) return;
    sMain.lateScheduled = true;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(kLateFillDelaySec * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        sMain.lateScheduled = false;
        if (fillLate(CACurrentMediaTime(), sLateScanMain)) scheduleLateFill();
    });
}

// ── Accelerometer lifecycle (main thread) ─────────────────────────────────────

static void updateMotion() {
    const bool want = sMain.enabled && sMain.pads > 0 && !sMain.background;
    const bool running = sAccelRunning.load();
    if (want == running) return;
    @try {
        if (want) {
            if (sMotion == nil) {
                sMotion = [[CMMotionManager alloc] init];
                sMotionQueue = [[NSOperationQueue alloc] init];
                sMotionQueue.maxConcurrentOperationCount = 1; // serial: the ring's single writer
                sMotionQueue.name = @"expojuce.touchvelocity.motion";
                sMotionQueue.qualityOfService = NSQualityOfServiceUserInteractive;
            }
            const bool available = sMotion.accelerometerAvailable ? true : false;
            sAccelAvailable.store(available);
            if (!available) return; // simulator: impact stays invalid, the radius plays alone
            sMotion.accelerometerUpdateInterval = 1.0 / kRequestedHz;
            [sMotion startAccelerometerUpdatesToQueue:sMotionQueue
                                          withHandler:^(CMAccelerometerData *data, NSError *error) {
                if (data == nil) return;
                const CMAcceleration a = data.acceleration;
                sRing.push(data.timestamp, a.x, a.y, a.z);
            }];
            sAccelRunning.store(true);
        } else {
            [sMotion stopAccelerometerUpdates];
            sAccelRunning.store(false);
        }
    } @catch (NSException *exception) {
        NSLog(@"[ExpoJuce] touch velocity: accelerometer %@ failed: %@", want ? @"start" : @"stop", exception);
        sAccelRunning.store(false);
    }
}

static void ensureObserving() {
    if (sMain.observing) return;
    sMain.observing = true;
    sMain.background = [UIApplication sharedApplication].applicationState == UIApplicationStateBackground;
    NSNotificationCenter *center = [NSNotificationCenter defaultCenter];
    NSOperationQueue *mainQueue = [NSOperationQueue mainQueue];
    [center addObserverForName:UIApplicationDidEnterBackgroundNotification object:nil queue:mainQueue
                    usingBlock:^(NSNotification *note) {
        sMain.background = true;
        updateMotion();
    }];
    [center addObserverForName:UIApplicationWillEnterForegroundNotification object:nil queue:mainQueue
                    usingBlock:^(NSNotification *note) {
        sMain.background = false;
        updateMotion();
    }];
}

// ── API ───────────────────────────────────────────────────────────────────────

@implementation ExpoJuceTouchVelocity

+ (BOOL)setConfig:(NSDictionary<NSString *, id> *)config {
    NSDictionary *c = [config isKindOfClass:[NSDictionary class]] ? config : @{};
    ReportedConfig r;
    id enabled = c[@"enabled"];
    r.enabled = [enabled isKindOfClass:[NSNumber class]] && [(NSNumber *)enabled boolValue];
    id curve = c[@"curve"];
    if ([curve isKindOfClass:[NSString class]]) {
        if ([curve isEqualToString:@"soft"]) r.settings.curve = touchvel::Curve::Soft;
        else if ([curve isEqualToString:@"hard"]) r.settings.curve = touchvel::Curve::Hard;
    }
    const touchvel::Settings defaults;
    r.settings.minVelocity = touchvel::sanitizeVelocity(numberOr(c, @"minVelocity", NAN), defaults.minVelocity);
    r.settings.maxVelocity = touchvel::sanitizeVelocity(numberOr(c, @"maxVelocity", NAN), defaults.maxVelocity);
    touchvel::orderVelocityBounds(r.settings);
    id calibration = c[@"calibration"];
    if ([calibration isKindOfClass:[NSDictionary class]]) {
        NSDictionary *k = calibration;
        touchvel::Calibration in;
        in.radiusSoft = numberOr(k, @"radiusSoft", NAN);
        in.radiusHard = numberOr(k, @"radiusHard", NAN);
        in.impactSoft = numberOr(k, @"impactSoft", NAN);
        in.impactHard = numberOr(k, @"impactHard", NAN);
        in.radiusWeight = numberOr(k, @"radiusWeight", NAN);
        in.impactWeight = numberOr(k, @"impactWeight", NAN);
        r.settings.calibration = touchvel::sanitizeCalibration(in);
        r.calibrationSet = true;
    }
    os_unfair_lock_lock(&sConfigLock);
    sReported = r;
    os_unfair_lock_unlock(&sConfigLock);
    const bool on = r.enabled;
    const touchvel::Settings settings = r.settings;
    onMain(^{
        sMain.enabled = on;
        sMain.settings = settings;
        ensureObserving();
        updateMotion();
    });
    return YES;
}

+ (void)padAttachedForceTouchCapable:(BOOL)forceTouchCapable {
    onMain(^{
        sMain.pads += 1;
        sPadCount.store(sMain.pads);
        if (forceTouchCapable) sForceCapable.store(true);
        ensureObserving();
        updateMotion();
    });
}

+ (void)padDetached {
    onMain(^{
        sMain.pads = std::max(0, sMain.pads - 1);
        sPadCount.store(sMain.pads);
        updateMotion();
    });
}

+ (int)estimateVelocityForTouches:(NSSet<UITouch *> *)touches
                   touchTimestamp:(double)touchTimestamp
                   entryTimestamp:(double)entryTimestamp {
    if (!sMain.enabled) return 0;
    touchvel::TouchInput inputs[kMaxTouches];
    int count = 0;
    for (UITouch *touch in touches) {
        if (count >= kMaxTouches) break;
        touchvel::TouchInput &in = inputs[count++];
        in.radius = (double)touch.majorRadius;
        in.radiusTolerance = (double)touch.majorRadiusTolerance;
        in.force = (double)touch.force;
        in.maxForce = (double)touch.maximumPossibleForce;
    }
    const int cap = touchvel::AccelRing::kSize;
    int n = sRing.readSince(touchvel::scanFrom(touchTimestamp), sScan, cap);
    if (n == 0) n = sRing.readSince(-std::numeric_limits<double>::infinity(), sScan, 1); // its age only
    const touchvel::Impact impact = touchvel::computeImpact(sScan, n, touchTimestamp, entryTimestamp);
    const touchvel::Estimate e = touchvel::estimate(inputs, count, impact, sMain.settings);
    // No lock here: the record for diagnostics is pushed after the note, by lastTouchSignals.
    sMain.last = e;
    sMain.lastTouchT = touchTimestamp;
    sMain.hasLast = true;
    sMain.lastRecorded = false;
    return e.velocity;
}

+ (NSDictionary<NSString *, id> *)lastTouchSignals {
    if (!sMain.hasLast) return @{};
    if (!sMain.lastRecorded) {
        sMain.lastRecorded = true;
        if (sMain.last.touch.maxForce > 0.0) sForceCapable.store(true);
        touchvel::Record record;
        record.estimate = sMain.last;
        record.touchT = sMain.lastTouchT;
        os_unfair_lock_lock(&sRecordLock);
        sRecent.push(record);
        os_unfair_lock_unlock(&sRecordLock);
        scheduleLateFill();
    }
    return signalsDictionary(sMain.last);
}

+ (NSDictionary<NSString *, id> *)diagnostics {
    const double now = CACurrentMediaTime();
    os_unfair_lock_lock(&sConfigLock);
    const ReportedConfig config = sReported;
    os_unfair_lock_unlock(&sConfigLock);

    std::vector<touchvel::AccelSample> recent((size_t)touchvel::AccelRing::kSize);
    const int n = sRing.readSince(now - 1.0, recent.data(), touchvel::AccelRing::kSize);
    const double hz = touchvel::measuredHz(recent.data(), n, now);

    // (The 1 s window above is a prefix of what impactLate needs; scan again with the full buffer.)
    fillLate(now, recent.data());
    std::vector<touchvel::Record> records;
    records.reserve(touchvel::RecentLog::kSize);
    os_unfair_lock_lock(&sRecordLock);
    for (int i = 0; i < sRecent.size(); ++i) records.push_back(sRecent.at(i));
    os_unfair_lock_unlock(&sRecordLock);

    NSMutableArray<NSDictionary<NSString *, id> *> *list = [NSMutableArray arrayWithCapacity:records.size()];
    for (const touchvel::Record &r : records) {
        NSMutableDictionary<NSString *, id> *d = signalsDictionary(r.estimate);
        d[@"velocity"] = @(r.estimate.velocity);
        d[@"forceUsed"] = @(r.estimate.forceUsed ? YES : NO);
        d[@"impactLate"] = numberOrNull(r.lateDone && r.late.valid, r.late.peak);
        d[@"atSec"] = @(r.touchT);
        [list addObject:d];
    }
    return @{
        @"supported": @YES,
        @"enabled": @(config.enabled ? YES : NO),
        @"curve": curveName(config.settings.curve),
        @"min": @(config.settings.minVelocity),
        @"max": @(config.settings.maxVelocity),
        @"calibration": config.calibrationSet ? (id)calibrationDictionary(config.settings.calibration)
                                              : (id)[NSNull null],
        @"accel": @{
            @"running": @(sAccelRunning.load() ? YES : NO),
            @"available": @(sAccelAvailable.load() ? YES : NO),
            @"requestedHz": @(kRequestedHz),
            @"measuredHz": @(hz),
            @"samples": @(sRing.pushed()),
        },
        @"forceCapable": @(sForceCapable.load() ? YES : NO),
        @"pads": @(sPadCount.load()),
        @"clockSec": @(now),
        @"last": list.count > 0 ? (id)list.lastObject : (id)[NSNull null],
        @"recent": list,
    };
}

@end
