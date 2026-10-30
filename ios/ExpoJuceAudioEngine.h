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

// Metronome + backing track (aux node, summed into the output after the effects). Options are
// lock-free, any thread, remembered before setup and across rebuilds; absent keys unchanged.
// metronome: enabled, style ("classic" | "wood" | "beep" | "cowbell" | "hat"), level 0–1,
// accent, beatsPerBar 1–16. Backing track: level 0–1, loop, enabled.
- (BOOL)setMetronome:(NSDictionary<NSString *, id> *)options; // NO (nothing applied): unknown style
- (void)setBackingTrackOptions:(NSDictionary<NSString *, id> *)options;
// Decodes `uri` (file:// URL or absolute path) on a background queue, cuts the loop region
// (gapless: loopSamples / leadingSamples / trailingSamples, optional), resamples to the device
// rate and swaps it in. A later load / unload supersedes it (ERR_BACKING_TRACK_SUPERSEDED).
// `completion` runs on that background queue: result on success, else errorCode + message.
- (void)loadBackingTrack:(NSString *)uri
                 gapless:(NSDictionary<NSString *, id> *)gapless
              completion:(void (^)(NSDictionary<NSString *, id> *result, NSString *errorCode,
                                   NSString *errorMessage))completion;
- (void)unloadBackingTrack;

// Output analysis for HUDs: { peak, rms, scope: [128 numbers], lastNote, bpm, beat, playing }.
// Cheap, any thread; zeros when the engine is not running.
- (NSDictionary<NSString *, id> *)analysis;

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
