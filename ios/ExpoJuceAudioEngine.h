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
// -setup uses the defaults: a 128-sample buffer, at the session's current sample rate (48000 Hz if
// the session reports no music rate).
- (BOOL)setup;
// Preferred I/O buffer size (samples) and sample rate; the device grants the nearest it
// supports (see diagnostics: requested* / granted* vs preferredBufferSize / preferredSampleRate).
// The sample rate is a hint: the engine runs at the session's current rate whenever that is a
// music rate (no resampling), and uses this value only when it is not. On a fresh session that
// is the route's own rate; after an earlier setup in the same process it is usually the rate that
// setup ran at. After a route change the engine changes rate only when the new route cannot run
// the current one.
// Values <= 0 fall back to the defaults.
- (BOOL)setupWithBufferSize:(int)bufferSize sampleRate:(double)sampleRate;
- (void)shutdown;

// Diagnostics
@property (nonatomic, readonly) int setupStep;       // -1 = setup never run
@property (atomic, readonly, copy) NSString *lastError; // nil = no error; atomic: read off the main thread
- (NSDictionary<NSString *, id> *)diagnostics;
// Returns NO if the engine is not ready or the device is not playing.
- (BOOL)playTestTone:(double)durationMs;
// Display-rate hint (DisplayRate.h), any thread, independent of setup: above 60 on a screen that
// goes faster, a main-thread display link keeps the display at min(hz, the screen maximum); 0 or 60
// removes that. Applied on the main thread. NO for a non-finite hz (nothing changes).
- (BOOL)setDisplayRateHint:(double)hz;

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

// MIDI byte input (MidiInput.h; native code calls expojuce_midi_input_bytes, ExpoJuceMidiInput.h).
// Any thread, before setup and across rebuilds.
// actions: 128 numbers, per input note: -1 pass to the app, 0–127 play that synth note, 256 + v hit
// drum voice v (0–8); anything else passes. velocity: 128 numbers, input velocity → played velocity
// (clamped 1–127), nil = identity. NO (nothing applied) unless both have 128 entries.
- (BOOL)setMidiInputMapActions:(NSArray<NSNumber *> *)actions velocity:(NSArray<NSNumber *> *)velocity;
// Off (the default): every note-on passes to the app; note-offs still release the input's holds.
- (void)setMidiInputEnabled:(BOOL)enabled;
// Ends every hold the input started (the device went away); returns the new epoch.
- (int)releaseMidiInputHolds;
// Now, in seconds on the input's arrival clock (CACurrentMediaTime()).
- (double)midiInputClock;
// The C entry point with bytes from JS (one call = one packet, at most 512 bytes). arrival <= 0 =
// now. Messages parsed; -1 = bad format or more than 512 bytes (never truncated).
- (int)midiInputBytes:(NSArray<NSNumber *> *)bytes format:(int)format arrival:(double)arrivalSeconds;
// Receives { events, now, epoch } batches on a background queue while the caller's JS listens
// (setMidiInputListening). Returns the caller's token: only that token clears it, so a module
// destroyed after its replacement was created leaves the new handler alone.
- (uint64_t)setMidiInputHandler:(void (^)(NSDictionary<NSString *, id> *batch))handler;
- (void)clearMidiInputHandler:(uint64_t)token;
// Events are dropped (not built) while nobody listens. Ignored unless `token` set the handler.
- (void)setMidiInputListening:(BOOL)listening token:(uint64_t)token;
// The engine's owner across module instances: OnCreate claims; OnDestroy releases, which stops the
// beat timer and shuts the engine down only if no newer module claimed it meanwhile (otherwise it
// only releases held notes).
- (uint64_t)claimModule;
- (void)releaseModule:(uint64_t)token;
// &expojuce_midi_input_bytes, for native callers that look it up through the Objective-C runtime.
+ (void *)midiInputBytesFunction;

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

// Sampler (0.10.0+): 16 sample slots (0–15) on 32 voices, mixed before the effects (see
// SamplerEngine.h, SamplerBank.h). Loaded samples and parameters live for the process: they work
// before setup and survive rebuilds. samplerHit (pads: the slot at its root) / samplerNoteOn (keys:
// every loaded slot whose zone holds the note): velocity clamped to 1–127; any thread, lock-free,
// through the note queue; dropped while the engine is not ready, like noteOn. samplerRelease /
// samplerNoteOff (and samplerNoteOn with velocity 0, a note-off) are always recorded (like
// noteOff). Out-of-range slot / note: ignored. samplerRelease releases every gated voice that a
// hit on the slot started, whoever hit it (two pads on one slot share the release).
- (void)samplerHit:(int)slot velocity:(int)velocity;
// samplerHit from a native touch (JucePadView in sampler mode); touchTimestamp as for noteOn.
- (void)samplerHit:(int)slot velocity:(int)velocity touchTimestamp:(double)touchTimestamp
    entryTimestamp:(double)entryTimestamp;
- (void)samplerRelease:(int)slot;
- (void)samplerNoteOn:(int)note velocity:(int)velocity;
- (void)samplerNoteOff:(int)note;
// Partial update of a slot; absent keys, keys of the wrong type and non-finite numbers unchanged,
// numbers clamped. Keys: rootNote, lowNote, highNote, startMs, endMs, loop ({ startMs, endMs } or
// NO / NSNull = no loop), oneShot, gainDb, pan, tune, fineCents, attackMs, releaseMs,
// velocitySens, chokeGroup. NO for a slot outside 0–15 (nothing applied).
- (BOOL)setSamplerSlot:(int)slot params:(NSDictionary<NSString *, id> *)params;
// Keys: level (0–1), mute (bool), polyphony (1–32). Always YES.
- (BOOL)setSampler:(NSDictionary<NSString *, id> *)settings;
// { level, mute, polyphony, slots: [16 × { loaded, frames, durationMs, ...params }] } as stored.
- (NSDictionary<NSString *, id> *)samplerState;
// Decodes `uri` (file:// URL or absolute path) on a background queue at the device rate and swaps
// it into `slot`. `params` (partial, as setSamplerSlot; nil = none) are applied right before the
// sample is published, and only if it is, so a failed or superseded load leaves the slot's
// current parameters intact. A later load / unload of the slot supersedes it
// (ERR_SAMPLER_SUPERSEDED). `completion` runs on that background queue: { frames, sampleRate,
// channels, durationMs } on success, else errorCode (ERR_SAMPLER_SLOT / _FILE / _FORMAT /
// _TOO_LONG / _MEMORY / _SUPERSEDED) + message.
- (void)loadSamplerSlot:(int)slot
                    uri:(NSString *)uri
                 params:(NSDictionary<NSString *, id> *)params
             completion:(void (^)(NSDictionary<NSString *, id> *result, NSString *errorCode,
                                  NSString *errorMessage))completion;
- (BOOL)unloadSamplerSlot:(int)slot; // NO: slot out of range

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

// Loop sequence (SequencePlayer.h, 0.9.0-rc.5): played on the audio thread against the transport.
// data: `count` numbers, 6 per event: start (beats, 0 <= start < loopBeats), length (beats > 0;
// drums: ignored), note (0–127, the drum voice 0–8 or the sampler slot 0–15), velocity (1–127),
// kind (0 note, 1 drum, 2 sampler pad hit, 3 sampler key note; 0.10.0-rc.2), skipBefore (absolute
// beat, NaN = none). fromBeat / endBeat: absolute beats; nothing fires before fromBeat or at / after
// endBeat (-inf / +inf = no bound). Any thread but the audio thread; the new sequence replaces the
// old one atomically. NO (nothing changes): count not a multiple of 6, more than 8192 events, a bad
// loop length with events, an unknown kind, a sampler slot outside 0–15 or a sampler key outside
// 0–127 (diagnostics sequence.rejectedEvent / rejectReason name it).
- (BOOL)setSequenceData:(const double *)data
                  count:(NSInteger)count
              loopBeats:(double)loopBeats
               fromBeat:(double)fromBeat
                endBeat:(double)endBeat;
- (void)setSequenceMuted:(BOOL)muted;

// Events for the app's JS side (0.9.0-rc.5): onSequenceNotes, onRemoteCommand, onAudioInterruption,
// onTransportChange, onBackgroundState, delivered to `handler` on a background serial queue.
// Returns the caller's token: only that token clears it or switches the sequence notes on.
- (uint64_t)setEngineEventHandler:(void (^)(NSString *name, NSDictionary<NSString *, id> *body))handler;
- (void)clearEngineEventHandler:(uint64_t)token;
// onSequenceNotes batches (every ~16 ms while notes play) are built only while this is on.
- (void)setSequenceNotesListening:(BOOL)listening token:(uint64_t)token;

// Background policy (BackgroundPolicy.h, 0.9.0-rc.5). Any thread; applied on the main thread.
- (void)setBackgroundPolicyKeepAlive:(BOOL)keepAlive idleStopSeconds:(double)idleStopSeconds;
// Now Playing (MPNowPlayingInfoCenter): keys title (required), artist, album; nil clears it.
// While it is set and the transport plays (or the app is in the background) the session is not
// mixable, which iOS requires to show it. NO (nothing changes) without a title. Any thread.
- (BOOL)setNowPlaying:(NSDictionary<NSString *, id> *)info;
// Lock-screen / Control Center commands routed to the native transport: any of "play", "pause",
// "togglePlayPause" (an empty list removes them). NO (nothing changes) for an unknown name.
- (BOOL)enableRemoteCommands:(NSArray<NSString *> *)commands;

// Beat events
- (void)setBeatCallback:(BeatCallback)callback;
// The beat timer lives on the main queue; called off the main thread these re-dispatch to main.
// startBeatTimer works before setup (the timer idles until the engine exists); stopBeatTimer keeps
// the callback.
- (void)startBeatTimer;
- (void)stopBeatTimer;

@end
