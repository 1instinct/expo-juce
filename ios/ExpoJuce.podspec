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
  s.platforms      = { :ios => '13.4', :tvos => '13.4' }
  s.swift_version  = '5.4'
  s.source         = { git: 'https://github.com/Beeper-buzz/expo-juce' }
  s.static_framework = true

  s.dependency 'ExpoModulesCore'

  # ARC source files: Swift module, ObjC bridge, C++ engine
  s.source_files = "*.{h,m,mm,swift}"
  s.requires_arc = true

  # JuceModules.mm compiles JUCE source which uses manual retain/release.
  # Exclude it from ARC — it must be compiled with -fno-objc-arc.
  s.exclude_files = "JuceModules.mm"

  # C++ headers must be private — they include <atomic>, <algorithm>, etc.
  # which fail when compiled as Objective-C through the module map.
  s.private_header_files = [
    "JuceConfig.h",
    "Voice.h",
    "VoiceAllocator.h",
    "PolySynthProcessor.h",
    "TransportEngine.h",
  ]

  # JUCE compilation unit — compiled WITHOUT ARC
  s.subspec 'JuceModules' do |jm|
    jm.source_files = "JuceModules.mm"
    jm.requires_arc = false
  end

  # JUCE modules compiled: all 11 (core, events, data_structures, graphics, gui_basics,
  # gui_extra, audio_basics, audio_formats, audio_devices, audio_processors, dsp)
  # JuceModules.mm is a single translation unit that #includes the JUCE .cpp files.
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
    # Suppress JUCE deprecation warnings and implicit conversion warnings in vendored code
    'GCC_WARN_ABOUT_DEPRECATED_DECLARATIONS' => 'NO',
    'OTHER_CPLUSPLUSFLAGS' => '-Wno-shorten-64-to-32 -Wno-comma -Wno-deprecated-declarations',
  }
end
