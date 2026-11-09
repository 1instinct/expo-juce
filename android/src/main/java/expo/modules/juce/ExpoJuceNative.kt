package expo.modules.juce

import android.content.Context
import android.media.AudioManager
import android.os.Looper
import android.util.Log
import org.json.JSONArray
import org.json.JSONObject

/**
 * The native library (android/CMakeLists.txt: JUCE + Oboe + the engine) and its JNI entry points
 * (src/main/cpp/ExpoJuceJni.cpp). Loading is lazy and never throws: if the library cannot be
 * loaded, every call reports "not ready" and [loadError] says why.
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
  @JvmStatic external fun nativeDiagnosticsJson(): String?
  @JvmStatic external fun nativePlayTestTone(durationMs: Double): Boolean
  @JvmStatic external fun nativeNoteOn(note: Int, velocity: Int)
  @JvmStatic external fun nativeNoteOff(note: Int)
  @JvmStatic external fun nativeAllNotesOff()
  @JvmStatic external fun nativeSetWaveform(waveform: String)
  @JvmStatic external fun nativeSetSynthParam(param: Int, value: Double)
  @JvmStatic external fun nativeStateJson(which: Int): String?
}
