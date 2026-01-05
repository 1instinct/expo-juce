#import "JuceToneGenerator.h"
#import <AVFoundation/AVFoundation.h>
#include <atomic>
#include <cmath>

// Simple sine wave generator
class SimpleToneGenerator {
public:
    SimpleToneGenerator() : frequency(440.0), phase(0.0), isPlaying(false) {}
    
    void setFrequency(double freq) {
        frequency = freq;
    }
    
    void start() {
        phase = 0.0;
        isPlaying = true;
    }
    
    void stop() {
        isPlaying = false;
    }
    
    float getNextSample(double sampleRate) {
        if (!isPlaying) {
            return 0.0f;
        }
        
        float sample = std::sin(phase * 2.0 * M_PI) * 0.3f; // 30% volume
        phase += frequency / sampleRate;
        if (phase >= 1.0) {
            phase -= 1.0;
        }
        return sample;
    }
    
private:
    std::atomic<double> frequency;
    double phase;
    std::atomic<bool> isPlaying;
};

@interface JuceToneGenerator ()
@property (nonatomic, strong) AVAudioEngine *audioEngine;
@property (nonatomic, strong) AVAudioSourceNode *sourceNode;
@property (nonatomic, assign) SimpleToneGenerator *generator;
@end

@implementation JuceToneGenerator

+ (instancetype)sharedInstance {
    static JuceToneGenerator *instance = nil;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        instance = [[self alloc] init];
    });
    return instance;
}

- (instancetype)init {
    self = [super init];
    if (self) {
        _generator = new SimpleToneGenerator();
        [self initialize];
    }
    return self;
}

- (void)dealloc {
    [self shutdown];
    if (_generator) {
        delete _generator;
        _generator = nullptr;
    }
}

- (void)initialize {
    NSError *error = nil;
    
    // Configure audio session for low latency
    AVAudioSession *audioSession = [AVAudioSession sharedInstance];
    [audioSession setCategory:AVAudioSessionCategoryPlayback
                  withOptions:AVAudioSessionCategoryOptionMixWithOthers
                        error:&error];
    if (error) {
        NSLog(@"Error setting audio session category: %@", error);
    }
    
    // Set preferred buffer duration for low latency
    [audioSession setPreferredIOBufferDuration:0.005 error:&error]; // 5ms
    if (error) {
        NSLog(@"Error setting buffer duration: %@", error);
    }
    
    [audioSession setActive:YES error:&error];
    if (error) {
        NSLog(@"Error activating audio session: %@", error);
    }
    
    // Create audio engine
    self.audioEngine = [[AVAudioEngine alloc] init];
    AVAudioFormat *format = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:44100.0 channels:2];
    
    SimpleToneGenerator *generatorPtr = self.generator;
    
    // Create source node that generates audio
    self.sourceNode = [[AVAudioSourceNode alloc] initWithFormat:format renderBlock:^OSStatus(BOOL *isSilence, const AudioTimeStamp *timestamp, AVAudioFrameCount frameCount, AudioBufferList *outputData) {
        
        float *leftChannel = (float *)outputData->mBuffers[0].mData;
        float *rightChannel = (float *)outputData->mBuffers[1].mData;
        
        for (AVAudioFrameCount i = 0; i < frameCount; i++) {
            float sample = generatorPtr->getNextSample(44100.0);
            leftChannel[i] = sample;
            rightChannel[i] = sample;
        }
        
        return noErr;
    }];
    
    // Connect source node to output
    [self.audioEngine attachNode:self.sourceNode];
    [self.audioEngine connect:self.sourceNode to:self.audioEngine.mainMixerNode format:format];
    [self.audioEngine connect:self.audioEngine.mainMixerNode to:self.audioEngine.outputNode format:format];
    
    // Start engine
    [self.audioEngine prepare];
    [self.audioEngine startAndReturnError:&error];
    if (error) {
        NSLog(@"Error starting audio engine: %@", error);
    }
}

- (void)shutdown {
    if (self.audioEngine) {
        [self.audioEngine stop];
        self.audioEngine = nil;
    }
    self.sourceNode = nil;
}

- (void)playToneWithFrequency:(double)frequency duration:(double)duration {
    if (!self.generator) return;
    
    self.generator->setFrequency(frequency);
    self.generator->start();
    
    // Stop after duration
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(duration * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        self.generator->stop();
    });
}

@end
