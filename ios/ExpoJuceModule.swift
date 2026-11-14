import ExpoModulesCore

public class ExpoJuceModule: Module {
  private var hasListeners = false
  // Tokens of this instance's MIDI input handler and engine ownership: on a reload the new
  // instance can be created before this one is destroyed, and this one's teardown then must not
  // undo what the new one set up (the bridge ignores a stale token).
  private var midiInputHandlerToken: UInt64 = 0
  private var moduleToken: UInt64 = 0
  // This instance's engine event handler (sequence notes, remote commands, interruptions, ...).
  private var engineEventToken: UInt64 = 0
  // JS listeners of onSequenceNotes (setSequenceNotesListening counts them): the engine builds the
  // batches only while one listens.
  private var sequenceNotesListeners = 0
  private let sequenceNotesLock = NSLock()

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

    OnCreate {
      self.moduleToken = ExpoJuceBridge.claimModule()
      // MIDI input batches (on the input's background queue) go to JS while anything listens; the
      // engine checks that (setMidiInputListening), so this block reads no state of the module.
      self.midiInputHandlerToken = ExpoJuceBridge.setMidiInputHandler { [weak self] batch in
        self?.sendEvent("onMidiInput", batch)
      }
      // Engine events (a background serial queue): onSequenceNotes, onRemoteCommand,
      // onAudioInterruption, onTransportChange, onBackgroundState.
      self.engineEventToken = ExpoJuceBridge.setEngineEventHandler { [weak self] name, body in
        self?.sendEvent(name, body)
      }
    }

    OnDestroy {
      ExpoJuceBridge.clearMidiInputHandler(self.midiInputHandlerToken)
      ExpoJuceBridge.clearEngineEventHandler(self.engineEventToken)
      // Stops the beat timer and shuts the engine down unless a newer instance owns it.
      ExpoJuceBridge.releaseModule(self.moduleToken)
    }

    Constants([
      "PI": Double.pi
    ])

    Events(
      "onChange", "onBeat", "onMidiInput", "onSequenceNotes", "onRemoteCommand", "onAudioInterruption",
      "onTransportChange", "onBackgroundState"
    )

    Function("hello") {
      return "Hello world!"
    }

    // Called when the first listener (of any event) subscribes and when the last one is removed.
    // The timer may start before setup (it idles until the engine exists); stopping it keeps the
    // beat callback, so the next subscriber gets events again without another setup.
    OnStartObserving {
      self.hasListeners = true
      ExpoJuceBridge.setMidiInputListening(true, token: self.midiInputHandlerToken)
      ExpoJuceBridge.startBeatTimer()
    }

    OnStopObserving {
      self.hasListeners = false
      ExpoJuceBridge.setMidiInputListening(false, token: self.midiInputHandlerToken)
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
        ExpoJuceBridge.setBeatCallback { [weak self] beat, bpm, position in
          guard let self = self, self.hasListeners else { return }
          self.sendEvent("onBeat", [
            "beat": beat,
            "bpm": bpm,
            "position": position,
          ])
        }
        // Listeners that subscribed before setup: start (or keep) the beat timer now.
        if self?.hasListeners == true {
          ExpoJuceBridge.startBeatTimer()
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

    // ── Display rate ───────────────────────────────────────────────
    // hz above 60 keeps a ProMotion display at that rate (0 or 60: no hint). Stored now, applied on
    // the main thread; false only for a non-finite hz.

    Function("setDisplayRateHint") { (hz: Double) -> Bool in
      return ExpoJuceBridge.setDisplayRateHint(hz)
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

    // ── MIDI byte input ────────────────────────────────────────────
    // Native code feeds bytes through expojuce_midi_input_bytes (ExpoJuceMidiInput.h); JS sets the
    // map and gets every parsed message back as onMidiInput batches. Any thread, lock-free map.

    Function("hasMidiInput") { () -> Bool in
      return true
    }

    // actions / velocity: 128 numbers each (see ExpoJuceAudioEngine.h). false: wrong lengths.
    Function("setMidiInputMap") { (actions: [Double], velocity: [Double]?) -> Bool in
      // Out-of-range values pass natively; clamp first so the conversion can never trap.
      let toNumbers = { (values: [Double]) -> [NSNumber] in
        values.map { NSNumber(value: $0.isFinite ? Int32(Swift.max(-1.0, Swift.min(1000.0, $0.rounded()))) : -1) }
      }
      return ExpoJuceBridge.setMidiInputMap(actions: toNumbers(actions), velocity: velocity.map(toNumbers))
    }

    Function("setMidiInputEnabled") { (enabled: Bool) in
      ExpoJuceBridge.setMidiInputEnabled(enabled)
    }

    Function("releaseMidiInputHolds") { () -> Int in
      return Int(ExpoJuceBridge.releaseMidiInputHolds())
    }

    Function("getMidiInputClock") { () -> Double in
      return ExpoJuceBridge.midiInputClock()
    }

    // At most 512 bytes; arrival <= 0 (or absent) = now. Messages parsed, -1 for an unknown format.
    Function("midiInputBytes") { (bytes: [Double], format: Int, arrival: Double?) -> Int in
      let numbers = bytes.map { NSNumber(value: $0.isFinite ? Int32(Swift.max(0.0, Swift.min(255.0, $0))) : 0) }
      return Int(ExpoJuceBridge.midiInputBytes(numbers, format: Int32(clamping: format), arrival: arrival ?? 0))
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

    // ── Effects ────────────────────────────────────────────────────
    // Lock-free: atomics in a process-lifetime store, remembered before setup and across
    // engine rebuilds. false = unknown effect / parameter or non-finite value.

    Function("setEffectEnabled") { (effect: String, enabled: Bool) -> Bool in
      return ExpoJuceBridge.setEffectEnabled(effect, enabled: enabled)
    }

    Function("setEffectParam") { (effect: String, param: String, value: Double) -> Bool in
      return ExpoJuceBridge.setEffectParam(effect, param: param, value: value)
    }

    Function("getEffectsState") { () -> [String: Any] in
      return ExpoJuceBridge.getEffectsState()
    }

    // ── Metronome / backing track / analysis ───────────────────────
    // Options are lock-free and remembered before setup and across rebuilds.

    // false (nothing applied) for an unknown style.
    Function("setMetronome") { (options: [String: Any]) -> Bool in
      return ExpoJuceBridge.setMetronome(options)
    }

    Function("setBackingTrackOptions") { (options: [String: Any]) in
      ExpoJuceBridge.setBackingTrackOptions(options)
    }

    // Decode + resample run on a background queue; a newer load or unload supersedes this one
    // (rejects with ERR_BACKING_TRACK_SUPERSEDED).
    AsyncFunction("loadBackingTrack") { (uri: String, gapless: [String: Any]?, promise: Promise) in
      ExpoJuceBridge.loadBackingTrack(uri, gapless: gapless) { result, code, message in
        if let result = result {
          promise.resolve(result)
        } else {
          promise.reject(code ?? "ERR_BACKING_TRACK", message ?? "backing track load failed")
        }
      }
    }

    Function("unloadBackingTrack") {
      ExpoJuceBridge.unloadBackingTrack()
    }

    Function("getAnalysis") { () -> [String: Any] in
      return ExpoJuceBridge.getAnalysis()
    }

    // ── Arpeggiator (0.7.0+) ───────────────────────────────────────
    // Options are lock-free and remembered before setup and across rebuilds. false (nothing
    // applied) for an unknown mode / rate.

    Function("setArpeggiator") { (options: [String: Any]) -> Bool in
      return ExpoJuceBridge.setArpeggiator(options)
    }

    Function("getArpeggiatorState") { () -> [String: Any] in
      return ExpoJuceBridge.getArpeggiatorState()
    }

    // ── Drums (0.8.0+) ─────────────────────────────────────────────
    // Hits go through the lock-free note queue (any thread); parameters are lock-free and
    // remembered before setup and across rebuilds.

    Function("hasDrums") { () -> Bool in
      return true
    }

    // voice 0–8 (anything else, including a non-integer, is ignored); velocity clamped to 1–127.
    Function("drumHit") { (voice: Double, velocity: Double) -> Void in
      guard voice.isFinite, velocity.isFinite, voice.rounded() == voice, voice >= 0, voice <= 8 else {
        return
      }
      let clampedVelocity = Int32(min(max(velocity.rounded(), 1), 127))
      ExpoJuceBridge.drumHit(Int32(voice), velocity: clampedVelocity)
    }

    // Partial update; unknown keys ignored, values clamped. false for a voice outside 0–8.
    Function("setDrumVoice") { (voice: Double, params: [String: Any]) -> Bool in
      guard voice.isFinite, voice.rounded() == voice, voice >= 0, voice <= 8 else {
        return false
      }
      return ExpoJuceBridge.setDrumVoice(Int32(voice), params: params)
    }

    Function("setDrumKit") { (params: [String: Any]) -> Bool in
      return ExpoJuceBridge.setDrumKit(params)
    }

    Function("getDrumState") { () -> [String: Any] in
      return ExpoJuceBridge.getDrumState()
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

    // ── Loop sequence (0.9.0-rc.5) ─────────────────────────────────

    // Lets JS detect a binary with the native sequence player (older binaries lack this function).
    Function("hasSequencePlayer") { () -> Bool in
      return true
    }

    // data: 6 numbers per event (start, length, note, velocity, kind 0 note / 1 drum, skipBefore or
    // NaN for none); fromBeat / endBeat: -Infinity / Infinity for no bound (JSI passes NaN and the
    // infinities as numbers). false (nothing changes) for a bad array, over 8192 events or a bad loop.
    Function("setSequence") { (data: [Double], loopBeats: Double, fromBeat: Double, endBeat: Double) -> Bool in
      return data.withUnsafeBufferPointer { buffer in
        ExpoJuceBridge.setSequence(
          data: buffer.baseAddress, count: buffer.count, loopBeats: loopBeats, fromBeat: fromBeat, endBeat: endBeat)
      }
    }

    Function("setSequenceMuted") { (muted: Bool) in
      ExpoJuceBridge.setSequenceMuted(muted)
    }

    // Counted: onSequenceNotes batches are built while at least one JS listener asks for them.
    Function("setSequenceNotesListening") { (listening: Bool) in
      self.sequenceNotesLock.lock()
      self.sequenceNotesListeners = max(0, self.sequenceNotesListeners + (listening ? 1 : -1))
      let on = self.sequenceNotesListeners > 0
      self.sequenceNotesLock.unlock()
      ExpoJuceBridge.setSequenceNotesListening(on, token: self.engineEventToken)
    }

    // ── Background, Now Playing, remote commands (0.9.0-rc.5) ───────

    // options: { keepAlive?: boolean (default true); idleStopSeconds?: number (default 30) }.
    Function("setBackgroundPolicy") { (options: [String: Any]?) in
      let keepAlive = (options?["keepAlive"] as? Bool) ?? true
      let idle = ExpoJuceModule.numberOption(options, "idleStopSeconds") ?? 30
      ExpoJuceBridge.setBackgroundPolicy(keepAlive: keepAlive, idleStopSeconds: idle)
    }

    // info: { title: string; artist?: string; album?: string } or null to clear.
    Function("setNowPlaying") { (info: [String: Any]?) -> Bool in
      return ExpoJuceBridge.setNowPlaying(info)
    }

    Function("enableRemoteCommands") { (commands: [String]) -> Bool in
      return ExpoJuceBridge.enableRemoteCommands(commands)
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

      // "notes" (default) or "drums": in drums mode a touch hits drum voice `note` (0–8).
      Prop("mode") { (view: JucePadView, mode: String?) in
        view.drumMode = (mode == "drums")
      }

      Prop("velocity") { (view: JucePadView, velocity: Int?) in
        view.velocity = velocity ?? 100
      }

      Prop("enabled") { (view: JucePadView, enabled: Bool?) in
        view.enabled = enabled ?? true
      }

      // Normalised flat [x0, y0, x1, y1, …]; empty / odd / < 3 points = whole rect.
      Prop("hitPolygon") { (view: JucePadView, polygon: [Double]?) in
        view.hitPolygon = polygon ?? []
      }
    }
  }
}
