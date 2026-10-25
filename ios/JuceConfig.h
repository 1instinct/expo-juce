#pragma once

// Xcode 16+ clang no longer implicitly includes these —
// JUCE uses FLT_MAX, FLT_MIN, INT_MAX, etc. throughout.
// Use C headers (not C++ <cfloat> etc.) since this header is included
// from Objective-C contexts that don't have C++ stdlib access.
#include <float.h>
#include <limits.h>
#include <stdint.h>
#include <math.h>

// Tell JUCE that global module settings are configured
#define JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED 1

// Not a standalone JUCE app — embedded in React Native
#define JUCE_STANDALONE_APPLICATION 0
#define JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP 0

// App identity (required by some JUCE modules)
#define JUCE_APPLICATION_NAME "ExpoJuce"
#define JUCE_APPLICATION_VERSION "0.4.0"

// Disable JUCE splash screen and analytics
#define JUCE_DISPLAY_SPLASH_SCREEN 0
#define JUCE_REPORT_APP_USAGE 0
#define JUCE_USE_DARK_SPLASH_SCREEN 0
#define JUCE_STRICT_REFCOUNTEDPOINTER 1

// ── Module availability flags (all 11 JUCE modules enabled) ──
#define JUCE_MODULE_AVAILABLE_juce_core 1
#define JUCE_MODULE_AVAILABLE_juce_events 1
#define JUCE_MODULE_AVAILABLE_juce_data_structures 1
#define JUCE_MODULE_AVAILABLE_juce_audio_basics 1
#define JUCE_MODULE_AVAILABLE_juce_audio_formats 1
#define JUCE_MODULE_AVAILABLE_juce_audio_devices 1
#define JUCE_MODULE_AVAILABLE_juce_audio_processors 1
#define JUCE_MODULE_AVAILABLE_juce_dsp 1
#define JUCE_MODULE_AVAILABLE_juce_graphics 1
#define JUCE_MODULE_AVAILABLE_juce_gui_basics 1
#define JUCE_MODULE_AVAILABLE_juce_gui_extra 1

// Modules we still don't use
#define JUCE_MODULE_AVAILABLE_juce_audio_plugin_client 0
#define JUCE_MODULE_AVAILABLE_juce_audio_utils 0
#define JUCE_MODULE_AVAILABLE_juce_cryptography 0
#define JUCE_MODULE_AVAILABLE_juce_opengl 0
#define JUCE_MODULE_AVAILABLE_juce_video 0
#define JUCE_MODULE_AVAILABLE_juce_product_unlocking 0

// Disable unit tests
#define JUCE_UNIT_TESTS 0

// Disable features we don't need
#define JUCE_USE_CURL 0
#define JUCE_WEB_BROWSER 0
#define JUCE_USE_OGGVORBIS 0
#define JUCE_USE_MP3AUDIOFORMAT 0
#define JUCE_USE_LAME_AUDIO_FORMAT 0
#define JUCE_USE_WINDOWS_MEDIA_FORMAT 0
#define JUCE_USE_FLAC 0

// Audio device config
#define JUCE_ASIO 0
#define JUCE_WASAPI 0
#define JUCE_DIRECTSOUND 0
#define JUCE_ALSA 0
#define JUCE_JACK 0
#define JUCE_BELA 0
#define JUCE_USE_ANDROID_OBOE 0
#define JUCE_USE_ANDROID_OPENSLES 0
#define JUCE_USE_WINRT_MIDI 0
#define JUCE_DISABLE_AUDIO_MIXING_WITH_OTHER_APPS 0

// Plugin format config (we don't host plugins)
#define JUCE_PLUGINHOST_VST 0
#define JUCE_PLUGINHOST_VST3 0
#define JUCE_PLUGINHOST_AU 0
#define JUCE_PLUGINHOST_LADSPA 0
#define JUCE_PLUGINHOST_LV2 0
#define JUCE_PLUGINHOST_ARA 0
