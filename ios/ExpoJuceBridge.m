#import "ExpoJuceBridge.h"
#import "JuceToneGenerator.h"

@implementation ExpoJuceBridge

+ (void)setup {
    [JuceToneGenerator sharedInstance];
}

+ (void)shutdown {
    [[JuceToneGenerator sharedInstance] shutdown];
}

// ── Synth ─────────────────────────────────────────────────────────

+ (void)playToneWithFrequency:(double)frequency duration:(double)duration {
    [[JuceToneGenerator sharedInstance] playToneWithFrequency:frequency duration:duration];
}

+ (void)setFrequency:(double)frequency {
    [[JuceToneGenerator sharedInstance] setFrequency:frequency];
}

+ (void)setLevel:(double)level {
    [[JuceToneGenerator sharedInstance] setLevel:level];
}

+ (void)setWaveform:(NSString *)waveform {
    [[JuceToneGenerator sharedInstance] setWaveform:waveform];
}

+ (void)setHarmonicLevels:(NSArray<NSNumber *> *)levels {
    [[JuceToneGenerator sharedInstance] setHarmonicLevels:levels];
}

+ (void)noteOn {
    [[JuceToneGenerator sharedInstance] noteOn];
}

+ (void)noteOff {
    [[JuceToneGenerator sharedInstance] noteOff];
}

// ── DSP Params ────────────────────────────────────────────────────

+ (void)setAttack:(double)ms {
    [[JuceToneGenerator sharedInstance] setAttack:ms];
}

+ (void)setRelease:(double)ms {
    [[JuceToneGenerator sharedInstance] setRelease:ms];
}

+ (void)setFilterCutoff:(double)hz {
    [[JuceToneGenerator sharedInstance] setFilterCutoff:hz];
}

+ (void)setFilterResonance:(double)q {
    [[JuceToneGenerator sharedInstance] setFilterResonance:q];
}

+ (void)setDetune:(double)cents {
    [[JuceToneGenerator sharedInstance] setDetune:cents];
}

// ── Transport ─────────────────────────────────────────────────────

+ (void)startTransport {
    [[JuceToneGenerator sharedInstance] startTransport];
}

+ (void)stopTransport {
    [[JuceToneGenerator sharedInstance] stopTransport];
}

+ (void)setTempo:(double)bpm {
    [[JuceToneGenerator sharedInstance] setTempo:bpm];
}

+ (void)setTransportPosition:(double)beat {
    [[JuceToneGenerator sharedInstance] setTransportPosition:beat];
}

+ (double)getTransportPosition {
    return [[JuceToneGenerator sharedInstance] getTransportPosition];
}

+ (double)getTempo {
    return [[JuceToneGenerator sharedInstance] getTempo];
}

+ (BOOL)isTransportPlaying {
    return [[JuceToneGenerator sharedInstance] isTransportPlaying];
}

+ (void)setBeatCallback:(ExpoJuceBeatBlock)callback {
    [[JuceToneGenerator sharedInstance] setBeatCallback:callback];
}

@end
