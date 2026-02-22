import ExpoModulesCore

public class ExpoJuceModule: Module {
  public func definition() -> ModuleDefinition {
    Name("ExpoJuce")

    OnCreate {
      ExpoJuceBridge.setup()

      // Wire native beat callback → JS "onBeat" event
      ExpoJuceBridge.setBeatCallback { [weak self] beat, bpm in
        self?.sendEvent("onBeat", [
          "beat": beat,
          "bpm": bpm,
        ])
      }
    }

    OnDestroy {
      ExpoJuceBridge.shutdown()
    }

    Constants([
      "PI": Double.pi
    ])

    Events("onChange", "onBeat")

    Function("hello") {
      return "Hello world!"
    }

    // ── Synth ───────────────────────────────────────────────────────

    Function("playTone") { (frequency: Double, duration: Double) -> String in
      ExpoJuceBridge.playTone(withFrequency: frequency, duration: duration)
      return "Playing tone at \(frequency)Hz for \(duration)ms"
    }

    Function("setFrequency") { (frequency: Double) in
      ExpoJuceBridge.setFrequency(frequency)
    }

    Function("setLevel") { (level: Double) in
      ExpoJuceBridge.setLevel(level)
    }

    Function("setWaveform") { (waveform: String) in
      ExpoJuceBridge.setWaveform(waveform)
    }

    Function("setHarmonicLevels") { (levels: [Double]) in
      let nsLevels = levels.map { NSNumber(value: $0) }
      ExpoJuceBridge.setHarmonicLevels(nsLevels)
    }

    Function("noteOn") {
      ExpoJuceBridge.noteOn()
    }

    Function("noteOff") {
      ExpoJuceBridge.noteOff()
    }

    // ── DSP Params ──────────────────────────────────────────────────

    Function("setAttack") { (ms: Double) in
      ExpoJuceBridge.setAttack(ms)
    }

    Function("setRelease") { (ms: Double) in
      ExpoJuceBridge.setRelease(ms)
    }

    Function("setFilterCutoff") { (hz: Double) in
      ExpoJuceBridge.setFilterCutoff(hz)
    }

    Function("setFilterResonance") { (q: Double) in
      ExpoJuceBridge.setFilterResonance(q)
    }

    Function("setDetune") { (cents: Double) in
      ExpoJuceBridge.setDetune(cents)
    }

    Function("setDSPValue") { (param: String, value: Double) in
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
    }

    // ── Transport ───────────────────────────────────────────────────

    Function("startTransport") {
      ExpoJuceBridge.startTransport()
    }

    Function("stopTransport") {
      ExpoJuceBridge.stopTransport()
    }

    Function("setTempo") { (bpm: Double) in
      ExpoJuceBridge.setTempo(bpm)
    }

    Function("setTransportPosition") { (beat: Double) in
      ExpoJuceBridge.setTransportPosition(beat)
    }

    Function("getTransportPosition") { () -> Double in
      return ExpoJuceBridge.getTransportPosition()
    }

    Function("getTempo") { () -> Double in
      return ExpoJuceBridge.getTempo()
    }

    Function("isTransportPlaying") { () -> Bool in
      return ExpoJuceBridge.isTransportPlaying()
    }

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
