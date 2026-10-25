#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

typedef void(^ExpoJuceBeatBlock)(double beat, double bpm);

@interface ExpoJuceBridge : NSObject

// Returns YES on success (or if already initialized). Swift: `setup() -> Bool`.
+ (BOOL)setup;
// As setup, with a preferred I/O buffer size (samples) and sample rate (<= 0 → defaults 128 / 48000).
// Swift: `setup(bufferSize:sampleRate:) -> Bool`.
+ (BOOL)setupWithBufferSize:(int)bufferSize sampleRate:(double)sampleRate NS_SWIFT_NAME(setup(bufferSize:sampleRate:));
// "step N: <error>" describing the last setup failure. Swift: `setupErrorMessage() -> String`.
+ (NSString *)setupErrorMessage NS_SWIFT_NAME(setupErrorMessage());
+ (void)shutdown;

// ── Diagnostics ──────────────────────────────────────────────────
+ (NSDictionary<NSString *, id> *)getDiagnostics NS_SWIFT_NAME(getDiagnostics());
// Returns NO if not queued (engine not ready / device not playing).
+ (BOOL)playTestTone:(double)durationMs NS_SWIFT_NAME(playTestTone(durationMs:));
+ (void)startBeatTimer;
+ (void)stopBeatTimer;

// ── Synth (MIDI-style) ───────────────────────────────────────────
+ (void)noteOn:(int)note velocity:(int)velocity;
// noteOn from a native touch (JucePadView). touchTimestamp = UITouch.timestamp (seconds since boot).
// Swift: `padNoteOn(_:velocity:touchTimestamp:)`.
+ (void)padNoteOn:(int)note velocity:(int)velocity touchTimestamp:(double)touchTimestamp NS_SWIFT_NAME(padNoteOn(_:velocity:touchTimestamp:));
+ (void)noteOff:(int)note;
+ (void)allNotesOff;

// ── Global Synth Params ──────────────────────────────────────────
+ (void)setWaveform:(NSString *)waveform;
+ (void)setFilterCutoff:(double)hz;
+ (void)setFilterResonance:(double)q;
+ (void)setAttack:(double)ms;
+ (void)setDecay:(double)ms;
+ (void)setSustain:(double)level;
+ (void)setRelease:(double)ms;
+ (void)setDetune:(double)cents;
+ (void)setMasterLevel:(double)level;

// ── Transport ────────────────────────────────────────────────────
+ (void)startTransport;
+ (void)stopTransport;
+ (void)setTempo:(double)bpm;
+ (void)setTransportPosition:(double)beat;
+ (double)getTransportPosition;
+ (double)getTempo;
+ (BOOL)isTransportPlaying;
+ (void)setBeatCallback:(ExpoJuceBeatBlock)callback;

@end

NS_ASSUME_NONNULL_END
