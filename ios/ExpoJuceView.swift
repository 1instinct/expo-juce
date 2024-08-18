import ExpoModulesCore

// This view will be used as a native component. Make sure to inherit from `ExpoView`
// to apply the proper styling (e.g. border radius and shadows).
class ExpoJuceView: ExpoView {
  public func definition() -> ModuleDefinition {
    Name("ExpoJuce")

    Function("getApiKey") { () -> String in
      "api-key"
    }
  }
}
