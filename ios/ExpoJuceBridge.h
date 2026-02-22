#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface ExpoJuceBridge : NSObject

+ (void)setup;
+ (void)shutdown;
+ (void)playToneWithFrequency:(double)frequency duration:(double)duration;
+ (void)setFrequency:(double)frequency;
+ (void)setLevel:(double)level;
+ (void)setWaveform:(NSString *)waveform;
+ (void)setHarmonicLevels:(NSArray<NSNumber *> *)levels;
+ (void)noteOn;
+ (void)noteOff;

@end

NS_ASSUME_NONNULL_END
