import ExpoModulesCore

public class ExpoJuceModule: Module {
  private var hasListeners = false

  // Reads a finite number from an options dictionary passed from JS.
  private static func numberOption(_ options: [String: Any]?, _ key: String) -> Double? {
    guard let value = options?[key] else {
      return nil
    }
    var number: Double?
    if let double = value as? Double {
      number = double
    } else if let int = value as? Int {
      number = Double(int)
    } else if let nsNumber = value as? NSNumber {
      number = nsNumber.doubleValue
    }
    guard let result = number, result.isFinite else {
      return nil
    }
    return result
  }

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

    // options: { bufferSize?: number; sampleRate?: number } (defaults 128 / 48000).
    AsyncFunction("setup") { [weak self] (options: [String: Any]?, promise: Promise) in
      let requestedBufferSize = ExpoJuceModule.numberOption(options, "bufferSize") ?? 128
      let bufferSize = Int32(min(max(requestedBufferSize, 0), 4096))
      let sampleRate = ExpoJuceModule.numberOption(options, "sampleRate") ?? 48000
      DispatchQueue.main.async {
        guard ExpoJuceBridge.setup(bufferSize: bufferSize, sampleRate: sampleRate) else {
          promise.reject("ERR_AUDIO_SETUP", ExpoJuceBridge.setupErrorMessage())
          return
        }
        ExpoJuceBridge.setBeatCallback { [weak self] beat, bpm in
          guard let self = self, self.hasListeners else { return }
          self.sendEvent("onBeat", [
            "beat": beat,
            "bpm": bpm,
          ])
        }
        promise.resolve(NSNull())
      }
    }

    AsyncFunction("shutdown") {
      ExpoJuceBridge.shutdown()
    }.runOnQueue(.main)

    // ── Diagnostics ────────────────────────────────────────────────

    Function("getDiagnostics") { () -> [String: Any] in
      return ExpoJuceBridge.getDiagnostics()
    }

    Function("playTestTone") { (durationMs: Double) -> Bool in
      return ExpoJuceBridge.playTestTone(durationMs: durationMs)
    }

    // ── Synth (MIDI-style) ─────────────────────────────────────────
    // Thread-safe: PolySynthProcessor uses a lock-free SPSC queue

    Function("noteOn") { (note: Int, velocity: Int) in
      ExpoJuceBridge.note(on: Int32(note), velocity: Int32(velocity))
    }

    Function("noteOff") { (note: Int) in
      ExpoJuceBridge.noteOff(Int32(note))
    }

    Function("allNotesOff") {
      ExpoJuceBridge.allNotesOff()
    }

    // ── Global Synth Params ────────────────────────────────────────
    // Thread-safe: all params are std::atomic

    Function("setWaveform") { (waveform: String) in
      ExpoJuceBridge.setWaveform(waveform)
    }

    Function("setFilterCutoff") { (hz: Double) in
      ExpoJuceBridge.setFilterCutoff(hz)
    }

    Function("setFilterResonance") { (q: Double) in
      ExpoJuceBridge.setFilterResonance(q)
    }

    Function("setAttack") { (ms: Double) in
      ExpoJuceBridge.setAttack(ms)
    }

    Function("setDecay") { (ms: Double) in
      ExpoJuceBridge.setDecay(ms)
    }

    Function("setSustain") { (level: Double) in
      ExpoJuceBridge.setSustain(level)
    }

    Function("setRelease") { (ms: Double) in
      ExpoJuceBridge.setRelease(ms)
    }

    Function("setDetune") { (cents: Double) in
      ExpoJuceBridge.setDetune(cents)
    }

    Function("setMasterLevel") { (level: Double) in
      ExpoJuceBridge.setMasterLevel(level)
    }

    // ── Transport ──────────────────────────────────────────────────

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

    // ── Async / View ───────────────────────────────────────────────

    AsyncFunction("setValueAsync") { (value: String) in
      self.sendEvent("onChange", [
        "value": value
      ])
    }

    // Lets JS detect a binary that has JucePadView (older binaries lack this function).
    Function("hasJucePadView") { () -> Bool in
      return true
    }

    // Native touch pad (see JucePadView.swift). The module's only view, so it is the default
    // view: requireNativeViewManager('ExpoJuce').
    View(JucePadView.self) {
      Events("onNoteOn", "onNoteOff")

      Prop("note") { (view: JucePadView, note: Int) in
        view.note = note
      }

      Prop("velocity") { (view: JucePadView, velocity: Int?) in
        view.velocity = velocity ?? 100
      }

      Prop("enabled") { (view: JucePadView, enabled: Bool?) in
        view.enabled = enabled ?? true
      }
    }
  }
}
