import ExpoModulesCore

public class ExpoJuceModule: Module {
  private var hasListeners = false

  public func definition() -> ModuleDefinition {
    Name("ExpoJuce")

    OnDestroy {
      ExpoJuceBridge.stopBeatTimer()
      ExpoJuceBridge.shutdown()
    }

    Constants([
      "PI": Double.pi
    ])

    Events("onChange", "onBeat")

    OnStartObserving {
      self.hasListeners = true
      ExpoJuceBridge.startBeatTimer()
    }

    OnStopObserving {
      self.hasListeners = false
      ExpoJuceBridge.stopBeatTimer()
    }

    Function("hello") {
      return "Hello world!"
    }

    // ── Synth ───────────────────────────────────────────────────────
    // All audio methods use AsyncFunction dispatched on main queue.
    // AVAudioEngine and CoreAudio must be accessed from the main thread.
    // Expo's synchronous Function() runs on the JS/Hermes thread,
    // which is NOT the main thread in production builds — this causes
    // native crashes when calling CoreAudio APIs.

    AsyncFunction("setup") { [weak self] in
      ExpoJuceBridge.setup()
      ExpoJuceBridge.setBeatCallback { [weak self] beat, bpm in
        guard let self = self, self.hasListeners else { return }
        self.sendEvent("onBeat", [
          "beat": beat,
          "bpm": bpm,
        ])
      }
    }.runOnQueue(.main)

    AsyncFunction("playTone") { (frequency: Double, duration: Double) -> String in
      ExpoJuceBridge.playTone(withFrequency: frequency, duration: duration)
      return "Playing tone at \(frequency)Hz for \(duration)ms"
    }.runOnQueue(.main)

    AsyncFunction("setFrequency") { (frequency: Double) in
      ExpoJuceBridge.setFrequency(frequency)
    }.runOnQueue(.main)

    AsyncFunction("setLevel") { (level: Double) in
      ExpoJuceBridge.setLevel(level)
    }.runOnQueue(.main)

    AsyncFunction("setWaveform") { (waveform: String) in
      ExpoJuceBridge.setWaveform(waveform)
    }.runOnQueue(.main)

    AsyncFunction("setHarmonicLevels") { (levels: [Double]) in
      let nsLevels = levels.map { NSNumber(value: $0) }
      ExpoJuceBridge.setHarmonicLevels(nsLevels)
    }.runOnQueue(.main)

    AsyncFunction("noteOn") {
      ExpoJuceBridge.noteOn()
    }.runOnQueue(.main)

    AsyncFunction("noteOff") {
      ExpoJuceBridge.noteOff()
    }.runOnQueue(.main)

    // ── DSP Params ──────────────────────────────────────────────────

    AsyncFunction("setAttack") { (ms: Double) in
      ExpoJuceBridge.setAttack(ms)
    }.runOnQueue(.main)

    AsyncFunction("setRelease") { (ms: Double) in
      ExpoJuceBridge.setRelease(ms)
    }.runOnQueue(.main)

    AsyncFunction("setFilterCutoff") { (hz: Double) in
      ExpoJuceBridge.setFilterCutoff(hz)
    }.runOnQueue(.main)

    AsyncFunction("setFilterResonance") { (q: Double) in
      ExpoJuceBridge.setFilterResonance(q)
    }.runOnQueue(.main)

    AsyncFunction("setDetune") { (cents: Double) in
      ExpoJuceBridge.setDetune(cents)
    }.runOnQueue(.main)

    AsyncFunction("setDSPValue") { (param: String, value: Double) in
      switch param {
      case "frequency":
        ExpoJuceBridge.setFrequency(value)
      case "level":
        ExpoJuceBridge.setLevel(value)
      case "attack":
        ExpoJuceBridge.setAttack(value)
      case "release":
        ExpoJuceBridge.setRelease(value)
      case "filterCutoff":
        ExpoJuceBridge.setFilterCutoff(value)
      case "filterResonance":
        ExpoJuceBridge.setFilterResonance(value)
      case "detune":
        ExpoJuceBridge.setDetune(value)
      default:
        break
      }
    }.runOnQueue(.main)

    // ── Transport ───────────────────────────────────────────────────

    AsyncFunction("startTransport") {
      ExpoJuceBridge.startTransport()
    }.runOnQueue(.main)

    AsyncFunction("stopTransport") {
      ExpoJuceBridge.stopTransport()
    }.runOnQueue(.main)

    AsyncFunction("setTempo") { (bpm: Double) in
      ExpoJuceBridge.setTempo(bpm)
    }.runOnQueue(.main)

    AsyncFunction("setTransportPosition") { (beat: Double) in
      ExpoJuceBridge.setTransportPosition(beat)
    }.runOnQueue(.main)

    AsyncFunction("getTransportPosition") { () -> Double in
      return ExpoJuceBridge.getTransportPosition()
    }.runOnQueue(.main)

    AsyncFunction("getTempo") { () -> Double in
      return ExpoJuceBridge.getTempo()
    }.runOnQueue(.main)

    AsyncFunction("isTransportPlaying") { () -> Bool in
      return ExpoJuceBridge.isTransportPlaying()
    }.runOnQueue(.main)

    // ── Async / View ────────────────────────────────────────────────

    AsyncFunction("setValueAsync") { (value: String) in
      self.sendEvent("onChange", [
        "value": value
      ])
    }

    View(ExpoJuceView.self) {
      Prop("name") { (view: ExpoJuceView, prop: String) in
        print(prop)
      }
    }
  }
}
