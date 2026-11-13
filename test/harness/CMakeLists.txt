cmake_minimum_required(VERSION 3.22)
project(expo_juce_audio_harness CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(JUCE_MODULES "${REPO_ROOT}/ios/juce_modules")

# ── Engine sources ────────────────────────────────────────────────────────────
# By default the engine sources are taken from a git ref (HEAD) rather than the working
# tree, so the harness tests committed code even while ios/ is being edited.
# -DHARNESS_GIT_REF=WORKTREE compiles the working copy instead.
set(HARNESS_GIT_REF "HEAD" CACHE STRING "git ref for ios/ engine sources, or WORKTREE")
set(SNAP "${CMAKE_BINARY_DIR}/engine_src")
file(MAKE_DIRECTORY "${SNAP}")
set(ENGINE_UNITS Voice VoiceAllocator TransportEngine PolySynthProcessor FxChainProcessor BackingTrack AuxProcessor)
set(ENGINE_CPP)
foreach(u IN LISTS ENGINE_UNITS)
  foreach(ext h mm)
    set(out "${SNAP}/${u}.${ext}")
    if(ext STREQUAL "mm")
      set(out "${SNAP}/${u}.cpp")   # these .mm files are pure C++
    endif()
    if(HARNESS_GIT_REF STREQUAL "WORKTREE")
      configure_file("${REPO_ROOT}/ios/${u}.${ext}" "${out}" COPYONLY)
    else()
      execute_process(COMMAND git show "${HARNESS_GIT_REF}:ios/${u}.${ext}"
                      WORKING_DIRECTORY "${REPO_ROOT}" OUTPUT_FILE "${out}"
                      RESULT_VARIABLE rc)
      if(NOT rc EQUAL 0)
        message(FATAL_ERROR "git show ${HARNESS_GIT_REF}:ios/${u}.${ext} failed")
      endif()
    endif()
  endforeach()
  list(APPEND ENGINE_CPP "${SNAP}/${u}.cpp")
endforeach()
# Header-only engine units (pure C++), e.g. the device callback GraphPlayer.
set(ENGINE_HEADERS GraphPlayer NoteEventQueue AnalysisRing Arpeggiator SpectrumAnalyser DrumMachine LatencyStats DeviceFormat)
foreach(u IN LISTS ENGINE_HEADERS)
  set(out "${SNAP}/${u}.h")
  if(HARNESS_GIT_REF STREQUAL "WORKTREE")
    configure_file("${REPO_ROOT}/ios/${u}.h" "${out}" COPYONLY)
  else()
    execute_process(COMMAND git show "${HARNESS_GIT_REF}:ios/${u}.h"
                    WORKING_DIRECTORY "${REPO_ROOT}" OUTPUT_FILE "${out}"
                    RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
      message(FATAL_ERROR "git show ${HARNESS_GIT_REF}:ios/${u}.h failed")
    endif()
  endif()
endforeach()

# ── Linux dev headers for JUCE gui modules (needed only because juce_audio_processors
#    includes juce_gui_extra). Fetched rootless into the build dir if not installed. ──
set(SYSROOT_INC)
if(NOT EXISTS /usr/include/X11/Xlib.h OR NOT EXISTS /usr/include/freetype2/ft2build.h)
  set(SYSROOT "${CMAKE_BINARY_DIR}/sysroot")
  execute_process(COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/fetch_deps.sh" "${SYSROOT}"
                  RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "fetch_deps.sh failed")
  endif()
  set(SYSROOT_INC "${SYSROOT}/usr/include" "${SYSROOT}/usr/include/freetype2")
else()
  set(SYSROOT_INC /usr/include/freetype2)
endif()

# ── JUCE: one translation unit per module, like ios/JuceModule_*.mm ──
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
set(JUCE_CPP)
foreach(u IN LISTS JUCE_UNITS)
  string(REPLACE "/" "_" n "${u}")
  set(out "${CMAKE_BINARY_DIR}/juce_units/${n}")
  file(WRITE "${out}.tmp" "#include \"JuceConfig.h\"\n#include \"${JUCE_MODULES}/${u}\"\n")
  configure_file("${out}.tmp" "${out}" COPYONLY)
  list(APPEND JUCE_CPP "${out}")
endforeach()

add_library(juce_lib STATIC ${JUCE_CPP})
target_include_directories(juce_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${JUCE_MODULES}")
target_include_directories(juce_lib SYSTEM PRIVATE ${SYSROOT_INC})
target_compile_definitions(juce_lib PUBLIC DEBUG=1 _DEBUG=1 JUCE_LOG_ASSERTIONS=1)
target_compile_options(juce_lib PUBLIC -O2)
target_compile_options(juce_lib PRIVATE -w)
find_package(Threads REQUIRED)
target_link_libraries(juce_lib PUBLIC Threads::Threads dl rt
                      /usr/lib/x86_64-linux-gnu/libfreetype.so.6)

add_executable(harness harness.cpp ${ENGINE_CPP})
target_include_directories(harness PRIVATE "${SNAP}")
target_link_libraries(harness PRIVATE juce_lib)
target_compile_definitions(harness PRIVATE HARNESS_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
                                           HARNESS_JS_SRC="${CMAKE_CURRENT_SOURCE_DIR}/../../src"
                                           # BackingTrack::acquire() test seam, see harness.cpp
                                           BACKING_TRACK_ACQUIRE_HOOK=harnessBackingTrackAcquireHook)

# Android host (android/src/main/cpp/AndroidEngineHost.cpp): its portable part, with JUCE's Android
# device replaced by a fake device type (host_test.cpp). Same git ref / WORKTREE rule as ios/.
set(HOST_SNAP "${CMAKE_BINARY_DIR}/host_src")
file(MAKE_DIRECTORY "${HOST_SNAP}")
foreach(f AndroidEngineHost.h AndroidEngineHost.cpp)
  if(HARNESS_GIT_REF STREQUAL "WORKTREE")
    configure_file("${REPO_ROOT}/android/src/main/cpp/${f}" "${HOST_SNAP}/${f}" COPYONLY)
  else()
    execute_process(COMMAND git show "${HARNESS_GIT_REF}:android/src/main/cpp/${f}"
                    WORKING_DIRECTORY "${REPO_ROOT}" OUTPUT_FILE "${HOST_SNAP}/${f}"
                    RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
      message(FATAL_ERROR "git show ${HARNESS_GIT_REF}:android/src/main/cpp/${f} failed")
    endif()
  endif()
endforeach()
# The generated Kotlin defaults the test compares with (host_test --write-defaults).
set(DEFAULT_SHAPES_REL "android/src/main/java/expo/modules/juce/DefaultShapes.kt")
if(HARNESS_GIT_REF STREQUAL "WORKTREE")
  configure_file("${REPO_ROOT}/${DEFAULT_SHAPES_REL}" "${HOST_SNAP}/DefaultShapes.kt" COPYONLY)
else()
  execute_process(COMMAND git show "${HARNESS_GIT_REF}:${DEFAULT_SHAPES_REL}"
                  WORKING_DIRECTORY "${REPO_ROOT}" OUTPUT_FILE "${HOST_SNAP}/DefaultShapes.kt"
                  RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "git show ${HARNESS_GIT_REF}:${DEFAULT_SHAPES_REL} failed")
  endif()
endif()
add_executable(host_test host_test.cpp "${HOST_SNAP}/AndroidEngineHost.cpp" ${ENGINE_CPP})
target_include_directories(host_test PRIVATE "${HOST_SNAP}" "${SNAP}")
target_compile_definitions(host_test PRIVATE EXPOJUCE_DEFAULT_SHAPES_KT="${HOST_SNAP}/DefaultShapes.kt")
target_link_libraries(host_test PRIVATE juce_lib)
