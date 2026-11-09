cmake_minimum_required(VERSION 3.18)
project(expojuce C CXX)

# The native library behind the Android module: JUCE (one translation unit per module, like
# ios/JuceModule_*.mm), JUCE's vendored Oboe, the engine units that are shared with iOS, and the
# Android host + JNI layer in src/main/cpp. Sources are read from ../ios, which the package
# ships for the iOS build anyway.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(PACKAGE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(IOS_DIR "${PACKAGE_ROOT}/ios")
set(JUCE_MODULES "${IOS_DIR}/juce_modules")
set(OBOE_DIR "${JUCE_MODULES}/juce_audio_devices/native/oboe")
set(CPP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
set(UNITS_DIR "${CMAKE_CURRENT_BINARY_DIR}/units")

# Writes a one-line wrapper that compiles `included` as C++ (the shared engine units are .mm files
# that are pure C++). configure_file only touches the wrapper when its text changes.
function(expojuce_unit out_var name included)
  set(out "${UNITS_DIR}/${name}.cpp")
  file(WRITE "${out}.tmp" "#include \"JuceConfig.h\"\n#include \"${included}\"\n")
  configure_file("${out}.tmp" "${out}" COPYONLY)
  set(${out_var} "${out}" PARENT_SCOPE)
endfunction()

# ── JUCE modules (the list test/harness/CMakeLists.txt compiles) ──
set(JUCE_UNITS
  juce_core/juce_core.cpp
  juce_core/juce_core_CompilationTime.cpp
  juce_events/juce_events.cpp
  juce_data_structures/juce_data_structures.cpp
  juce_audio_basics/juce_audio_basics.cpp
  juce_audio_formats/juce_audio_formats.cpp
  juce_audio_devices/juce_audio_devices.cpp
  juce_audio_processors/juce_audio_processors.cpp
  juce_dsp/juce_dsp.cpp
  juce_graphics/juce_graphics.cpp
  juce_graphics/juce_graphics_Harfbuzz.cpp
  juce_gui_basics/juce_gui_basics.cpp
  juce_gui_extra/juce_gui_extra.cpp)
set(THIRD_PARTY_SOURCES)
foreach(unit IN LISTS JUCE_UNITS)
  string(REPLACE "/" "_" name "${unit}")
  expojuce_unit(out "${name}" "${JUCE_MODULES}/${unit}")
  list(APPEND THIRD_PARTY_SOURCES "${out}")
endforeach()

# ── Oboe (JUCE_USE_ANDROID_OBOE, see ios/JuceConfig.h) and the NDK's cpufeatures (juce_core) ──
file(GLOB_RECURSE OBOE_SOURCES CONFIGURE_DEPENDS "${OBOE_DIR}/src/*.cpp")
list(APPEND THIRD_PARTY_SOURCES ${OBOE_SOURCES})
set(CPUFEATURES_DIR "${ANDROID_NDK}/sources/android/cpufeatures")
list(APPEND THIRD_PARTY_SOURCES "${CPUFEATURES_DIR}/cpu-features.c")

# ── Engine units shared with iOS (the harness's ENGINE_UNITS) ──
set(ENGINE_UNITS Voice VoiceAllocator TransportEngine PolySynthProcessor FxChainProcessor BackingTrack AuxProcessor)
set(ENGINE_SOURCES)
foreach(unit IN LISTS ENGINE_UNITS)
  expojuce_unit(out "engine_${unit}" "${IOS_DIR}/${unit}.mm")
  list(APPEND ENGINE_SOURCES "${out}")
endforeach()

add_library(expojuce SHARED
  "${CPP_DIR}/AndroidEngineHost.cpp"
  "${CPP_DIR}/ExpoJuceJni.cpp"
  ${ENGINE_SOURCES}
  ${THIRD_PARTY_SOURCES})

target_include_directories(expojuce PRIVATE
  "${CPP_DIR}"
  "${IOS_DIR}"                # JuceConfig.h and the shared engine headers
  "${JUCE_MODULES}"
  "${OBOE_DIR}/include"
  "${OBOE_DIR}/src"
  "${CPUFEATURES_DIR}")

target_compile_definitions(expojuce PRIVATE
  JUCE_ANDROID=1
  # JUCE calls newer NDK APIs (e.g. AFont, API 29) behind __builtin_available checks: those
  # symbols must be weak references below their API level, as in JUCE's own Android builds.
  __ANDROID_UNAVAILABLE_SYMBOLS_ARE_WEAK__
  $<$<NOT:$<CONFIG:Debug>>:NDEBUG>)

# Size and speed: -Os in every configuration (a debug app build still needs a real-time engine),
# dead-code stripping at link time, hidden symbols except JNI entry points (JNIEXPORT).
target_compile_options(expojuce PRIVATE
  -Os
  -fexceptions
  -frtti
  -fvisibility=hidden
  -fvisibility-inlines-hidden
  -ffunction-sections
  -fdata-sections)
# JUCE and Oboe are third-party code: keep their warnings out of the build log.
set_source_files_properties(${THIRD_PARTY_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")

target_link_options(expojuce PRIVATE
  -Wl,--gc-sections
  -Wl,--exclude-libs,ALL
  # 16 KB page alignment (required for apps targeting Android 15+ on Google Play)
  -Wl,-z,max-page-size=16384)

# OpenSLES: Oboe's OpenSL ES fallback (devices without AAudio) references its interface IDs.
target_link_libraries(expojuce PRIVATE log android dl OpenSLES)
