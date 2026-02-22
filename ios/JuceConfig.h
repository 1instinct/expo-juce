#pragma once

// Xcode 16+ clang no longer implicitly includes these —
// JUCE uses FLT_MAX, FLT_MIN, INT_MAX, etc. throughout.
#include <cfloat>
#include <climits>
#include <cstdint>
#include <cmath>

// Tell JUCE that global module settings are configured
#define JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED 1

// Disable JUCE splash screen and analytics
#define JUCE_DISPLAY_SPLASH_SCREEN 0
#define JUCE_REPORT_APP_USAGE 0
#define JUCE_USE_DARK_SPLASH_SCREEN 0
#define JUCE_STRICT_REFCOUNTEDPOINTER 1

// Module availability flags
#define JUCE_MODULE_AVAILABLE_juce_core 1
#define JUCE_MODULE_AVAILABLE_juce_events 1
#define JUCE_MODULE_AVAILABLE_juce_data_structures 1
#define JUCE_MODULE_AVAILABLE_juce_audio_basics 1
#define JUCE_MODULE_AVAILABLE_juce_audio_formats 1
#define JUCE_MODULE_AVAILABLE_juce_dsp 1

// Not a standalone JUCE app — embedded in React Native
#define JUCE_STANDALONE_APPLICATION 0

// Disable unit tests
#define JUCE_UNIT_TESTS 0

// Disable features we don't need
#define JUCE_USE_CURL 0
#define JUCE_WEB_BROWSER 0
#define JUCE_USE_OGGVORBIS 0
#define JUCE_USE_MP3AUDIOFORMAT 0
#define JUCE_USE_LAME_AUDIO_FORMAT 0
#define JUCE_USE_WINDOWS_MEDIA_FORMAT 0
