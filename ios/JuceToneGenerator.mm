#import "JuceToneGenerator.h"
#import <AVFoundation/AVFoundation.h>
#include <atomic>
#include <cmath>

enum class Waveform { Sine, Square, Saw, Triangle };

static const int MAX_HARMONICS = 8;

class SynthEngine {
public:
    SynthEngine()
        : frequency(440.0), level(0.5), phase(0.0),
          waveform(Waveform::Sine), isPlaying(false), envelopeGain(0.0),
          attackRate(0.005), releaseRate(0.002)
    {
        for (int i = 0; i < MAX_HARMONICS; i++) {
            harmonics[i] = (i == 0) ? 1.0 : 0.0;
        }
    }

    void setFrequency(double freq) { frequency = freq; }
    void setLevel(double lev) { level = lev; }
    void setWaveform(Waveform wf) { waveform.store(wf); }

    void setHarmonics(const double *levels, int count) {
        for (int i = 0; i < MAX_HARMONICS; i++) {
            harmonics[i] = (i < count) ? levels[i] : 0.0;
        }
    }

    void noteOn() {
        phase = 0.0;
        isPlaying = true;
    }

    void noteOff() {
        isPlaying = false;
    }

    float getNextSample(double sampleRate) {
        // Smooth envelope to avoid clicks
        double target = isPlaying.load() ? 1.0 : 0.0;
        double rate = (target > envelopeGain) ? attackRate : releaseRate;
        envelopeGain += (target - envelopeGain) * rate;

        if (envelopeGain < 0.0001 && !isPlaying.load()) {
            return 0.0f;
        }

        double freq = frequency.load();
        double lev = level.load();
        Waveform wf = waveform.load();
        double sample = 0.0;

        switch (wf) {
            case Waveform::Sine:
                // Additive synthesis with harmonics
                for (int h = 0; h < MAX_HARMONICS; h++) {
                    double hLevel = harmonics[h].load();
                    if (hLevel > 0.0001) {
                        sample += std::sin(phase * (h + 1) * 2.0 * M_PI) * hLevel;
                    }
                }
                break;

            case Waveform::Square: {
                double p = std::fmod(phase, 1.0);
                sample = (p < 0.5) ? 0.7 : -0.7;
                break;
            }

            case Waveform::Saw: {
                double p = std::fmod(phase, 1.0);
                sample = (2.0 * p - 1.0) * 0.6;
                break;
            }

            case Waveform::Triangle: {
                double p = std::fmod(phase, 1.0);
                sample = (4.0 * std::fabs(p - 0.5) - 1.0) * 0.8;
                break;
            }
        }

        phase += freq / sampleRate;
        if (phase >= 1.0) phase -= 1.0;

        return (float)(sample * lev * envelopeGain);
    }

private:
    std::atomic<double> frequency;
    std::atomic<double> level;
    double phase;
    std::atomic<Waveform> waveform;
    std::atomic<bool> isPlaying;
    double envelopeGain;
    double attackRate;
    double releaseRate;
    std::atomic<double> harmonics[MAX_HARMONICS];
};

@interface JuceToneGenerator ()
@property (nonatomic, strong) AVAudioEngine *audioEngine;
@property (nonatomic, strong) AVAudioSourceNode *sourceNode;
@property (nonatomic, assign) SynthEngine *engine;
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
        _engine = new SynthEngine();
        [self initialize];
    }
    return self;
}

- (void)dealloc {
    [self shutdown];
    if (_engine) {
        delete _engine;
        _engine = nullptr;
    }
}

- (void)initialize {
    NSError *error = nil;

    AVAudioSession *audioSession = [AVAudioSession sharedInstance];
    [audioSession setCategory:AVAudioSessionCategoryPlayback
                  withOptions:AVAudioSessionCategoryOptionMixWithOthers
                        error:&error];
    if (error) NSLog(@"Error setting audio session category: %@", error);

    [audioSession setPreferredIOBufferDuration:0.005 error:&error]; // 5ms
    if (error) NSLog(@"Error setting buffer duration: %@", error);

    [audioSession setActive:YES error:&error];
    if (error) NSLog(@"Error activating audio session: %@", error);

    self.audioEngine = [[AVAudioEngine alloc] init];
    AVAudioFormat *format = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:44100.0 channels:2];

    SynthEngine *eng = self.engine;

    self.sourceNode = [[AVAudioSourceNode alloc] initWithFormat:format renderBlock:^OSStatus(BOOL *isSilence, const AudioTimeStamp *timestamp, AVAudioFrameCount frameCount, AudioBufferList *outputData) {
        float *leftChannel = (float *)outputData->mBuffers[0].mData;
        float *rightChannel = (float *)outputData->mBuffers[1].mData;

        for (AVAudioFrameCount i = 0; i < frameCount; i++) {
            float sample = eng->getNextSample(44100.0);
            leftChannel[i] = sample;
            rightChannel[i] = sample;
        }

        return noErr;
    }];

    [self.audioEngine attachNode:self.sourceNode];
    [self.audioEngine connect:self.sourceNode to:self.audioEngine.mainMixerNode format:format];
    [self.audioEngine connect:self.audioEngine.mainMixerNode to:self.audioEngine.outputNode format:format];

    [self.audioEngine prepare];
    [self.audioEngine startAndReturnError:&error];
    if (error) NSLog(@"Error starting audio engine: %@", error);
}

- (void)shutdown {
    if (self.audioEngine) {
        [self.audioEngine stop];
        self.audioEngine = nil;
    }
    self.sourceNode = nil;
}

- (void)playToneWithFrequency:(double)frequency duration:(double)duration {
    if (!self.engine) return;

    self.engine->setFrequency(frequency);
    self.engine->noteOn();

    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(duration / 1000.0 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        self.engine->noteOff();
    });
}

- (void)setFrequency:(double)frequency {
    if (self.engine) self.engine->setFrequency(frequency);
}

- (void)setLevel:(double)level {
    if (self.engine) self.engine->setLevel(level);
}

- (void)setWaveform:(NSString *)waveform {
    if (!self.engine) return;

    if ([waveform isEqualToString:@"square"]) {
        self.engine->setWaveform(Waveform::Square);
    } else if ([waveform isEqualToString:@"saw"]) {
        self.engine->setWaveform(Waveform::Saw);
    } else if ([waveform isEqualToString:@"triangle"]) {
        self.engine->setWaveform(Waveform::Triangle);
    } else {
        self.engine->setWaveform(Waveform::Sine);
    }
}

- (void)setHarmonicLevels:(NSArray<NSNumber *> *)levels {
    if (!self.engine) return;

    double harmonics[MAX_HARMONICS];
    for (int i = 0; i < MAX_HARMONICS; i++) {
        harmonics[i] = (i < (int)levels.count) ? levels[i].doubleValue : 0.0;
    }
    self.engine->setHarmonics(harmonics, (int)levels.count);
}

- (void)noteOn {
    if (self.engine) self.engine->noteOn();
}

- (void)noteOff {
    if (self.engine) self.engine->noteOff();
}

@end
