#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

typedef void(^ExpoJuceBeatBlock)(double beat, double bpm);

@interface ExpoJuceBridge : NSObject

+ (void)setup;
+ (void)shutdown;
+ (void)startBeatTimer;
+ (void)stopBeatTimer;

// ── Synth (MIDI-style) ───────────────────────────────────────────
+ (void)noteOn:(int)note velocity:(int)velocity;
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
