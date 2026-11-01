#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

// beat: fractional transport position at the end of the block that crossed a beat;
// position: fractional transport position when the event is delivered.
typedef void(^ExpoJuceBeatBlock)(double beat, double bpm, double position);

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

// ── Effects ──────────────────────────────────────────────────────
// Lock-free, any thread; remembered across setup / rebuilds. NO for an unknown effect or
// parameter, or a non-finite value. Swift: `setEffectEnabled(_:enabled:)`,
// `setEffectParam(_:param:value:)`, `getEffectsState()`.
+ (BOOL)setEffectEnabled:(NSString *)effectId enabled:(BOOL)enabled NS_SWIFT_NAME(setEffectEnabled(_:enabled:));
+ (BOOL)setEffectParam:(NSString *)effectId param:(NSString *)param value:(double)value NS_SWIFT_NAME(setEffectParam(_:param:value:));
+ (NSDictionary<NSString *, id> *)getEffectsState NS_SWIFT_NAME(getEffectsState());

// ── Metronome / backing track / analysis ─────────────────────────
+ (BOOL)setMetronome:(NSDictionary<NSString *, id> *)options NS_SWIFT_NAME(setMetronome(_:)); // NO: unknown style
+ (void)setBackingTrackOptions:(NSDictionary<NSString *, id> *)options NS_SWIFT_NAME(setBackingTrackOptions(_:));
// `completion` runs on a background queue: result, or errorCode + errorMessage.
+ (void)loadBackingTrack:(NSString *)uri
                 gapless:(nullable NSDictionary<NSString *, id> *)gapless
              completion:(void (^)(NSDictionary<NSString *, id> *_Nullable result, NSString *_Nullable errorCode,
                                   NSString *_Nullable errorMessage))completion
    NS_SWIFT_NAME(loadBackingTrack(_:gapless:completion:));
+ (void)unloadBackingTrack;
+ (NSDictionary<NSString *, id> *)getAnalysis NS_SWIFT_NAME(getAnalysis());

// ── Arpeggiator (0.7.0+) ─────────────────────────────────────────
// Lock-free, any thread; remembered before setup and across rebuilds. NO (nothing applied): unknown
// mode / rate, pattern not 1–16 booleans. Swift: `setArpeggiator(_:)`, `getArpeggiatorState()`.
+ (BOOL)setArpeggiator:(NSDictionary<NSString *, id> *)options NS_SWIFT_NAME(setArpeggiator(_:));
+ (NSDictionary<NSString *, id> *)getArpeggiatorState NS_SWIFT_NAME(getArpeggiatorState());

// ── Drums (0.8.0+) ───────────────────────────────────────────────
// drumHit: voice 0–7 (others ignored), velocity clamped to 1–127; lock-free, any thread.
// Parameters: lock-free, remembered before setup and across rebuilds; unknown keys ignored, values
// clamped. setDrumVoice returns NO for a voice outside 0–7. Swift: `drumHit(_:velocity:)`,
// `padDrumHit(_:velocity:touchTimestamp:)`, `setDrumVoice(_:params:)`, `setDrumKit(_:)`, `getDrumState()`.
+ (void)drumHit:(int)voice velocity:(int)velocity NS_SWIFT_NAME(drumHit(_:velocity:));
+ (void)padDrumHit:(int)voice velocity:(int)velocity touchTimestamp:(double)touchTimestamp NS_SWIFT_NAME(padDrumHit(_:velocity:touchTimestamp:));
+ (BOOL)setDrumVoice:(int)voice params:(NSDictionary<NSString *, id> *)params NS_SWIFT_NAME(setDrumVoice(_:params:));
+ (BOOL)setDrumKit:(NSDictionary<NSString *, id> *)params NS_SWIFT_NAME(setDrumKit(_:));
+ (NSDictionary<NSString *, id> *)getDrumState NS_SWIFT_NAME(getDrumState());

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
