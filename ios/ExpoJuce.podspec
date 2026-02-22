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

  # Only OUR source files — not JUCE subdirectories.
  # JUCE headers must NOT be in source_files or CocoaPods adds them to the
  # module map, where they fail to compile standalone (missing FLT_MAX etc).
  # JuceModules.mm #includes the JUCE .cpp files via amalgamation;
  # JUCE headers are found via HEADER_SEARCH_PATHS.
  s.source_files = "*.{h,m,mm,swift}"
  # JuceConfig.h is a C++ prefix header (uses <cfloat> etc.) — it must NOT
  # appear in the Clang module map or Swift/ObjC imports will fail.
  # It's applied to C++ via OTHER_CPLUSPLUSFLAGS -include instead.
  s.exclude_files = "JuceConfig.h"

  # Keep JUCE sources on disk so the compiler can find them via #include
  s.preserve_paths = "juce_modules/**"

  # iOS frameworks required by JUCE modules + our AVAudioEngine code
  s.frameworks = 'AVFoundation', 'AudioToolbox', 'CoreAudio',
                 'Foundation', 'Accelerate', 'QuartzCore',
                 'CoreFoundation', 'CoreText', 'Security'
  s.libraries = 'c++'

  s.pod_target_xcconfig = {
    'DEFINES_MODULE' => 'YES',
    'SWIFT_COMPILATION_MODE' => 'wholemodule',
    'CLANG_CXX_LANGUAGE_STANDARD' => 'c++17',
    'CLANG_ENABLE_OBJC_ARC' => 'YES',
    # PODS_TARGET_SRCROOT already points to ios/ (where this podspec lives).
    # Only our dir in HEADER_SEARCH_PATHS — safe for the Clang module map.
    # JUCE's juce_modules/ goes in OTHER_CPLUSPLUSFLAGS so it only affects
    # C++/ObjC++ source compilation and can't shadow system headers during
    # module building (JUCE bundles its own zlib.h, etc.).
    'HEADER_SEARCH_PATHS' => '"$(PODS_TARGET_SRCROOT)"',
    'GCC_PREPROCESSOR_DEFINITIONS' => '$(inherited) JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1',
    'OTHER_CPLUSPLUSFLAGS' => '$(inherited) -I"$(PODS_TARGET_SRCROOT)/juce_modules" -include "$(PODS_TARGET_SRCROOT)/JuceConfig.h"',
  }
end
