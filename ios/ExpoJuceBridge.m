#import "ExpoJuceBridge.h"
#import "ExpoJuceAudioEngine.h"

@implementation ExpoJuceBridge

+ (BOOL)setup { return [[ExpoJuceAudioEngine sharedInstance] setup]; }
+ (BOOL)setupWithBufferSize:(int)bufferSize sampleRate:(double)sampleRate {
    return [[ExpoJuceAudioEngine sharedInstance] setupWithBufferSize:bufferSize sampleRate:sampleRate];
}
+ (NSString *)setupErrorMessage {
    ExpoJuceAudioEngine *engine = [ExpoJuceAudioEngine sharedInstance];
    NSString *error = engine.lastError ?: @"unknown error";
    return [NSString stringWithFormat:@"step %d: %@", engine.setupStep, error];
}
+ (void)shutdown { [[ExpoJuceAudioEngine sharedInstance] shutdown]; }

// ── Diagnostics ──
+ (NSDictionary<NSString *, id> *)getDiagnostics { return [[ExpoJuceAudioEngine sharedInstance] diagnostics]; }
+ (BOOL)playTestTone:(double)durationMs { return [[ExpoJuceAudioEngine sharedInstance] playTestTone:durationMs]; }
+ (void)startBeatTimer { [[ExpoJuceAudioEngine sharedInstance] startBeatTimer]; }
+ (void)stopBeatTimer { [[ExpoJuceAudioEngine sharedInstance] stopBeatTimer]; }

// ── Synth ──
+ (void)noteOn:(int)note velocity:(int)velocity {
    [[ExpoJuceAudioEngine sharedInstance] noteOn:note velocity:velocity];
}
+ (void)padNoteOn:(int)note velocity:(int)velocity touchTimestamp:(double)touchTimestamp {
    [[ExpoJuceAudioEngine sharedInstance] noteOn:note velocity:velocity touchTimestamp:touchTimestamp];
}
+ (void)noteOff:(int)note {
    [[ExpoJuceAudioEngine sharedInstance] noteOff:note];
}
+ (void)allNotesOff { [[ExpoJuceAudioEngine sharedInstance] allNotesOff]; }

// ── Global Params ──
+ (void)setWaveform:(NSString *)waveform { [[ExpoJuceAudioEngine sharedInstance] setWaveform:waveform]; }
+ (void)setFilterCutoff:(double)hz { [[ExpoJuceAudioEngine sharedInstance] setFilterCutoff:hz]; }
+ (void)setFilterResonance:(double)q { [[ExpoJuceAudioEngine sharedInstance] setFilterResonance:q]; }
+ (void)setAttack:(double)ms { [[ExpoJuceAudioEngine sharedInstance] setAttack:ms]; }
+ (void)setDecay:(double)ms { [[ExpoJuceAudioEngine sharedInstance] setDecay:ms]; }
+ (void)setSustain:(double)level { [[ExpoJuceAudioEngine sharedInstance] setSustain:level]; }
+ (void)setRelease:(double)ms { [[ExpoJuceAudioEngine sharedInstance] setRelease:ms]; }
+ (void)setDetune:(double)cents { [[ExpoJuceAudioEngine sharedInstance] setDetune:cents]; }
+ (void)setMasterLevel:(double)level { [[ExpoJuceAudioEngine sharedInstance] setMasterLevel:level]; }

// ── Effects ──
+ (BOOL)setEffectEnabled:(NSString *)effectId enabled:(BOOL)enabled {
    return [[ExpoJuceAudioEngine sharedInstance] setEffectEnabled:effectId enabled:enabled];
}
+ (BOOL)setEffectParam:(NSString *)effectId param:(NSString *)param value:(double)value {
    return [[ExpoJuceAudioEngine sharedInstance] setEffectParam:effectId param:param value:value];
}
+ (NSDictionary<NSString *, id> *)getEffectsState { return [[ExpoJuceAudioEngine sharedInstance] effectsState]; }

// ── Metronome / backing track / analysis ──
+ (void)setMetronome:(NSDictionary<NSString *, id> *)options { [[ExpoJuceAudioEngine sharedInstance] setMetronome:options]; }
+ (void)setBackingTrackOptions:(NSDictionary<NSString *, id> *)options {
    [[ExpoJuceAudioEngine sharedInstance] setBackingTrackOptions:options];
}
+ (void)loadBackingTrack:(NSString *)uri
                 gapless:(NSDictionary<NSString *, id> *)gapless
              completion:(void (^)(NSDictionary<NSString *, id> *, NSString *, NSString *))completion {
    [[ExpoJuceAudioEngine sharedInstance] loadBackingTrack:uri gapless:gapless completion:completion];
}
+ (void)unloadBackingTrack { [[ExpoJuceAudioEngine sharedInstance] unloadBackingTrack]; }
+ (NSDictionary<NSString *, id> *)getAnalysis { return [[ExpoJuceAudioEngine sharedInstance] analysis]; }

// ── Transport ──
+ (void)startTransport { [[ExpoJuceAudioEngine sharedInstance] startTransport]; }
+ (void)stopTransport { [[ExpoJuceAudioEngine sharedInstance] stopTransport]; }
+ (void)setTempo:(double)bpm { [[ExpoJuceAudioEngine sharedInstance] setTempo:bpm]; }
+ (void)setTransportPosition:(double)beat { [[ExpoJuceAudioEngine sharedInstance] setTransportPosition:beat]; }
+ (double)getTransportPosition { return [[ExpoJuceAudioEngine sharedInstance] getTransportPosition]; }
+ (double)getTempo { return [[ExpoJuceAudioEngine sharedInstance] getTempo]; }
+ (BOOL)isTransportPlaying { return [[ExpoJuceAudioEngine sharedInstance] isTransportPlaying]; }
+ (void)setBeatCallback:(ExpoJuceBeatBlock)callback {
    [[ExpoJuceAudioEngine sharedInstance] setBeatCallback:callback];
}

@end
