package expo.modules.juce

import android.content.Context
import android.media.AudioManager
import android.os.Looper
import android.util.Log
import org.json.JSONArray
import org.json.JSONObject
import kotlin.math.roundToInt

/**
 * The native library (android/CMakeLists.txt: JUCE + Oboe + the engine) and its JNI entry points
 * (src/main/cpp/ExpoJuceJni.cpp). Loading is lazy and never throws: if the library cannot be
 * loaded, every call reports "not ready", [loadError] says why, and the getters return the same
 * shapes as the loaded path ([DefaultShapes]), so readers never see missing keys.
 */
internal object ExpoJuceNative {
  private const val TAG = "ExpoJuce"
  private const val LIBRARY = "expojuce"

  /** Indices of nativeSetSynthParam (ExpoJuceJni.cpp). */
  enum class SynthParam { FILTER_CUTOFF, FILTER_RESONANCE, ATTACK, DECAY, SUSTAIN, RELEASE, DETUNE, MASTER_LEVEL }

  /** Indices of nativeStateJson (ExpoJuceJni.cpp). */
  enum class State { EFFECTS, ARPEGGIATOR, DRUMS, ANALYSIS }

  @Volatile var loadError: String? = null
    private set

  val isLoaded: Boolean by lazy {
    try {
      System.loadLibrary(LIBRARY)
      true
    } catch (error: Throwable) {
      loadError = "System.loadLibrary(\"$LIBRARY\"): ${error.message ?: error.javaClass.name}"
      Log.e(TAG, loadError!!, error)
      false
    }
  }

  @Volatile private var initialised = false

  /** JUCE has been initialised (nativeInit succeeded); lifecycle calls are possible. */
  val isInitialised: Boolean
    get() = initialised

  /**
   * Initialises JUCE once: its JNI classes, its app context and its MessageManager. Main thread
   * only (JUCE's message queue binds to the calling thread's Looper). Returns false if the
   * library is missing or initialisation failed.
   */
  fun ensureInitialised(context: Context): Boolean {
    check(Looper.myLooper() == Looper.getMainLooper()) { "JUCE must be initialised on the main thread" }
    if (initialised) return true
    if (!isLoaded) return false
    initialised = nativeInit(context.applicationContext ?: context)
    return initialised
  }

  /** The device's native output sample rate (Hz), or 0 if the platform does not say. */
  fun nativeOutputSampleRate(context: Context): Double {
    val audioManager = context.getSystemService(Context.AUDIO_SERVICE) as? AudioManager ?: return 0.0
    return audioManager.getProperty(AudioManager.PROPERTY_OUTPUT_SAMPLE_RATE)?.toDoubleOrNull() ?: 0.0
  }

  // ── Stored state ──
  // Effects, arpeggiator and drum settings cannot change on Android yet (their setters are not
  // implemented), so each shape is converted once. getAnalysis is polled per frame: its shape is
  // converted once and only the live field (lastNote) is patched into a shallow copy.

  val effectsState: Map<String, Any?> by lazy { stateMap(State.EFFECTS) }
  val arpeggiatorState: Map<String, Any?> by lazy { stateMap(State.ARPEGGIATOR) }
  val drumState: Map<String, Any?> by lazy { stateMap(State.DRUMS) }
  private val analysisShape: Map<String, Any?> by lazy { stateMap(State.ANALYSIS) }

  fun analysis(): Map<String, Any?> {
    val result = LinkedHashMap(analysisShape)
    result["lastNote"] = if (isLoaded) nativeLastNote() else -1
    return result
  }

  /**
   * getSourceAnalysis(source, options): the native result, or the inactive shape (zeros) if the
   * library is missing or returned nothing usable. Options: bands (default true), scope
   * ("none" | "window" | "cycle", default "window"), scopePoints (16–128, default 64), cycleHz,
   * windowMs (5–42, default 20).
   */
  fun sourceAnalysis(source: String, options: Map<String, Any?>?): Map<String, Any?> {
    val bands = options?.get("bands") as? Boolean ?: true
    val scope = options?.get("scope") as? String ?: "window"
    val points = (options?.get("scopePoints") as? Number)?.toDouble() ?: Double.NaN
    val cycleHz = (options?.get("cycleHz") as? Number)?.toDouble() ?: 0.0
    val windowMs = (options?.get("windowMs") as? Number)?.toDouble() ?: Double.NaN
    if (isLoaded) {
      val native = jsonToMap(nativeSourceAnalysisJson(source, bands, scope, points, cycleHz, windowMs))
      if (native.isNotEmpty()) return native
    }
    val count = if (scope == "none") 0 else if (points.isFinite()) points.roundToInt().coerceIn(16, 128) else 64
    return linkedMapOf(
      "active" to false,
      "peak" to 0.0,
      "rms" to 0.0,
      "bands" to List(24) { 0.0 },
      "scope" to List(count) { 0.0 },
      "scopeMode" to if (scope == "none") "none" else "window",
      "scopePeriod" to 0.0,
      "sampleRate" to 0.0,
    )
  }

  // The native shape, or the generated default if the library is missing or its JSON is unusable.
  private fun stateMap(which: State): Map<String, Any?> {
    val native = if (isLoaded) jsonToMap(nativeStateJson(which.ordinal)) else emptyMap()
    return native.ifEmpty { jsonToMap(defaultJson(which)) }
  }

  private fun defaultJson(which: State): String = when (which) {
    State.EFFECTS -> DefaultShapes.EFFECTS
    State.ARPEGGIATOR -> DefaultShapes.ARPEGGIATOR
    State.DRUMS -> DefaultShapes.DRUMS
    State.ANALYSIS -> DefaultShapes.ANALYSIS
  }

  // ── Diagnostics ──

  private val defaultDiagnostics: Map<String, Any?> by lazy { jsonToMap(DefaultShapes.DIAGNOSTICS) }

  /**
   * getDiagnostics(): the native diagnostics, or, if the library is missing (or returned nothing
   * usable), the not-set-up shape with isReady false, engineState "failed" and lastError set.
   */
  fun diagnostics(): Map<String, Any?> {
    if (isLoaded) {
      val native = jsonToMap(nativeDiagnosticsJson())
      if (native.isNotEmpty()) return native
    }
    val result = LinkedHashMap(defaultDiagnostics)
    result["isReady"] = false
    result["engineState"] = "failed"
    result["lastError"] = loadError ?: "the native library returned no diagnostics"
    return result
  }

  // ── JSON from the native side → maps / lists for the module's return values ──

  fun jsonToMap(json: String?): Map<String, Any?> {
    if (json.isNullOrEmpty()) return emptyMap()
    return try {
      toMap(JSONObject(json))
    } catch (error: Exception) {
      Log.e(TAG, "bad JSON from native: $json", error)
      emptyMap()
    }
  }

  private fun toMap(obj: JSONObject): Map<String, Any?> {
    val map = LinkedHashMap<String, Any?>()
    for (key in obj.keys()) map[key] = toValue(obj.get(key))
    return map
  }

  private fun toValue(value: Any?): Any? = when (value) {
    is JSONObject -> toMap(value)
    is JSONArray -> List(value.length()) { index -> toValue(value.get(index)) }
    JSONObject.NULL -> null
    else -> value
  }

  // ── JNI ──
  @JvmStatic external fun nativeInit(context: Context): Boolean
  @JvmStatic external fun nativeSetup(bufferSize: Int, sampleRate: Double, nativeSampleRate: Double): Boolean
  @JvmStatic external fun nativeSetupErrorMessage(): String
  @JvmStatic external fun nativeShutdown()
  @JvmStatic external fun nativeEnterBackground()
  @JvmStatic external fun nativeEnterForeground()
  @JvmStatic external fun nativeLastNote(): Int
  @JvmStatic external fun nativeDiagnosticsJson(): String?
  @JvmStatic external fun nativePlayTestTone(durationMs: Double): Boolean
  @JvmStatic external fun nativeNoteOn(note: Int, velocity: Int)
  @JvmStatic external fun nativeNoteOff(note: Int)
  @JvmStatic external fun nativeAllNotesOff()
  @JvmStatic external fun nativeSetWaveform(waveform: String)
  @JvmStatic external fun nativeSetSynthParam(param: Int, value: Double)
  @JvmStatic external fun nativeStateJson(which: Int): String?
  @JvmStatic external fun nativeSourceAnalysisJson(
    source: String, bands: Boolean, scope: String, scopePoints: Double, cycleHz: Double, windowMs: Double,
  ): String?
}
