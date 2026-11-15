#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

// Touch velocity for the native pad (JucePadView): turns a touch-down's contact radius, force and
// the device's accelerometer impact into a MIDI velocity (TouchVelocity.h). The accelerometer runs
// only while touch velocity is enabled, at least one pad with touchVelocity is in a window and the
// app is not in the background.
@interface ExpoJuceTouchVelocity : NSObject

// Any thread. Replaces the whole configuration: { enabled, curve?, minVelocity?, maxVelocity?,
// calibration? } (omitted / invalid fields take their defaults; numbers are clamped). Always YES.
+ (BOOL)setConfig:(NSDictionary<NSString *, id> *)config NS_SWIFT_NAME(setConfig(_:));

// Any thread: getDiagnostics().touchVelocity.
+ (NSDictionary<NSString *, id> *)diagnostics NS_SWIFT_NAME(diagnostics());

// Any thread (the work is forwarded to the main thread): a pad with touchVelocity on entered / left
// a window. The accelerometer follows the count. forceTouchCapable: the pad's
// traitCollection.forceTouchCapability is available (diagnostics only).
+ (void)padAttachedForceTouchCapable:(BOOL)forceTouchCapable NS_SWIFT_NAME(padAttached(forceTouchCapable:));
+ (void)padDetached NS_SWIFT_NAME(padDetached());

// Main thread, in touchesBegan before the note: the velocity (1–127) for the strongest of the new
// touches, or 0 when touch velocity is disabled (play the fixed velocity). touchTimestamp: the
// earliest UITouch.timestamp; entryTimestamp: CACurrentMediaTime() at the handler's entry.
+ (int)estimateVelocityForTouches:(NSSet<UITouch *> *)touches
                   touchTimestamp:(double)touchTimestamp
                   entryTimestamp:(double)entryTimestamp
    NS_SWIFT_NAME(estimateVelocity(touches:touchTimestamp:entryTimestamp:));

// Main thread, after the note: the signals of the last estimate (the onNoteOn `touch` payload).
+ (NSDictionary<NSString *, id> *)lastTouchSignals NS_SWIFT_NAME(lastTouchSignals());

@end

NS_ASSUME_NONNULL_END
