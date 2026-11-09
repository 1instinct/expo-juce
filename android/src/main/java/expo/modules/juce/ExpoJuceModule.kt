package expo.modules.juce

import android.os.Handler
import android.os.Looper
import expo.modules.kotlin.Promise
import expo.modules.kotlin.functions.Queues
import expo.modules.kotlin.modules.Module
import expo.modules.kotlin.modules.ModuleDefinition

/**
 * The Android side of the ExpoJuce module. Same names and argument shapes as
 * ios/ExpoJuceModule.swift.
 *
 * Implemented: lifecycle (setup / shutdown on the main thread), diagnostics, the test tone, notes
 * and the synth parameters. Everything else exists with the iOS "not ready" / defaults shape, so
 * callers written for iOS work unchanged: setters return false (nothing applied), getters return
 * the stored defaults, loadBackingTrack rejects with ERR_UNSUPPORTED, hasDrums and hasJucePadView
 * return false (the JS wrapper then falls back to its non-native paths).
 */
class ExpoJuceModule : Module() {
  override fun definition() = ModuleDefinition {
    Name("ExpoJuce")

    OnDestroy {
      // Lifecycle calls belong on the main thread (JUCE's message thread).
      onMainThread {
        if (ExpoJuceNative.isInitialised) ExpoJuceNative.nativeShutdown()
      }
    }

    // In the background the audio device is closed (held notes released) and nothing renders;
    // back in the foreground it reopens with the same setup. Both are no-ops before setup.
    OnActivityEntersBackground {
      onMainThread {
        if (ExpoJuceNative.isInitialised) ExpoJuceNative.nativeEnterBackground()
      }
    }

    OnActivityEntersForeground {
      onMainThread {
        if (ExpoJuceNative.isInitialised) ExpoJuceNative.nativeEnterForeground()
      }
    }

    Constants(
      "PI" to Math.PI
    )

    Events("onChange", "onBeat")

    Function("hello") {
      "Hello world!"
    }

    // ── Lifecycle ──────────────────────────────────────────────────

    // options: { bufferSize?: number; sampleRate?: number } (defaults 128 / 48000). The device's
    // native output rate, when the platform reports one, is preferred over sampleRate so the
    // stream runs without resampling (diagnostics: sampleRate, nativeSampleRate,
    // preferredSampleRate).
    AsyncFunction("setup") { options: Map<String, Any?>?, promise: Promise ->
      val requestedBufferSize = numberOption(options, "bufferSize") ?: 128.0
      val bufferSize = requestedBufferSize.coerceIn(0.0, 4096.0).toInt()
      val sampleRate = numberOption(options, "sampleRate") ?: 48000.0
      val context = appContext.reactContext
      try {
        if (context == null) {
          promise.reject("ERR_AUDIO_SETUP", "step -1: no Android context", null)
        } else if (!ExpoJuceNative.ensureInitialised(context)) {
          val reason = ExpoJuceNative.loadError ?: "JUCE initialisation failed"
          promise.reject("ERR_AUDIO_SETUP", "step -1: $reason", null)
        } else if (ExpoJuceNative.nativeSetup(bufferSize, sampleRate, ExpoJuceNative.nativeOutputSampleRate(context))) {
          promise.resolve(null)
        } else {
          promise.reject("ERR_AUDIO_SETUP", ExpoJuceNative.nativeSetupErrorMessage(), null)
        }
      } catch (error: Throwable) {
        promise.reject("ERR_AUDIO_SETUP", "step -1: ${error.message ?: error.javaClass.name}", error)
      }
    }.runOnQueue(Queues.MAIN)

    AsyncFunction("shutdown") {
      if (ExpoJuceNative.isInitialised) ExpoJuceNative.nativeShutdown()
    }.runOnQueue(Queues.MAIN)

    // ── Diagnostics ────────────────────────────────────────────────

    Function("getDiagnostics") {
      if (ExpoJuceNative.isLoaded) {
        ExpoJuceNative.jsonToMap(ExpoJuceNative.nativeDiagnosticsJson())
      } else {
        unloadedDiagnostics()
      }
    }

    Function("playTestTone") { durationMs: Double ->
      ExpoJuceNative.isLoaded && ExpoJuceNative.nativePlayTestTone(durationMs)
    }

    // ── Synth (MIDI-style) ─────────────────────────────────────────
    // Lock-free note queue: any thread, before or after setup (note-ons are dropped until ready).

    Function("noteOn") { note: Int, velocity: Int ->
      if (ExpoJuceNative.isLoaded) ExpoJuceNative.nativeNoteOn(note, velocity)
    }

    Function("noteOff") { note: Int ->
      if (ExpoJuceNative.isLoaded) ExpoJuceNative.nativeNoteOff(note)
    }

    Function("allNotesOff") {
      if (ExpoJuceNative.isLoaded) ExpoJuceNative.nativeAllNotesOff()
    }

    // ── Global Synth Params ────────────────────────────────────────
    // Applied to the running synth (dropped before setup, as on iOS).

    Function("setWaveform") { waveform: String ->
      if (ExpoJuceNative.isLoaded) ExpoJuceNative.nativeSetWaveform(waveform)
    }

    Function("setFilterCutoff") { hz: Double -> synthParam(ExpoJuceNative.SynthParam.FILTER_CUTOFF, hz) }
    Function("setFilterResonance") { q: Double -> synthParam(ExpoJuceNative.SynthParam.FILTER_RESONANCE, q) }
    Function("setAttack") { ms: Double -> synthParam(ExpoJuceNative.SynthParam.ATTACK, ms) }
    Function("setDecay") { ms: Double -> synthParam(ExpoJuceNative.SynthParam.DECAY, ms) }
    Function("setSustain") { level: Double -> synthParam(ExpoJuceNative.SynthParam.SUSTAIN, level) }
    Function("setRelease") { ms: Double -> synthParam(ExpoJuceNative.SynthParam.RELEASE, ms) }
    Function("setDetune") { cents: Double -> synthParam(ExpoJuceNative.SynthParam.DETUNE, cents) }
    Function("setMasterLevel") { level: Double -> synthParam(ExpoJuceNative.SynthParam.MASTER_LEVEL, level) }

    // ── Effects (not processed on Android yet: nothing applied, defaults reported) ──

    Function("setEffectEnabled") { _: String, _: Boolean -> false }

    Function("setEffectParam") { _: String, _: String, _: Double -> false }

    Function("getEffectsState") { ExpoJuceNative.effectsState }

    // ── Metronome / backing track / analysis (not on Android yet) ──

    Function("setMetronome") { _: Map<String, Any?> -> false }

    Function("setBackingTrackOptions") { _: Map<String, Any?> -> }

    AsyncFunction("loadBackingTrack") { _: String, _: Map<String, Any?>?, promise: Promise ->
      promise.reject("ERR_UNSUPPORTED", "backing tracks are not supported on Android yet", null)
    }

    Function("unloadBackingTrack") {}

    Function("getAnalysis") { ExpoJuceNative.analysis() }

    // ── Arpeggiator (not on Android yet) ───────────────────────────

    Function("setArpeggiator") { _: Map<String, Any?> -> false }

    Function("getArpeggiatorState") { ExpoJuceNative.arpeggiatorState }

    // ── Drums (not on Android yet: hasDrums is false) ──────────────

    Function("hasDrums") { false }

    Function("drumHit") { _: Double, _: Double -> }

    Function("setDrumVoice") { _: Double, _: Map<String, Any?> -> false }

    Function("setDrumKit") { _: Map<String, Any?> -> false }

    Function("getDrumState") { ExpoJuceNative.drumState }

    // ── Transport (not on Android yet: stopped at beat 0, 120 bpm) ──

    Function("startTransport") {}

    Function("stopTransport") {}

    Function("setTempo") { _: Double -> }

    Function("setTransportPosition") { _: Double -> }

    Function("getTransportPosition") { 0.0 }

    Function("getTempo") { 120.0 }

    Function("isTransportPlaying") { false }

    // ── Async / View ───────────────────────────────────────────────

    AsyncFunction("setValueAsync") { value: String ->
      sendEvent("onChange", mapOf("value" to value))
    }

    // No native pad view on Android yet: the JS wrapper's isJucePadViewAvailable() is false.
    // The function must exist, because the wrapper passes setup options only when it does.
    Function("hasJucePadView") { false }
  }

  // Runs block on the main thread: now if this is it, else posted.
  private fun onMainThread(block: () -> Unit) {
    if (Looper.myLooper() == Looper.getMainLooper()) block() else Handler(Looper.getMainLooper()).post { block() }
  }

  private fun synthParam(param: ExpoJuceNative.SynthParam, value: Double) {
    if (ExpoJuceNative.isLoaded) ExpoJuceNative.nativeSetSynthParam(param.ordinal, value)
  }

  private fun unloadedDiagnostics(): Map<String, Any?> = mapOf(
    "platform" to "android",
    "isReady" to false,
    "setupStep" to -1,
    "lastError" to (ExpoJuceNative.loadError ?: ""),
    "engineState" to "failed",
    "deviceName" to "",
    "deviceType" to "",
    "sampleRate" to 0.0,
    "bufferSize" to 0,
    "outputLatencySamples" to 0,
    "callbackCount" to 0.0,
    "graphProcessCount" to 0.0,
    "synthProcessCount" to 0.0,
    "outputPeak" to 0.0,
    "synthPeak" to 0.0,
    "nanCount" to 0.0,
    "toneSamplesMixed" to 0.0,
    "notesQueued" to 0.0,
    "notesConsumed" to 0.0,
    "notesDropped" to 0.0,
    "activeVoices" to 0,
    "lastNoteToRenderMs" to -1.0,
    "lastTouchToRenderMs" to -1.0,
    "preferredBufferSize" to 0,
    "preferredSampleRate" to 0.0,
    "sessionReassertCount" to 0,
    "interruptionCount" to 0,
    "mediaResetCount" to 0,
    "lastRecoveryError" to "",
    "sessionCategory" to "",
    "sessionSampleRate" to 0.0,
    "sessionIOBufferMs" to 0.0,
    "sessionOutputLatencyMs" to 0.0,
    "otherAudioPlaying" to false,
    "routeOutputs" to emptyList<String>()
  )

  private companion object {
    // A finite number from the JS options object, or null.
    fun numberOption(options: Map<String, Any?>?, key: String): Double? {
      val value = (options?.get(key) as? Number)?.toDouble() ?: return null
      return if (value.isFinite()) value else null
    }
  }
}
