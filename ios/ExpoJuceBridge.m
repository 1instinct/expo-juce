#import "ExpoJuceBridge.h"
#import "BeeperAudioEngine.h"

@implementation ExpoJuceBridge

+ (void)setup { [[BeeperAudioEngine sharedInstance] setup]; }
+ (void)shutdown { [[BeeperAudioEngine sharedInstance] shutdown]; }
+ (void)startBeatTimer { [[BeeperAudioEngine sharedInstance] startBeatTimer]; }
+ (void)stopBeatTimer { [[BeeperAudioEngine sharedInstance] stopBeatTimer]; }

// ── Synth ──
+ (void)noteOn:(int)note velocity:(int)velocity {
    [[BeeperAudioEngine sharedInstance] noteOn:note velocity:velocity];
}
+ (void)noteOff:(int)note {
    [[BeeperAudioEngine sharedInstance] noteOff:note];
}
+ (void)allNotesOff { [[BeeperAudioEngine sharedInstance] allNotesOff]; }

// ── Global Params ──
+ (void)setWaveform:(NSString *)waveform { [[BeeperAudioEngine sharedInstance] setWaveform:waveform]; }
+ (void)setFilterCutoff:(double)hz { [[BeeperAudioEngine sharedInstance] setFilterCutoff:hz]; }
+ (void)setFilterResonance:(double)q { [[BeeperAudioEngine sharedInstance] setFilterResonance:q]; }
+ (void)setAttack:(double)ms { [[BeeperAudioEngine sharedInstance] setAttack:ms]; }
+ (void)setDecay:(double)ms { [[BeeperAudioEngine sharedInstance] setDecay:ms]; }
+ (void)setSustain:(double)level { [[BeeperAudioEngine sharedInstance] setSustain:level]; }
+ (void)setRelease:(double)ms { [[BeeperAudioEngine sharedInstance] setRelease:ms]; }
+ (void)setDetune:(double)cents { [[BeeperAudioEngine sharedInstance] setDetune:cents]; }
+ (void)setMasterLevel:(double)level { [[BeeperAudioEngine sharedInstance] setMasterLevel:level]; }

// ── Transport ──
+ (void)startTransport { [[BeeperAudioEngine sharedInstance] startTransport]; }
+ (void)stopTransport { [[BeeperAudioEngine sharedInstance] stopTransport]; }
+ (void)setTempo:(double)bpm { [[BeeperAudioEngine sharedInstance] setTempo:bpm]; }
+ (void)setTransportPosition:(double)beat { [[BeeperAudioEngine sharedInstance] setTransportPosition:beat]; }
+ (double)getTransportPosition { return [[BeeperAudioEngine sharedInstance] getTransportPosition]; }
+ (double)getTempo { return [[BeeperAudioEngine sharedInstance] getTempo]; }
+ (BOOL)isTransportPlaying { return [[BeeperAudioEngine sharedInstance] isTransportPlaying]; }
+ (void)setBeatCallback:(ExpoJuceBeatBlock)callback {
    [[BeeperAudioEngine sharedInstance] setBeatCallback:callback];
}

@end
