#import "ExpoJuceBridge.h"
#import "JuceToneGenerator.h"

@implementation ExpoJuceBridge

+ (void)initialize {
    [[JuceToneGenerator sharedInstance] initialize];
}

+ (void)shutdown {
    [[JuceToneGenerator sharedInstance] shutdown];
}

+ (void)playToneWithFrequency:(double)frequency duration:(double)duration {
    [[JuceToneGenerator sharedInstance] playToneWithFrequency:frequency duration:duration];
}

@end
