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

  # Our source files: Swift module, ObjC bridge, C++ tone generator
  s.source_files = "*.{h,m,mm,swift}"
  s.exclude_files = ["JuceConfig.h", "JuceModules.mm"]
  s.requires_arc = true

  # JUCE sources preserved on disk for future use but NOT compiled —
  # current engine uses pure C++ + AVFoundation, no JUCE APIs.
  # Compiling JUCE modules (especially juce_events) causes launch crashes
  # due to static initializers conflicting with React Native's app lifecycle.
  s.preserve_paths = ["juce_modules/**"]

  s.frameworks = 'AVFoundation', 'AudioToolbox', 'CoreAudio',
                 'Foundation', 'Accelerate'
  s.libraries = 'c++'

  s.pod_target_xcconfig = {
    'DEFINES_MODULE' => 'YES',
    'SWIFT_COMPILATION_MODE' => 'wholemodule',
    'CLANG_CXX_LANGUAGE_STANDARD' => 'c++17',
    'HEADER_SEARCH_PATHS' => '"$(PODS_TARGET_SRCROOT)"',
  }
end
