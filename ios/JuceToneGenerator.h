#import <Foundation/Foundation.h>

@interface JuceToneGenerator : NSObject

+ (instancetype)sharedInstance;
- (void)playToneWithFrequency:(double)frequency duration:(double)duration;
- (void)initialize;
- (void)shutdown;

@end
