import ExpoModulesCore

public class ExpoJuceModule: Module {
  public func definition() -> ModuleDefinition {
    Name("ExpoJuce")

    OnCreate {
      ExpoJuceBridge.initialize()
    }

    OnDestroy {
      ExpoJuceBridge.shutdown()
    }

    Constants([
      "PI": Double.pi
    ])

    Events("onChange")

    Function("hello") {
      return "Hello world!"
    }

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

    Function("setDSPValue") { (param: String, value: Double) in
      switch param {
      case "frequency":
        ExpoJuceBridge.setFrequency(value)
      case "level":
        ExpoJuceBridge.setLevel(value)
      default:
        break
      }
    }

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
