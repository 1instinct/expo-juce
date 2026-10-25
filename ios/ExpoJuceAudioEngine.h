#pragma once

#import <Foundation/Foundation.h>

typedef void(^BeatCallback)(double beat, double bpm);

@interface ExpoJuceAudioEngine : NSObject

+ (instancetype)sharedInstance;

// Lifecycle
// Returns YES on success (or if already initialized). On failure, setupStep
// holds the step that failed and lastError a description.
// -setup uses the defaults: 128-sample buffer at 48000 Hz.
- (BOOL)setup;
// Preferred I/O buffer size (samples) and sample rate; the device grants the nearest it
// supports (see diagnostics: bufferSize / sampleRate vs preferredBufferSize / preferredSampleRate).
// Values <= 0 fall back to the defaults.
- (BOOL)setupWithBufferSize:(int)bufferSize sampleRate:(double)sampleRate;
- (void)shutdown;

// Diagnostics
@property (nonatomic, readonly) int setupStep;       // -1 = setup never run
@property (atomic, readonly, copy) NSString *lastError; // nil = no error; atomic: read off the main thread
- (NSDictionary<NSString *, id> *)diagnostics;
// Returns NO if the engine is not ready or the device is not playing.
- (BOOL)playTestTone:(double)durationMs;

// Synth — MIDI-style
- (void)noteOn:(int)note velocity:(int)velocity;
// noteOn from a native touch: touchTimestamp is UITouch.timestamp (seconds since boot,
// mach_absolute_time clock). Used for the lastTouchToRenderMs diagnostic.
- (void)noteOn:(int)note velocity:(int)velocity touchTimestamp:(double)touchTimestamp;
- (void)noteOff:(int)note;
- (void)allNotesOff;

// Global synth params
- (void)setWaveform:(NSString *)waveform;
- (void)setFilterCutoff:(double)hz;
- (void)setFilterResonance:(double)q;
- (void)setAttack:(double)ms;
- (void)setDecay:(double)ms;
- (void)setSustain:(double)level;
- (void)setRelease:(double)ms;
- (void)setDetune:(double)cents;
- (void)setMasterLevel:(double)level;

// Effects chain (synth → fx → output). Any thread, never blocks; settings are remembered
// whether or not the engine is running (before setup, across rebuilds) and apply to the
// running chain at its next processing chunk. effectId / param as in FxChainProcessor.h
// (kFxSpecs). Unknown id / param or a non-finite value: no-op, returns NO. Values are clamped.
- (BOOL)setEffectEnabled:(NSString *)effectId enabled:(BOOL)enabled;
- (BOOL)setEffectParam:(NSString *)effectId param:(NSString *)param value:(double)value;
// { <effectId>: { enabled: BOOL, params: { <param>: number } } } for every effect.
- (NSDictionary<NSString *, id> *)effectsState;

// Transport
- (void)startTransport;
- (void)stopTransport;
- (void)setTempo:(double)bpm;
- (void)setTransportPosition:(double)beat;
- (double)getTransportPosition;
- (double)getTempo;
- (BOOL)isTransportPlaying;

// Beat events
- (void)setBeatCallback:(BeatCallback)callback;
// The beat timer lives on the main queue; called off the main thread these re-dispatch to main.
- (void)startBeatTimer;
- (void)stopBeatTimer;

@end
