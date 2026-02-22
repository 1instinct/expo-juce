#import "ExpoJuceBridge.h"
#import "JuceToneGenerator.h"

@implementation ExpoJuceBridge

+ (void)setup {
    // Access the singleton — audio engine is initialized in its -init
    [JuceToneGenerator sharedInstance];
}

+ (void)shutdown {
    [[JuceToneGenerator sharedInstance] shutdown];
}

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

@end
