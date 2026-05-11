#pragma once

#import <Foundation/Foundation.h>

typedef void(^BeatCallback)(double beat, double bpm);

@interface BeeperAudioEngine : NSObject

+ (instancetype)sharedInstance;

// Lifecycle
- (void)setup;
- (void)shutdown;

// Synth — MIDI-style
- (void)noteOn:(int)note velocity:(int)velocity;
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
- (void)startBeatTimer;
- (void)stopBeatTimer;

@end
