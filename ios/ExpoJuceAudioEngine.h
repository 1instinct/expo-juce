#pragma once

#import <Foundation/Foundation.h>

// beat: transport position (beats, fractional) at the end of the block that crossed a beat;
// position: transport position (beats, fractional) when the event is delivered.
typedef void(^BeatCallback)(double beat, double bpm, double position);

@interface ExpoJuceAudioEngine : NSObject

+ (instancetype)sharedInstance;

// Lifecycle
// Returns YES on success (or if already initialized). On failure, setupStep
// holds the step that failed and lastError a description.
// -setup uses the defaults: a 128-sample buffer, at the route's own sample rate (48000 Hz if the
// session reports none).
- (BOOL)setup;
// Preferred I/O buffer size (samples) and sample rate; the device grants the nearest it
// supports (see diagnostics: requested* / granted* vs preferredBufferSize / preferredSampleRate).
// The sample rate is a hint: the engine runs at the route's current hardware rate whenever the
// session reports one (no resampling), and uses this value only when it does not.
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
// As above, plus entryTimestamp: CACurrentMediaTime() at the touch handler's entry (touchesBegan),
// same clock. Feeds the touch latency legs (diagnostics touchLatency): delivery = entry − touch
// (recorded here, main thread only), handler = entry → queue push, queueToRender = push → render.
- (void)noteOn:(int)note velocity:(int)velocity touchTimestamp:(double)touchTimestamp
    entryTimestamp:(double)entryTimestamp;
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
// `gapless` may also carry `sourceBpm`, the file's own tempo (0.8.0-rc.3+): the track then follows
// the transport tempo (see AuxProcessor.h).
- (void)loadBackingTrack:(NSString *)uri
                 gapless:(NSDictionary<NSString *, id> *)gapless
              completion:(void (^)(NSDictionary<NSString *, id> *result, NSString *errorCode,
                                   NSString *errorMessage))completion;
- (void)unloadBackingTrack;

// Arpeggiator between the note queue and the voices (0.7.0+). Options are lock-free, any thread,
// remembered before setup and across rebuilds; absent keys unchanged, numbers clamped. Keys:
// enabled, mode ("up" | "down" | "upDown" | "played" | "random" | "chord"), rate ("1/4" | "1/8" |
// "1/8T" | "1/8D" | "1/16" | "1/16T" | "1/32"), octaves 1–4, gate 0.05–1.5, swing 0–0.5, latch,
// pattern (1–16 booleans), accent 0–1, ratchet 1–4, transposeSteps 0–8, transposeDistance −12..12.
- (BOOL)setArpeggiator:(NSDictionary<NSString *, id> *)options; // NO (nothing applied): unknown mode / rate, pattern not 1–16 booleans
// Every option as stored (pattern as an array of `length` booleans).
- (NSDictionary<NSString *, id> *)arpeggiatorState;

// Drums (0.8.0+): nine synthesized voices (eight before 0.8.0-rc.2) mixed before the effects (see DrumMachine.h).
// drumHit: voice 0–8, velocity clamped to 1–127; any thread, lock-free, through the note queue
// (out-of-range voice ignored; dropped while the engine is not ready, like noteOn).
- (void)drumHit:(int)voice velocity:(int)velocity;
// drumHit from a native touch (JucePadView in drums mode); touchTimestamp as for noteOn.
- (void)drumHit:(int)voice velocity:(int)velocity touchTimestamp:(double)touchTimestamp;
- (void)drumHit:(int)voice velocity:(int)velocity touchTimestamp:(double)touchTimestamp
    entryTimestamp:(double)entryTimestamp;

// Touch path diagnostics from JucePadView (main thread). reportTouchPath: the gesture recognizers
// and scroll views between the pad and its window (diagnostics touchPath, capped at 30 entries).
// reportLastTouch: the recognizers attached to the last touch and where it landed (lastTouch).
- (void)reportTouchPath:(NSArray<NSDictionary<NSString *, id> *> *)entries;
- (void)reportLastTouch:(NSDictionary<NSString *, id> *)touch;
// Parameters: lock-free, any thread, remembered before setup and across rebuilds; absent / unknown
// keys and non-finite values unchanged, numbers clamped. Voice keys: tune (semitones −24..24),
// decay, tone, level (0–1). NO for a voice outside 0–8 (nothing applied).
- (BOOL)setDrumVoice:(int)voice params:(NSDictionary<NSString *, id> *)params;
// Kit keys: level (0–1), mute (bool), drive (0–1). Always YES.
- (BOOL)setDrumKit:(NSDictionary<NSString *, id> *)params;
// { kit: { level, mute, drive }, voices: [9 × { tune, decay, tone, level }] } as stored.
- (NSDictionary<NSString *, id> *)drumState;

// Output analysis for HUDs: { peak, rms, scope: [128 numbers], bands: [24 numbers], sampleRate,
// lastNote, bpm, beat, playing, arp: { active, step, steps, note } }. Any thread; one 2048-point
// FFT per call (about 50 µs); zeros when the engine is not running.
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
// startBeatTimer works before setup (the timer idles until the engine exists); stopBeatTimer keeps
// the callback.
- (void)startBeatTimer;
- (void)stopBeatTimer;

@end
