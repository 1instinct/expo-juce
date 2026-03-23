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

  # Our source files: Swift module, ObjC bridge, C++ tone generator, JUCE compilation unit
  s.source_files = "*.{h,m,mm,swift}"
  s.requires_arc = true

  # JUCE modules compiled: juce_core, juce_audio_basics, juce_audio_formats, juce_dsp
  # NOT compiled: juce_events, juce_data_structures (static initializer conflicts with RN)
  # JuceModules.mm is a single translation unit that #includes the JUCE .cpp files.
  s.preserve_paths = ["juce_modules/**"]

  s.frameworks = 'AVFoundation', 'AudioToolbox', 'CoreAudio',
                 'Foundation', 'Accelerate', 'CoreMIDI'
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
