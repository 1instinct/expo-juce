#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface ExpoJuceBridge : NSObject

+ (void)initialize;
+ (void)shutdown;
+ (void)playToneWithFrequency:(double)frequency duration:(double)duration;

@end

NS_ASSUME_NONNULL_END
