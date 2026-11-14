require 'json'

package = JSON.parse(File.read(File.join(__dir__, '..', 'package.json')))

Pod::Spec.new do |s|
  s.name           = 'ExpoJuce'
  s.version        = package['version']
  s.summary        = package['description']
  s.description    = package['description']
  s.license        = package['license']
  s.author         = package['author']
  s.homepage       = package['homepage']
  s.platforms      = { :ios => '13.4' }
  s.swift_version  = '5.4'
  s.source         = { git: 'https://github.com/1instinct/expo-juce' }
  s.static_framework = true

  s.dependency 'ExpoModulesCore'

  s.source_files = "*.{h,m,mm,swift}"

  # Per-file ARC control: JuceModule_*.mm files compile JUCE source which uses
  # manual retain/release. Only files listed here get ARC enabled.
  s.requires_arc = [
    'ExpoJuceModule.swift',
    'JucePadView.swift',
    'ExpoJuceBridge.m',
    'ExpoJuceAudioEngine.mm',
    'PolySynthProcessor.mm',
    'FxChainProcessor.mm',
    'AuxProcessor.mm',
    'BackingTrack.mm',
    'Voice.mm',
    'VoiceAllocator.mm',
    'TransportEngine.mm',
    'ExpoJuce-Bridging-Header.h',
  ]

  # C++ headers must be private — they include <atomic>, <algorithm>, etc.
  # which fail when compiled as Objective-C through the module map.
  s.private_header_files = [
    "JuceConfig.h",
    "Voice.h",
    "VoiceAllocator.h",
    "OwnerToken.h",
    "PolySynthProcessor.h",
    "FxChainProcessor.h",
    "AuxProcessor.h",
    "BackingTrack.h",
    "AnalysisRing.h",
    "TransportEngine.h",
    "GraphPlayer.h",
    "NoteEventQueue.h",
    "Arpeggiator.h",
    "SpectrumAnalyser.h",
    "SourceRing.h",
    "SourceAnalysis.h",
    "DrumMachine.h",
    "LatencyStats.h",
    "DisplayRate.h",
    "DeviceFormat.h",
    "MidiInput.h",
    "ExpoJuceMidiInput.h", # plain C, but its <stdint.h> include trips check:headers; native callers dlsym it
  ]

  # JUCE modules: each compiled as its own translation unit (separate .mm file)
  # to avoid static symbol collisions between modules.
  s.preserve_paths = ["juce_modules/**"]

  s.frameworks = 'AVFoundation', 'AudioToolbox', 'CoreAudio',
                 'Foundation', 'Accelerate', 'CoreMIDI',
                 'CoreGraphics', 'CoreImage', 'CoreText',
                 'QuartzCore', 'UIKit', 'WebKit', 'Metal', 'MetalKit'
  s.weak_frameworks = 'UserNotifications'
  s.libraries = 'c++'

  s.pod_target_xcconfig = {
    'DEFINES_MODULE' => 'YES',
    'SWIFT_COMPILATION_MODE' => 'wholemodule',
    'CLANG_CXX_LANGUAGE_STANDARD' => 'c++17',
    'HEADER_SEARCH_PATHS' => '"$(PODS_TARGET_SRCROOT)" "$(PODS_TARGET_SRCROOT)/juce_modules"',
    'GCC_WARN_ABOUT_DEPRECATED_DECLARATIONS' => 'NO',
    'OTHER_CPLUSPLUSFLAGS' => '-Wno-shorten-64-to-32 -Wno-comma -Wno-deprecated-declarations',
  }
end
