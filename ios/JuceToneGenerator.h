#import <Foundation/Foundation.h>

@interface JuceToneGenerator : NSObject

+ (instancetype)sharedInstance;
- (void)initialize;
- (void)shutdown;
- (void)playToneWithFrequency:(double)frequency duration:(double)duration;
- (void)setFrequency:(double)frequency;
- (void)setLevel:(double)level;
- (void)setWaveform:(NSString *)waveform;
- (void)setHarmonicLevels:(NSArray<NSNumber *> *)levels;
- (void)noteOn;
- (void)noteOff;

@end
