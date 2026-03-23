#import "JuceToneGenerator.h"
#import <AVFoundation/AVFoundation.h>
#include <atomic>
#include <cmath>
#include <algorithm>

#include "JuceConfig.h"
#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

// ── Waveform Types ────────────────────────────────────────────────
enum class Waveform { Sine, Square, Saw, Triangle };

static const int MAX_HARMONICS = 8;

// ── Synth Engine (JUCE DSP) ──────────────────────────────────────
//
// Thread safety contract:
//   - Main thread: calls set*() methods (atomic parameter updates)
//   - Audio thread: calls getNextSample() (reads atomics, mutates DSP state)
//   - DSP objects (oscillator, filter, adsr) are ONLY touched by the audio thread
//   - prepare() is called once from main thread BEFORE audio thread starts
//
class SynthEngine {
public:
    SynthEngine()
        : frequency(440.0), level(0.5),
          waveform(Waveform::Sine), detuneCents(0.0),
          pendingNoteOn(false), pendingNoteOff(false),
          filterCutoff(20000.0), filterResonance(0.0),
          attackMs(10.0), releaseMs(200.0),
          pendingParamUpdate(true)
    {
        for (int i = 0; i < MAX_HARMONICS; i++) {
            harmonics[i] = (i == 0) ? 1.0 : 0.0;
        }
    }

    // Called once from main thread before audio engine starts
    void prepare(double sampleRate, int blockSize) {
        juce::dsp::ProcessSpec spec;
        spec.sampleRate = sampleRate;
        spec.maximumBlockSize = (juce::uint32)blockSize;
        spec.numChannels = 1;

        // Pre-initialize all waveform oscillators with lookup tables (no allocation on audio thread)
        sineOsc.initialise([](float x) { return std::sin(x); }, 2048);
        sineOsc.prepare(spec);

        squareOsc.initialise([](float x) -> float { return x < 0.0f ? -1.0f : 1.0f; }, 2048);
        squareOsc.prepare(spec);

        sawOsc.initialise([](float x) -> float { return x / juce::MathConstants<float>::pi; }, 2048);
        sawOsc.prepare(spec);

        triOsc.initialise([](float x) -> float {
            return 2.0f * std::abs(x) / juce::MathConstants<float>::pi - 1.0f;
        }, 2048);
        triOsc.prepare(spec);

        filter.prepare(spec);
        filter.setType(juce::dsp::StateVariableTPTFilter<float>::Type::lowpass);

        adsr.setSampleRate(sampleRate);
        juce::ADSR::Parameters adsrParams { 0.01f, 0.0f, 1.0f, 0.2f };
        adsr.setParameters(adsrParams);

        cachedSampleRate = sampleRate;
        prepared = true;
    }

    // ── Main-thread setters (all atomic or lock-free) ─────────────

    void setFrequency(double freq) { frequency.store(freq); }
    void setLevel(double lev) { level.store(lev); }
    void setWaveform(Waveform wf) { waveform.store(wf); }
    void setDetune(double cents) { detuneCents.store(cents); }

    void setHarmonics(const double *levels, int count) {
        for (int i = 0; i < MAX_HARMONICS; i++) {
            harmonics[i].store((i < count) ? levels[i] : 0.0);
        }
    }

    void noteOn() {
        pendingNoteOn.store(true);
    }

    void noteOff() {
        pendingNoteOff.store(true);
    }

    void setAttack(double ms) {
        attackMs.store(std::max(1.0, ms));
        pendingParamUpdate.store(true);
    }

    void setRelease(double ms) {
        releaseMs.store(std::max(1.0, ms));
        pendingParamUpdate.store(true);
    }

    void setFilterCutoff(double hz) {
        filterCutoff.store(std::max(20.0, std::min(20000.0, hz)));
    }

    void setFilterResonance(double q) {
        filterResonance.store(std::max(0.0, std::min(1.0, q)));
    }

    // ── Audio-thread render ───────────────────────────────────────

    float getNextSample(double sampleRate) {
        if (!prepared) return 0.0f;

        // Apply pending note events (main thread -> audio thread)
        if (pendingNoteOn.exchange(false)) {
            sineOsc.reset();
            squareOsc.reset();
            sawOsc.reset();
            triOsc.reset();
            filter.reset();
            adsr.noteOn();
        }
        if (pendingNoteOff.exchange(false)) {
            adsr.noteOff();
        }

        // Apply pending ADSR parameter changes
        if (pendingParamUpdate.exchange(false)) {
            float atkSec = (float)(attackMs.load() * 0.001);
            float relSec = (float)(releaseMs.load() * 0.001);
            juce::ADSR::Parameters p { atkSec, 0.0f, 1.0f, relSec };
            adsr.setParameters(p);
        }

        // Get envelope value
        float envGain = adsr.getNextSample();
        if (envGain < 0.00001f && !adsr.isActive()) {
            return 0.0f;
        }

        // Read atomic parameters
        double freq = frequency.load();
        float lev = (float)level.load();
        Waveform wf = waveform.load();
        double detune = detuneCents.load();

        // Apply detune
        if (std::fabs(detune) > 0.01) {
            freq *= std::pow(2.0, detune / 1200.0);
        }

        // Set frequency on all oscillators (they track phase independently)
        float freqF = (float)freq;
        sineOsc.setFrequency(freqF, true);
        squareOsc.setFrequency(freqF, true);
        sawOsc.setFrequency(freqF, true);
        triOsc.setFrequency(freqF, true);

        // Update filter parameters (safe — JUCE TPT filter handles per-sample modulation)
        float cutoff = (float)filterCutoff.load();
        float resonance = (float)filterResonance.load();
        float mappedQ = 0.707f + resonance * 9.293f;
        filter.setCutoffFrequency(cutoff);
        filter.setResonance(mappedQ);

        // Generate sample from the active waveform's pre-initialized oscillator
        float sample = 0.0f;

        switch (wf) {
            case Waveform::Sine: {
                float baseSample = sineOsc.processSample(0.0f);
                float h0 = (float)harmonics[0].load();
                sample = baseSample * h0;

                // Additive harmonics (if any higher harmonics are set)
                for (int h = 1; h < MAX_HARMONICS; h++) {
                    float hLevel = (float)harmonics[h].load();
                    if (hLevel > 0.0001f) {
                        // Approximate higher harmonics from base phase
                        // This is safe: no allocation, just math
                        float harmSample = std::sin(std::asin(baseSample) * (float)(h + 1));
                        sample += harmSample * hLevel;
                    }
                }
                break;
            }

            case Waveform::Square:
                sample = squareOsc.processSample(0.0f) * 0.7f;
                break;

            case Waveform::Saw:
                sample = sawOsc.processSample(0.0f) * 0.6f;
                break;

            case Waveform::Triangle:
                sample = triOsc.processSample(0.0f) * 0.8f;
                break;
        }

        // Advance inactive oscillators to keep phase tracking coherent
        // (prevents phase discontinuity on waveform switch)
        if (wf != Waveform::Sine)    sineOsc.processSample(0.0f);
        if (wf != Waveform::Square)  squareOsc.processSample(0.0f);
        if (wf != Waveform::Saw)     sawOsc.processSample(0.0f);
        if (wf != Waveform::Triangle) triOsc.processSample(0.0f);

        // Apply filter
        float filtered = filter.processSample(0, sample);

        return filtered * lev * envGain;
    }

private:
    // Atomic parameters (written by main thread, read by audio thread)
    std::atomic<double> frequency;
    std::atomic<double> level;
    std::atomic<Waveform> waveform;
    std::atomic<double> detuneCents;
    std::atomic<double> harmonics[MAX_HARMONICS];
    std::atomic<bool> pendingNoteOn;
    std::atomic<bool> pendingNoteOff;
    std::atomic<double> filterCutoff;
    std::atomic<double> filterResonance;
    std::atomic<double> attackMs;
    std::atomic<double> releaseMs;
    std::atomic<bool> pendingParamUpdate;

    // DSP objects (ONLY touched by audio thread after prepare())
    juce::dsp::Oscillator<float> sineOsc;
    juce::dsp::Oscillator<float> squareOsc;
    juce::dsp::Oscillator<float> sawOsc;
    juce::dsp::Oscillator<float> triOsc;
    juce::dsp::StateVariableTPTFilter<float> filter;
    juce::ADSR adsr;
    double cachedSampleRate = 44100.0;
    bool prepared = false;
};

// ── Transport Engine ──────────────────────────────────────────────
// All-atomic transport — safe for cross-thread access.
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

    // Returns true if we crossed a new beat boundary (audio thread only)
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

// ── Beat State (lock-free audio->main thread communication) ──────
struct BeatState {
    std::atomic<bool> crossed{false};
    std::atomic<double> beat{0.0};
    std::atomic<double> bpm{120.0};
};

// ── Objective-C Wrapper ───────────────────────────────────────────

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

            // Prepare JUCE DSP objects on main thread BEFORE audio thread starts
            self.engine->prepare(sampleRate, 512);

            SynthEngine *eng = self.engine;
            TransportEngine *trans = self.transport;

            if (!self.beatState) {
                self.beatState = new BeatState();
            }
            BeatState *bs = self.beatState;

            self.sourceNode = [[AVAudioSourceNode alloc] initWithRenderBlock:^OSStatus(
                BOOL *isSilence,
                const AudioTimeStamp *timestamp,
                AVAudioFrameCount frameCount,
                AudioBufferList *outputData
            ) {
                float *leftChannel = (float *)outputData->mBuffers[0].mData;
                float *rightChannel = (outputData->mNumberBuffers > 1)
                    ? (float *)outputData->mBuffers[1].mData
                    : leftChannel;

                for (AVAudioFrameCount i = 0; i < frameCount; i++) {
                    // Advance transport
                    if (trans->advance(sampleRate)) {
                        bs->crossed.store(true);
                        bs->beat.store(trans->getPosition());
                        bs->bpm.store(trans->getTempo());
                    }

                    // Render synth sample
                    float sample = eng->getNextSample(sampleRate);
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

            NSLog(@"[ExpoJuce] Audio engine started successfully (JUCE DSP active)");
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
    if (self.transport) self.transport->start();
}

- (void)stopTransport {
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
