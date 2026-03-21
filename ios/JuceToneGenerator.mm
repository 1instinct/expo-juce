#import "JuceToneGenerator.h"
#import <AVFoundation/AVFoundation.h>
#include <atomic>
#include <cmath>
#include <algorithm>

// ── Waveform Types ────────────────────────────────────────────────
enum class Waveform { Sine, Square, Saw, Triangle };

static const int MAX_HARMONICS = 8;

// ── Simple one-pole low-pass filter ───────────────────────────────
class OnePoleLPF {
public:
    OnePoleLPF() : y1(0.0), cutoff(20000.0), resonance(0.0) {}

    void setCutoff(double hz) { cutoff.store(hz); }
    void setResonance(double q) { resonance.store(q); }

    float process(float input, double sampleRate) {
        double fc = cutoff.load();

        // Clamp cutoff to Nyquist
        if (fc > sampleRate * 0.49) fc = sampleRate * 0.49;
        if (fc < 20.0) fc = 20.0;

        // Compute coefficient (simple RC low-pass)
        double w = 2.0 * M_PI * fc / sampleRate;
        double g = w / (1.0 + w);

        // One-pole filter
        y1 = y1 + g * (input - y1);

        return (float)y1;
    }

    void reset() { y1 = 0.0; }

private:
    double y1;
    std::atomic<double> cutoff;
    std::atomic<double> resonance;
};

// ── ADSR Envelope ─────────────────────────────────────────────────
class ADSREnvelope {
public:
    enum class Stage { Idle, Attack, Sustain, Release };

    ADSREnvelope()
        : attackMs(10.0), releaseMs(200.0),
          stage(Stage::Idle), gain(0.0) {}

    void setAttack(double ms) { attackMs.store(std::max(1.0, ms)); }
    void setRelease(double ms) { releaseMs.store(std::max(1.0, ms)); }

    void noteOn() { stage.store(Stage::Attack); }
    void noteOff() { stage.store(Stage::Release); }
    bool isActive() { return stage.load() != Stage::Idle; }

    float process(double sampleRate) {
        Stage s = stage.load();

        switch (s) {
            case Stage::Idle:
                gain = 0.0;
                break;

            case Stage::Attack: {
                double rate = 1.0 / (attackMs.load() * 0.001 * sampleRate);
                gain += rate;
                if (gain >= 1.0) {
                    gain = 1.0;
                    stage.store(Stage::Sustain);
                }
                break;
            }

            case Stage::Sustain:
                gain = 1.0;
                break;

            case Stage::Release: {
                double rate = 1.0 / (releaseMs.load() * 0.001 * sampleRate);
                gain -= rate;
                if (gain <= 0.0) {
                    gain = 0.0;
                    stage.store(Stage::Idle);
                }
                break;
            }
        }

        return (float)gain;
    }

    void reset() {
        gain = 0.0;
        stage.store(Stage::Idle);
    }

private:
    std::atomic<double> attackMs;
    std::atomic<double> releaseMs;
    std::atomic<Stage> stage;
    double gain;
};

// ── Synth Engine ──────────────────────────────────────────────────
class SynthEngine {
public:
    SynthEngine()
        : frequency(440.0), level(0.5), phase(0.0),
          waveform(Waveform::Sine), detuneCents(0.0)
    {
        for (int i = 0; i < MAX_HARMONICS; i++) {
            harmonics[i] = (i == 0) ? 1.0 : 0.0;
        }
    }

    void setFrequency(double freq) { frequency = freq; }
    void setLevel(double lev) { level = lev; }
    void setWaveform(Waveform wf) { waveform.store(wf); }
    void setDetune(double cents) { detuneCents.store(cents); }

    void setHarmonics(const double *levels, int count) {
        for (int i = 0; i < MAX_HARMONICS; i++) {
            harmonics[i] = (i < count) ? levels[i] : 0.0;
        }
    }

    void noteOn() {
        phase = 0.0;
        filter.reset();
        envelope.noteOn();
    }

    void noteOff() {
        envelope.noteOff();
    }

    void setAttack(double ms) { envelope.setAttack(ms); }
    void setRelease(double ms) { envelope.setRelease(ms); }
    void setFilterCutoff(double hz) { filter.setCutoff(hz); }
    void setFilterResonance(double q) { filter.setResonance(q); }

    float getNextSample(double sampleRate) {
        float envGain = envelope.process(sampleRate);
        if (envGain < 0.00001f && !envelope.isActive()) {
            return 0.0f;
        }

        double freq = frequency.load();
        double lev = level.load();
        Waveform wf = waveform.load();
        double detune = detuneCents.load();

        // Apply detune (cents to frequency ratio)
        if (std::fabs(detune) > 0.01) {
            freq *= std::pow(2.0, detune / 1200.0);
        }

        double sample = 0.0;

        switch (wf) {
            case Waveform::Sine:
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

        // Apply filter
        float filtered = filter.process((float)sample, sampleRate);

        return filtered * (float)lev * envGain;
    }

private:
    std::atomic<double> frequency;
    std::atomic<double> level;
    double phase;
    std::atomic<Waveform> waveform;
    std::atomic<double> detuneCents;
    std::atomic<double> harmonics[MAX_HARMONICS];
    ADSREnvelope envelope;
    OnePoleLPF filter;
};

// ── Transport Engine ──────────────────────────────────────────────
class TransportEngine {
public:
    TransportEngine()
        : bpm(120.0), beatPosition(0.0),
          playing(false), lastBeatInt(-1) {}

    void setTempo(double newBpm) { bpm.store(std::max(20.0, std::min(400.0, newBpm))); }
    double getTempo() { return bpm.load(); }

    void setPosition(double beat) { beatPosition.store(beat); lastBeatInt.store((int)beat); }
    double getPosition() { return beatPosition.load(); }

    void start() { playing.store(true); }
    void stop() { playing.store(false); }
    bool isPlaying() { return playing.load(); }

    // Returns true if we crossed a new beat boundary
    bool advance(double sampleRate) {
        if (!playing.load()) return false;

        double currentBpm = bpm.load();
        double beatsPerSample = currentBpm / (60.0 * sampleRate);
        double newPos = beatPosition.load() + beatsPerSample;
        beatPosition.store(newPos);

        int currentBeatInt = (int)newPos;
        int lastBeat = lastBeatInt.load();
        if (currentBeatInt != lastBeat) {
            lastBeatInt.store(currentBeatInt);
            return true;
        }
        return false;
    }

private:
    std::atomic<double> bpm;
    std::atomic<double> beatPosition;
    std::atomic<bool> playing;
    std::atomic<int> lastBeatInt;
};

// ── Objective-C Wrapper ───────────────────────────────────────────

struct BeatState {
    std::atomic<bool> crossed{false};
    std::atomic<double> beat{0.0};
    std::atomic<double> bpm{120.0};
};

@interface JuceToneGenerator ()
@property (nonatomic, strong) AVAudioEngine *audioEngine;
@property (nonatomic, strong) AVAudioSourceNode *sourceNode;
@property (nonatomic, assign) SynthEngine *engine;
@property (nonatomic, assign) TransportEngine *transport;
@property (nonatomic, assign) BeatState *beatState;
@property (nonatomic, strong) dispatch_source_t beatTimer;
@property (nonatomic, copy) BeatCallback beatCallback;
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
        _transport = new TransportEngine();
        // Audio engine initialized lazily via [self initialize] called from setup
    }
    return self;
}

- (void)dealloc {
    [self shutdown];
    if (_engine) { delete _engine; _engine = nullptr; }
    if (_transport) { delete _transport; _transport = nullptr; }
    if (_beatState) { delete _beatState; _beatState = nullptr; }
}

- (void)initialize {
    [self initializeWithRetries:3];
}

- (void)initializeWithRetries:(int)maxRetries {
    if (self.audioEngine) {
        NSLog(@"[ExpoJuce] Audio engine already initialized, skipping");
        return;
    }

    // Listen for audio interruptions (phone calls, Siri, etc.)
    [[NSNotificationCenter defaultCenter] removeObserver:self
                                                    name:AVAudioSessionInterruptionNotification
                                                  object:nil];
    [[NSNotificationCenter defaultCenter] addObserver:self
                                             selector:@selector(handleAudioInterruption:)
                                                 name:AVAudioSessionInterruptionNotification
                                               object:[AVAudioSession sharedInstance]];

    for (int attempt = 1; attempt <= maxRetries; attempt++) {
        NSLog(@"[ExpoJuce] Initializing audio engine (attempt %d/%d)...", attempt, maxRetries);

        @try {
            NSError *error = nil;

            AVAudioSession *audioSession = [AVAudioSession sharedInstance];
            [audioSession setCategory:AVAudioSessionCategoryPlayback
                          withOptions:AVAudioSessionCategoryOptionMixWithOthers
                                error:&error];
            if (error) {
                NSLog(@"[ExpoJuce] Error setting audio session category: %@", error);
                continue;
            }

            [audioSession setActive:YES error:&error];
            if (error) {
                NSLog(@"[ExpoJuce] Error activating audio session: %@", error);
                continue;
            }

            self.audioEngine = [[AVAudioEngine alloc] init];

            double sampleRate = audioSession.sampleRate > 0 ? audioSession.sampleRate : 44100.0;
            AVAudioFormat *format = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:sampleRate channels:2];
            NSLog(@"[ExpoJuce] Using sample rate: %.0f", sampleRate);

            SynthEngine *eng = self.engine;
            TransportEngine *trans = self.transport;

            if (!self.beatState) {
                self.beatState = new BeatState();
            }
            BeatState *bs = self.beatState;

            self.sourceNode = [[AVAudioSourceNode alloc] initWithRenderBlock:^OSStatus(BOOL *isSilence, const AudioTimeStamp *timestamp, AVAudioFrameCount frameCount, AudioBufferList *outputData) {
                double sr = sampleRate;
                float *leftChannel = (float *)outputData->mBuffers[0].mData;
                float *rightChannel = (outputData->mNumberBuffers > 1) ? (float *)outputData->mBuffers[1].mData : leftChannel;

                for (AVAudioFrameCount i = 0; i < frameCount; i++) {
                    if (trans->advance(sr)) {
                        bs->crossed.store(true);
                        bs->beat.store(trans->getPosition());
                        bs->bpm.store(trans->getTempo());
                    }

                    float sample = eng->getNextSample(sr);
                    leftChannel[i] = sample;
                    rightChannel[i] = sample;
                }

                return noErr;
            }];

            [self.audioEngine attachNode:self.sourceNode];
            [self.audioEngine connect:self.sourceNode to:self.audioEngine.mainMixerNode format:format];
            [self.audioEngine connect:self.audioEngine.mainMixerNode to:self.audioEngine.outputNode format:nil];

            [self.audioEngine prepare];
            [self.audioEngine startAndReturnError:&error];
            if (error) {
                NSLog(@"[ExpoJuce] Error starting audio engine: %@", error);
                [self tearDownAudioEngine];
                continue;
            }

            NSLog(@"[ExpoJuce] Audio engine started successfully");
            return;

        } @catch (NSException *exception) {
            NSLog(@"[ExpoJuce] CRASH in audio init (attempt %d): %@ — %@", attempt, exception.name, exception.reason);
            [self tearDownAudioEngine];
        }
    }

    NSLog(@"[ExpoJuce] Audio engine failed to initialize after %d attempts", maxRetries);
}

- (void)tearDownAudioEngine {
    if (self.audioEngine) {
        [self.audioEngine stop];
        self.audioEngine = nil;
    }
    self.sourceNode = nil;
}

- (void)handleAudioInterruption:(NSNotification *)notification {
    NSDictionary *info = notification.userInfo;
    AVAudioSessionInterruptionType type = (AVAudioSessionInterruptionType)[info[AVAudioSessionInterruptionTypeKey] unsignedIntegerValue];

    if (type == AVAudioSessionInterruptionTypeBegan) {
        NSLog(@"[ExpoJuce] Audio interrupted (phone call, Siri, etc.)");
    } else if (type == AVAudioSessionInterruptionTypeEnded) {
        NSLog(@"[ExpoJuce] Audio interruption ended, restarting engine...");
        NSError *error = nil;
        [self.audioEngine startAndReturnError:&error];
        if (error) {
            NSLog(@"[ExpoJuce] Failed to restart after interruption: %@, reinitializing...", error);
            [self tearDownAudioEngine];
            [self initializeWithRetries:2];
        }
    }
}

- (void)stopBeatTimer {
    if (self.beatTimer) {
        dispatch_source_cancel(self.beatTimer);
        self.beatTimer = nil;
    }
    self.beatCallback = nil;
}

- (void)startBeatTimer {
    if (self.beatTimer) return;
    if (!self.beatState) return;

    BeatState *bs = self.beatState;
    self.beatTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
    dispatch_source_set_timer(self.beatTimer, DISPATCH_TIME_NOW, 16 * NSEC_PER_MSEC, 1 * NSEC_PER_MSEC);
    __weak JuceToneGenerator *weakSelf = self;
    dispatch_source_set_event_handler(self.beatTimer, ^{
        JuceToneGenerator *strongSelf = weakSelf;
        if (!strongSelf) return;
        BeatCallback cb = strongSelf.beatCallback;
        if (bs->crossed.exchange(false) && cb) {
            cb(bs->beat.load(), bs->bpm.load());
        }
    });
    dispatch_resume(self.beatTimer);
}

- (void)shutdown {
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [self stopBeatTimer];
    if (self.audioEngine) {
        [self.audioEngine stop];
        self.audioEngine = nil;
    }
    self.sourceNode = nil;
}

// ── Synth Methods ─────────────────────────────────────────────────

- (void)playToneWithFrequency:(double)frequency duration:(double)duration {
    NSLog(@"[ExpoJuce] playTone freq=%.1f dur=%.0f", frequency, duration);
    if (!self.engine) return;

    self.engine->setFrequency(frequency);
    self.engine->noteOn();

    __weak JuceToneGenerator *weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(duration / 1000.0 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        if (weakSelf.engine) weakSelf.engine->noteOff();
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

// ── DSP Params ────────────────────────────────────────────────────

- (void)setAttack:(double)ms {
    if (self.engine) self.engine->setAttack(ms);
}

- (void)setRelease:(double)ms {
    if (self.engine) self.engine->setRelease(ms);
}

- (void)setFilterCutoff:(double)hz {
    if (self.engine) self.engine->setFilterCutoff(hz);
}

- (void)setFilterResonance:(double)q {
    if (self.engine) self.engine->setFilterResonance(q);
}

- (void)setDetune:(double)cents {
    if (self.engine) self.engine->setDetune(cents);
}

// ── Transport ─────────────────────────────────────────────────────

- (void)startTransport {
    NSLog(@"[ExpoJuce] Transport start");
    if (self.transport) self.transport->start();
}

- (void)stopTransport {
    NSLog(@"[ExpoJuce] Transport stop");
    if (self.transport) self.transport->stop();
}

- (void)setTempo:(double)bpm {
    if (self.transport) self.transport->setTempo(bpm);
}

- (void)setTransportPosition:(double)beat {
    if (self.transport) self.transport->setPosition(beat);
}

- (double)getTransportPosition {
    return self.transport ? self.transport->getPosition() : 0.0;
}

- (double)getTempo {
    return self.transport ? self.transport->getTempo() : 120.0;
}

- (BOOL)isTransportPlaying {
    return self.transport ? self.transport->isPlaying() : NO;
}

- (void)setBeatCallback:(BeatCallback)callback {
    self.beatCallback = callback;
}

@end
