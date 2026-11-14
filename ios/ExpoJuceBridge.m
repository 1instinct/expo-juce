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
+ (BOOL)setDisplayRateHint:(double)hz { return [[ExpoJuceAudioEngine sharedInstance] setDisplayRateHint:hz]; }
+ (void)startBeatTimer { [[ExpoJuceAudioEngine sharedInstance] startBeatTimer]; }
+ (void)stopBeatTimer { [[ExpoJuceAudioEngine sharedInstance] stopBeatTimer]; }
+ (void)reportTouchPath:(NSArray<NSDictionary<NSString *, id> *> *)entries {
    [[ExpoJuceAudioEngine sharedInstance] reportTouchPath:entries];
}
+ (void)reportLastTouch:(NSDictionary<NSString *, id> *)touch {
    [[ExpoJuceAudioEngine sharedInstance] reportLastTouch:touch];
}

// ── Synth ──
+ (void)noteOn:(int)note velocity:(int)velocity {
    [[ExpoJuceAudioEngine sharedInstance] noteOn:note velocity:velocity];
}
+ (void)padNoteOn:(int)note velocity:(int)velocity touchTimestamp:(double)touchTimestamp {
    [[ExpoJuceAudioEngine sharedInstance] noteOn:note velocity:velocity touchTimestamp:touchTimestamp];
}
+ (void)padNoteOn:(int)note velocity:(int)velocity touchTimestamp:(double)touchTimestamp entryTimestamp:(double)entryTimestamp {
    [[ExpoJuceAudioEngine sharedInstance] noteOn:note velocity:velocity touchTimestamp:touchTimestamp entryTimestamp:entryTimestamp];
}
+ (void)noteOff:(int)note {
    [[ExpoJuceAudioEngine sharedInstance] noteOff:note];
}
+ (void)allNotesOff { [[ExpoJuceAudioEngine sharedInstance] allNotesOff]; }

// ── MIDI byte input ──
+ (BOOL)setMidiInputMapActions:(NSArray<NSNumber *> *)actions velocity:(NSArray<NSNumber *> *)velocity {
    return [[ExpoJuceAudioEngine sharedInstance] setMidiInputMapActions:actions velocity:velocity];
}
+ (void)setMidiInputEnabled:(BOOL)enabled { [[ExpoJuceAudioEngine sharedInstance] setMidiInputEnabled:enabled]; }
+ (int)releaseMidiInputHolds { return [[ExpoJuceAudioEngine sharedInstance] releaseMidiInputHolds]; }
+ (double)midiInputClock { return [[ExpoJuceAudioEngine sharedInstance] midiInputClock]; }
+ (int)midiInputBytes:(NSArray<NSNumber *> *)bytes format:(int)format arrival:(double)arrivalSeconds {
    return [[ExpoJuceAudioEngine sharedInstance] midiInputBytes:bytes format:format arrival:arrivalSeconds];
}
+ (uint64_t)setMidiInputHandler:(void (^)(NSDictionary<NSString *, id> *))handler {
    return [[ExpoJuceAudioEngine sharedInstance] setMidiInputHandler:handler];
}
+ (void)clearMidiInputHandler:(uint64_t)token { [[ExpoJuceAudioEngine sharedInstance] clearMidiInputHandler:token]; }
+ (void)setMidiInputListening:(BOOL)listening token:(uint64_t)token {
    [[ExpoJuceAudioEngine sharedInstance] setMidiInputListening:listening token:token];
}
+ (uint64_t)claimModule { return [[ExpoJuceAudioEngine sharedInstance] claimModule]; }
+ (void)releaseModule:(uint64_t)token { [[ExpoJuceAudioEngine sharedInstance] releaseModule:token]; }
+ (void *)midiInputBytesFunction { return [ExpoJuceAudioEngine midiInputBytesFunction]; }

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
+ (BOOL)setMetronome:(NSDictionary<NSString *, id> *)options { return [[ExpoJuceAudioEngine sharedInstance] setMetronome:options]; }
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
+ (NSDictionary<NSString *, id> *)getSourceAnalysis:(NSString *)source options:(NSDictionary<NSString *, id> *)options {
    return [[ExpoJuceAudioEngine sharedInstance] sourceAnalysis:source options:options];
}

// ── Arpeggiator ──
+ (BOOL)setArpeggiator:(NSDictionary<NSString *, id> *)options {
    return [[ExpoJuceAudioEngine sharedInstance] setArpeggiator:options];
}
+ (NSDictionary<NSString *, id> *)getArpeggiatorState { return [[ExpoJuceAudioEngine sharedInstance] arpeggiatorState]; }

// ── Drums ──
+ (void)drumHit:(int)voice velocity:(int)velocity {
    [[ExpoJuceAudioEngine sharedInstance] drumHit:voice velocity:velocity];
}
+ (void)padDrumHit:(int)voice velocity:(int)velocity touchTimestamp:(double)touchTimestamp {
    [[ExpoJuceAudioEngine sharedInstance] drumHit:voice velocity:velocity touchTimestamp:touchTimestamp];
}
+ (void)padDrumHit:(int)voice velocity:(int)velocity touchTimestamp:(double)touchTimestamp entryTimestamp:(double)entryTimestamp {
    [[ExpoJuceAudioEngine sharedInstance] drumHit:voice velocity:velocity touchTimestamp:touchTimestamp
                                   entryTimestamp:entryTimestamp];
}
+ (BOOL)setDrumVoice:(int)voice params:(NSDictionary<NSString *, id> *)params {
    return [[ExpoJuceAudioEngine sharedInstance] setDrumVoice:voice params:params];
}
+ (BOOL)setDrumKit:(NSDictionary<NSString *, id> *)params {
    return [[ExpoJuceAudioEngine sharedInstance] setDrumKit:params];
}
+ (NSDictionary<NSString *, id> *)getDrumState { return [[ExpoJuceAudioEngine sharedInstance] drumState]; }

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
