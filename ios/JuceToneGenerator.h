#import <Foundation/Foundation.h>

typedef void(^BeatCallback)(double beat, double bpm);

@interface JuceToneGenerator : NSObject

+ (instancetype)sharedInstance;
- (void)initialize;
- (void)shutdown;

// ── Synth ─────────────────────────────────────────────────────────
- (void)playToneWithFrequency:(double)frequency duration:(double)duration;
- (void)setFrequency:(double)frequency;
- (void)setLevel:(double)level;
- (void)setWaveform:(NSString *)waveform;
- (void)setHarmonicLevels:(NSArray<NSNumber *> *)levels;
- (void)noteOn;
- (void)noteOff;

// ── DSP Params ────────────────────────────────────────────────────
- (void)setAttack:(double)ms;
- (void)setRelease:(double)ms;
- (void)setFilterCutoff:(double)hz;
- (void)setFilterResonance:(double)q;
- (void)setDetune:(double)cents;

// ── Transport ─────────────────────────────────────────────────────
- (void)startTransport;
- (void)stopTransport;
- (void)setTempo:(double)bpm;
- (void)setTransportPosition:(double)beat;
- (double)getTransportPosition;
- (double)getTempo;
- (BOOL)isTransportPlaying;
- (void)setBeatCallback:(BeatCallback)callback;

@end
