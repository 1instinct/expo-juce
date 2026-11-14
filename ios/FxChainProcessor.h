#pragma once

// Pure C++ (no Objective-C): compiled into the iOS module and by the offline harness
// (test/harness), which renders it with allocation counting.

#include "JuceConfig.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include "TransportEngine.h"
#include "SourceRing.h"
#include <atomic>
#include <cstdint>

// ── Effect table ──
// Fixed processing order (index order): drive → crush → eq → chorus → phaser → echo → reverb →
// pump → compressor, then an always-on safety limiter (not user-facing, not in this table).

enum FxIndex : int {
    kFxDrive = 0, kFxCrush, kFxEq, kFxChorus, kFxPhaser, kFxEcho, kFxReverb, kFxPump, kFxCompressor,
    kFxCount
};
constexpr int kFxMaxParams = 7;

struct FxParamSpec {
    const char* name;
    float minValue, maxValue, defaultValue;
    bool integer; // stored rounded (switches and division indices)
};

struct FxSpec {
    const char* id;
    int numParams;
    FxParamSpec params[kFxMaxParams];
};

// Parameter indices, in table order.
enum FxDriveParam : int { kDriveAmount = 0, kDriveTone, kDriveMix };
enum FxCrushParam : int { kCrushBits = 0, kCrushDownsample, kCrushNoise, kCrushMix };
enum FxEqParam : int { kEqLow = 0, kEqMid, kEqMidFreq, kEqHigh };
enum FxChorusParam : int { kChorusRate = 0, kChorusDepth, kChorusMix };
enum FxPhaserParam : int { kPhaserRate = 0, kPhaserDepth, kPhaserFeedback, kPhaserMix };
enum FxEchoParam : int { kEchoTime = 0, kEchoSync, kEchoDivision, kEchoFeedback, kEchoTone, kEchoPingPong, kEchoMix };
enum FxReverbParam : int { kReverbSize = 0, kReverbDamping, kReverbWidth, kReverbMix };
enum FxPumpParam : int { kPumpDepth = 0, kPumpShape, kPumpDivision };
enum FxCompressorParam : int { kCompThreshold = 0, kCompRatio, kCompAttack, kCompRelease, kCompMakeup };

inline constexpr FxSpec kFxSpecs[kFxCount] = {
    { "drive", 3, { { "amount", 0.0f, 1.0f, 0.4f, false },
                    { "tone", 0.0f, 1.0f, 0.6f, false },
                    { "mix", 0.0f, 1.0f, 1.0f, false } } },
    { "crush", 4, { { "bits", 2.0f, 16.0f, 8.0f, false },
                    { "downsample", 1.0f, 32.0f, 4.0f, false },
                    { "noise", 0.0f, 1.0f, 0.1f, false },
                    { "mix", 0.0f, 1.0f, 1.0f, false } } },
    { "eq", 4, { { "low", -12.0f, 12.0f, 0.0f, false },
                 { "mid", -12.0f, 12.0f, 0.0f, false },
                 { "midFreq", 200.0f, 5000.0f, 1000.0f, false },
                 { "high", -12.0f, 12.0f, 0.0f, false } } },
    { "chorus", 3, { { "rate", 0.05f, 5.0f, 0.8f, false },
                     { "depth", 0.0f, 1.0f, 0.4f, false },
                     { "mix", 0.0f, 1.0f, 0.5f, false } } },
    { "phaser", 4, { { "rate", 0.05f, 5.0f, 0.4f, false },
                     { "depth", 0.0f, 1.0f, 0.6f, false },
                     { "feedback", 0.0f, 0.9f, 0.5f, false },
                     { "mix", 0.0f, 1.0f, 0.5f, false } } },
    { "echo", 7, { { "time", 0.02f, 2.0f, 0.375f, false },
                   { "sync", 0.0f, 1.0f, 1.0f, true },
                   { "division", 0.0f, 5.0f, 2.0f, true },
                   { "feedback", 0.0f, 0.95f, 0.4f, false },
                   { "tone", 0.0f, 1.0f, 0.5f, false },
                   { "pingPong", 0.0f, 1.0f, 0.0f, true },
                   { "mix", 0.0f, 1.0f, 0.35f, false } } },
    { "reverb", 4, { { "size", 0.0f, 1.0f, 0.6f, false },
                     { "damping", 0.0f, 1.0f, 0.5f, false },
                     { "width", 0.0f, 1.0f, 1.0f, false },
                     { "mix", 0.0f, 1.0f, 0.3f, false } } },
    { "pump", 3, { { "depth", 0.0f, 1.0f, 0.6f, false },
                   { "shape", 0.0f, 1.0f, 0.5f, false },
                   { "division", 0.0f, 3.0f, 0.0f, true } } },
    { "compressor", 5, { { "threshold", -60.0f, 0.0f, -18.0f, false },
                         { "ratio", 1.0f, 20.0f, 4.0f, false },
                         { "attack", 0.1f, 100.0f, 10.0f, false },
                         { "release", 10.0f, 1000.0f, 120.0f, false },
                         { "makeup", 0.0f, 24.0f, 4.0f, false } } },
};

// Echo `division` index → beats: 1/4, 1/8, 1/8 dotted, 1/16, 1/4 dotted, 1/2.
inline constexpr double kEchoDivisionBeats[6] = { 1.0, 0.5, 0.75, 0.25, 1.5, 2.0 };
// Pump `division` index → beats: 1/4, 1/8, 1/2, 1/1.
inline constexpr double kPumpDivisionBeats[4] = { 1.0, 0.5, 2.0, 4.0 };

// Plain copy of every effect's state (enabled + all parameter values, after clamping).
struct FxSettings {
    bool enabled[kFxCount];
    float values[kFxCount][kFxMaxParams];
    static FxSettings defaults();
    bool operator==(const FxSettings& other) const;
};

// The effect settings as the app configured them: atomics written by any thread, read by the
// audio thread once per processing chunk. The engine keeps one for the life of the process
// (like its NoteEventQueue) and attaches it to every FxChainProcessor it builds, so settings
// made before setup, while a rebuild is in progress, or before a failed rebuild is retried are
// all applied to the next processor without being captured or restored.
class FxParamStore {
public:
    FxParamStore() { resetToDefaults(); }

    // -1 if unknown.
    static int effectIndex(const char* id);
    static int paramIndex(int fx, const char* name);

    // Any thread. Unknown id / param, or a non-finite value: no-op, returns false.
    // Values are clamped to the parameter's range (switches and divisions are rounded).
    bool setEnabled(const char* id, bool enabled);
    bool setParam(const char* id, const char* param, double value);
    void setEnabled(int fx, bool enabled);
    void setParam(int fx, int param, double value);

    bool isEnabled(int fx) const { return enabled[fx].load(std::memory_order_relaxed); }
    float get(int fx, int param) const { return values[fx][param].load(std::memory_order_relaxed); }

    FxSettings snapshot() const;
    void apply(const FxSettings& s);
    void resetToDefaults();

private:
    std::atomic<bool> enabled[kFxCount];
    std::atomic<float> values[kFxCount][kFxMaxParams];
};

// Audio-thread counters, read from any thread (relaxed).
struct FxDiagnostics {
    std::atomic<uint64_t> processCount{0};
    std::atomic<float> limiterGain{1.0f};     // lowest limiter gain in the last block (1 = idle)
    std::atomic<uint64_t> nonFiniteResets{0}; // chunks whose output went non-finite (effects reset)
};

// One graph node, stereo in → stereo out, running the effects in the fixed order above.
//
// Real-time rules: everything is allocated in prepareToPlay; processBlock never allocates,
// locks or logs. The host may deliver blocks larger than the prepared block size (GraphPlayer
// renders up to its own capacity), so processBlock works in chunks of at most kChunk samples
// and every dsp object is prepared for kChunk, whatever samplesPerBlock says.
//
// Bypass: a disabled effect is skipped entirely (the signal is not touched). Turning an
// effect on or off crossfades it over kToggleRampSeconds; an effect that is turned on starts
// from a cleared state (no stale echo or reverb tail). Echo and reverb spill over: turning
// them off fades their INPUT out instead, and they keep running on silence, adding their tail
// to the passing signal, until the tail has stayed under kTailFloor (-90 dBFS) for a while
// (one echo period, or 100 ms for the reverb) or kMaxTailSeconds have passed (the last
// kTailFadeSeconds fade it out); only then are they bypassed. With every effect off (tails
// ended) and the signal below the limiter ceiling, the output is bit-exact equal to the input.
//
// Limiter: a stereo-linked peak limiter with instant attack and a 50 ms release, ceiling
// -0.3 dBFS. Its gain is exactly 1 (no multiply) while the signal stays under the ceiling.
// juce::dsp::Limiter is not used: it is a two-stage compressor with makeup gain (a loudness
// maximiser: about +4 dB of output gain at a -0.3 dB threshold), which would colour the dry signal.
class FxChainProcessor : public juce::AudioProcessor {
public:
    static constexpr int kChunk = 256;
    static constexpr double kToggleRampSeconds = 0.010;
    static constexpr double kParamRampSeconds = 0.050;
    static constexpr double kMaxEchoSeconds = 2.0;
    static constexpr float kLimiterCeilingDb = -0.3f;
    static constexpr float kTailFloor = 3.1623e-5f;    // -90 dBFS
    static constexpr double kMaxTailSeconds = 5.0;
    static constexpr double kTailFadeSeconds = 0.050;

    FxChainProcessor();
    ~FxChainProcessor() override = default;

    // The parameter store this chain reads: its own by default; the engine passes its
    // process-lifetime store before the node is added to the graph. nullptr = own store.
    void setParamStore(FxParamStore* s) { store = s ? s : &ownStore; }
    FxParamStore& params() { return *store; }

    // Tempo and beat position for echo sync and pump. Set before the node renders.
    void setTransport(TransportEngine* t) { transport = t; }

    // Instrument tap for getSourceAnalysis: the chain's output (after the limiter) is written into
    // it at the end of every block (ring writes only). nullptr = none. Set before the node renders.
    void setSourceRing(SourceRing* r) { sourceRing = r; }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    const juce::String getName() const override { return "FxChain"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}

    FxDiagnostics diag;

    // Audio thread (or a test driving processBlock): the effect is processed (on, fading, or
    // playing out its tail after being turned off).
    bool isEffectRunning(int fx) const { return fx >= 0 && fx < kFxCount && toggles[fx].running; }

private:
    using Smoothed = juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>;

    struct Toggle {
        Smoothed level;       // 0 = bypassed, 1 = fully in (echo / reverb: their input gain)
        bool running = false; // processed this chunk (on, fading out, or playing its tail)
        int64_t tailSamples = 0;  // echo / reverb: samples rendered with the input muted
        int64_t quietSamples = 0; // … of which the latest run under kTailFloor
    };

    static bool spillsOver(int fx) { return fx == kFxEcho || fx == kFxReverb; }
    void runSpilling(int fx, Toggle& t, float* left, float* right, int n, double beatStart, double beatsPerSample,
                     bool playing);

    void processChunk(float* left, float* right, int n, double beatStart, double beatsPerSample, bool playing);
    void resetEffect(int fx);
    void loadTargets(int fx, bool snap);
    void runEffect(int fx, float* left, float* right, int n, double beatStart, double beatsPerSample, bool playing);

    void runDrive(float* left, float* right, int n);
    void runCrush(float* left, float* right, int n);
    void runEq(float* left, float* right, int n);
    void runChorus(float* left, float* right, int n);
    void runPhaser(float* left, float* right, int n);
    void runEcho(float* left, float* right, int n, double bpm);
    void runReverb(float* left, float* right, int n);
    void runPump(float* left, float* right, int n, double beatStart, double beatsPerSample, bool playing);
    void runCompressor(float* left, float* right, int n);
    float runLimiter(float* left, float* right, int n);

    FxParamStore ownStore;
    FxParamStore* store = &ownStore;
    TransportEngine* transport = nullptr;
    SourceRing* sourceRing = nullptr;

    double sr = 48000.0;
    bool prepared = false;
    Toggle toggles[kFxCount];
    float dryL[kChunk], dryR[kChunk], fadeGain[kChunk];

    // drive
    Smoothed drivePre, drivePost, driveMix;
    float driveToneCoeff = 1.0f, driveLp[2] = { 0.0f, 0.0f };
    // crush
    Smoothed crushNoise, crushMix;
    int crushCounter = 0;
    float crushHeld[2] = { 0.0f, 0.0f };
    uint32_t crushRng = 0x9E3779B9u;
    // eq
    Smoothed eqLowDb, eqMidDb, eqMidFreq, eqHighDb;
    float eqApplied[4] = { 1e9f, 1e9f, 1e9f, 1e9f };
    juce::dsp::IIR::Filter<float> eqLow[2], eqMid[2], eqHigh[2];
    // chorus / phaser / reverb / compressor: last values handed to the dsp object
    juce::dsp::Chorus<float> chorus;
    float chorusApplied[3] = { -1.0f, -1.0f, -1.0f };
    juce::dsp::Phaser<float> phaser;
    float phaserApplied[4] = { -1.0f, -1.0f, -1.0f, -1.0f };
    // echo
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> echoLine;
    int echoMaxDelaySamples = 0;
    // Samples pushed since the echo was last reset. Enabling the echo does not clear the line
    // (a memset of up to 2 s × 96 kHz per channel on the audio thread); reads older than this
    // return silence instead, which is what a cleared line would give.
    int echoFilled = 0;
    Smoothed echoDelay, echoFeedback, echoMix;
    float echoToneCoeff = 1.0f, echoLp[2] = { 0.0f, 0.0f };
    bool echoSnap = true; // jump to the delay time on the next chunk (set on enable)
    // reverb
    juce::dsp::Reverb reverb;
    float reverbApplied[4] = { -1.0f, -1.0f, -1.0f, -1.0f };
    // pump
    Smoothed pumpDepth;
    double pumpFreeBeat = 0.0; // free-running beat while the transport is stopped
    // compressor
    juce::dsp::Compressor<float> compressor;
    float compApplied[4] = { 1e9f, 1e9f, 1e9f, 1e9f };
    Smoothed compMakeup;
    // limiter
    float limiterGain = 1.0f, limiterRelease = 0.0f, limiterCeiling = 1.0f;

    JUCE_DECLARE_NON_COPYABLE(FxChainProcessor)
};
