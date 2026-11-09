// JNI layer for expo.modules.juce.ExpoJuceNative (ExpoJuceNative.kt). Forwards primitives and
// strings to AndroidEngineHost; maps / JSON stay on the Kotlin side.
//
// JUCE owns JNI_OnLoad (juce_core/native/juce_Threads_android.cpp): when the library is loaded it
// stores the JavaVM and looks for com.rmsl.juce.Java, which this package does not ship (that
// class would load a library called "juce_jni"). JUCE is therefore initialised explicitly, by
// nativeInit, which Kotlin calls once on the Android main thread before the first setup.

#include <jni.h>

#include "JuceConfig.h"
#define JUCE_CORE_INCLUDE_JNI_HELPERS 1
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include "AndroidEngineHost.h"

#include <atomic>
#include <string>

namespace {

std::atomic<bool> sJuceInitialised{false};

std::string toStdString(JNIEnv* env, jstring value) {
    if (value == nullptr) return {};
    const char* chars = env->GetStringUTFChars(value, nullptr);
    if (chars == nullptr) return {};
    std::string result(chars);
    env->ReleaseStringUTFChars(value, chars);
    return result;
}

jstring toJString(JNIEnv* env, const std::string& value) {
    return env->NewStringUTF(value.c_str());
}

// A C++ exception must not unwind into the JVM: report it as a Java RuntimeException instead.
void throwJava(JNIEnv* env, const char* what) {
    if (env->ExceptionCheck()) return;
    if (jclass type = env->FindClass("java/lang/RuntimeException")) env->ThrowNew(type, what);
}

template <typename Result, typename Body>
Result guarded(JNIEnv* env, Result fallback, Body&& body) {
    try {
        return body();
    } catch (const std::exception& e) {
        throwJava(env, e.what());
    } catch (...) {
        throwJava(env, "native exception");
    }
    return fallback;
}

template <typename Body>
void guardedVoid(JNIEnv* env, Body&& body) {
    guarded<int>(env, 0, [&] { body(); return 0; });
}

} // namespace

#define EXPOJUCE_JNI(returnType, name) \
    extern "C" JNIEXPORT returnType JNICALL Java_expo_modules_juce_ExpoJuceNative_##name

// Main thread only. JUCE's Android message queue posts to a Handler bound to the calling thread's
// Looper, so creating the MessageManager here makes the main thread JUCE's message thread; without
// a running Looper on that thread the graph's async rebuilds never run and the output is silent.
EXPOJUCE_JNI(jboolean, nativeInit)(JNIEnv* env, jclass, jobject context) {
    if (sJuceInitialised.load(std::memory_order_acquire)) return JNI_TRUE;
    if (context == nullptr) return JNI_FALSE;
    return guarded<jboolean>(env, JNI_FALSE, [&] {
        juce::JNIClassBase::initialiseAllClasses(env, context);
        juce::Thread::initialiseJUCE(env, context);
        juce::MessageManager::getInstance();
        juce::initialiseJuce_GUI();
        sJuceInitialised.store(true, std::memory_order_release);
        return JNI_TRUE;
    });
}

EXPOJUCE_JNI(jboolean, nativeSetup)(JNIEnv* env, jclass, jint bufferSize, jdouble sampleRate, jdouble nativeSampleRate) {
    if (!sJuceInitialised.load(std::memory_order_acquire)) return JNI_FALSE;
    return guarded<jboolean>(env, JNI_FALSE, [&] {
        return expojuce::host::setup((int)bufferSize, (double)sampleRate, (double)nativeSampleRate) ? JNI_TRUE : JNI_FALSE;
    });
}

EXPOJUCE_JNI(jstring, nativeSetupErrorMessage)(JNIEnv* env, jclass) {
    if (!sJuceInitialised.load(std::memory_order_acquire)) return toJString(env, "step -1: JUCE is not initialised");
    return toJString(env, expojuce::host::setupErrorMessage());
}

EXPOJUCE_JNI(void, nativeShutdown)(JNIEnv* env, jclass) {
    if (!sJuceInitialised.load(std::memory_order_acquire)) return;
    guardedVoid(env, [] { expojuce::host::shutdown(); });
}

EXPOJUCE_JNI(jstring, nativeDiagnosticsJson)(JNIEnv* env, jclass) {
    return guarded<jstring>(env, nullptr, [&] { return toJString(env, expojuce::host::diagnosticsJson()); });
}

EXPOJUCE_JNI(jboolean, nativePlayTestTone)(JNIEnv* env, jclass, jdouble durationMs) {
    return guarded<jboolean>(env, JNI_FALSE, [&] {
        return expojuce::host::playTestTone((double)durationMs) ? JNI_TRUE : JNI_FALSE;
    });
}

EXPOJUCE_JNI(void, nativeNoteOn)(JNIEnv*, jclass, jint note, jint velocity) {
    expojuce::host::noteOn((int)note, (int)velocity);
}

EXPOJUCE_JNI(void, nativeNoteOff)(JNIEnv*, jclass, jint note) {
    expojuce::host::noteOff((int)note);
}

EXPOJUCE_JNI(void, nativeAllNotesOff)(JNIEnv*, jclass) {
    expojuce::host::allNotesOff();
}

EXPOJUCE_JNI(void, nativeSetWaveform)(JNIEnv* env, jclass, jstring waveform) {
    const std::string value = toStdString(env, waveform);
    guardedVoid(env, [&] { expojuce::host::setWaveform(value); });
}

// The eight numeric synth parameters, by index (the order of ExpoJuceNative.SynthParam).
EXPOJUCE_JNI(void, nativeSetSynthParam)(JNIEnv* env, jclass, jint param, jdouble value) {
    guardedVoid(env, [&] {
        const double v = (double)value;
        switch ((int)param) {
            case 0: expojuce::host::setFilterCutoff(v); break;
            case 1: expojuce::host::setFilterResonance(v); break;
            case 2: expojuce::host::setAttack(v); break;
            case 3: expojuce::host::setDecay(v); break;
            case 4: expojuce::host::setSustain(v); break;
            case 5: expojuce::host::setRelease(v); break;
            case 6: expojuce::host::setDetune(v); break;
            case 7: expojuce::host::setMasterLevel(v); break;
            default: break;
        }
    });
}

// 0 effects, 1 arpeggiator, 2 drums, 3 analysis.
EXPOJUCE_JNI(jstring, nativeStateJson)(JNIEnv* env, jclass, jint which) {
    return guarded<jstring>(env, nullptr, [&] {
        switch ((int)which) {
            case 0: return toJString(env, expojuce::host::effectsStateJson());
            case 1: return toJString(env, expojuce::host::arpeggiatorStateJson());
            case 2: return toJString(env, expojuce::host::drumStateJson());
            case 3: return toJString(env, expojuce::host::analysisJson());
            default: return toJString(env, "{}");
        }
    });
}
