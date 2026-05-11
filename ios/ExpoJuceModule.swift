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

    Function("hello") {
      return "Hello world!"
    }

    OnStartObserving {
      self.hasListeners = true
      ExpoJuceBridge.startBeatTimer()
    }

    OnStopObserving {
      self.hasListeners = false
      ExpoJuceBridge.stopBeatTimer()
    }

    // ── Lifecycle ──────────────────────────────────────────────────

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

    AsyncFunction("shutdown") {
      ExpoJuceBridge.shutdown()
    }.runOnQueue(.main)

    // ── Synth (MIDI-style) ─────────────────────────────────────────

    AsyncFunction("noteOn") { (note: Int, velocity: Int) in
      ExpoJuceBridge.noteOn(Int32(note), velocity: Int32(velocity))
    }.runOnQueue(.main)

    AsyncFunction("noteOff") { (note: Int) in
      ExpoJuceBridge.noteOff(Int32(note))
    }.runOnQueue(.main)

    AsyncFunction("allNotesOff") {
      ExpoJuceBridge.allNotesOff()
    }.runOnQueue(.main)

    // ── Global Synth Params ────────────────────────────────────────

    AsyncFunction("setWaveform") { (waveform: String) in
      ExpoJuceBridge.setWaveform(waveform)
    }.runOnQueue(.main)

    AsyncFunction("setFilterCutoff") { (hz: Double) in
      ExpoJuceBridge.setFilterCutoff(hz)
    }.runOnQueue(.main)

    AsyncFunction("setFilterResonance") { (q: Double) in
      ExpoJuceBridge.setFilterResonance(q)
    }.runOnQueue(.main)

    AsyncFunction("setAttack") { (ms: Double) in
      ExpoJuceBridge.setAttack(ms)
    }.runOnQueue(.main)

    AsyncFunction("setDecay") { (ms: Double) in
      ExpoJuceBridge.setDecay(ms)
    }.runOnQueue(.main)

    AsyncFunction("setSustain") { (level: Double) in
      ExpoJuceBridge.setSustain(level)
    }.runOnQueue(.main)

    AsyncFunction("setRelease") { (ms: Double) in
      ExpoJuceBridge.setRelease(ms)
    }.runOnQueue(.main)

    AsyncFunction("setDetune") { (cents: Double) in
      ExpoJuceBridge.setDetune(cents)
    }.runOnQueue(.main)

    AsyncFunction("setMasterLevel") { (level: Double) in
      ExpoJuceBridge.setMasterLevel(level)
    }.runOnQueue(.main)

    // ── Transport ──────────────────────────────────────────────────

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

    // ── Async / View ───────────────────────────────────────────────

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
