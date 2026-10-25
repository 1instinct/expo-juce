#pragma once
// Linux harness replacement for ios/JuceConfig.h. Same module set and flags as the iOS
// build where they matter to the audio path; Linux-only backends (ALSA/JACK) are off,
// so no audio device is ever opened — the harness fakes the device.

#define JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED 1
#define JUCE_STANDALONE_APPLICATION 0
#define JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP 0
#define JUCE_APPLICATION_NAME "ExpoJuceHarness"
#define JUCE_APPLICATION_VERSION "0.0.0"
#define JUCE_DISPLAY_SPLASH_SCREEN 0
#define JUCE_REPORT_APP_USAGE 0
#define JUCE_USE_DARK_SPLASH_SCREEN 0
#define JUCE_STRICT_REFCOUNTEDPOINTER 1

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
#define JUCE_MODULE_AVAILABLE_juce_audio_plugin_client 0
#define JUCE_MODULE_AVAILABLE_juce_audio_utils 0
#define JUCE_MODULE_AVAILABLE_juce_cryptography 0
#define JUCE_MODULE_AVAILABLE_juce_opengl 0
#define JUCE_MODULE_AVAILABLE_juce_video 0
#define JUCE_MODULE_AVAILABLE_juce_product_unlocking 0

#define JUCE_UNIT_TESTS 0
#define JUCE_USE_CURL 0
#define JUCE_WEB_BROWSER 0
#define JUCE_USE_OGGVORBIS 0
#define JUCE_USE_MP3AUDIOFORMAT 0
#define JUCE_USE_LAME_AUDIO_FORMAT 0
#define JUCE_USE_WINDOWS_MEDIA_FORMAT 0
#define JUCE_USE_FLAC 0

// No real audio backend: the harness drives AudioIODeviceCallback with a fake device.
#define JUCE_ALSA 0
#define JUCE_JACK 0
#define JUCE_BELA 0

// Harness-only: lets the main thread pump the message queue via runDispatchLoopUntil
// (on iOS the main CFRunLoop does this for free).
#define JUCE_MODAL_LOOPS_PERMITTED 1

// Linux GUI bits we can do without (headers come from fetch_deps.sh; no window is opened)
#define JUCE_USE_XRANDR 0
#define JUCE_USE_XINERAMA 0
#define JUCE_USE_XSHM 0
#define JUCE_USE_XRENDER 0
#define JUCE_USE_XCURSOR 0
#define JUCE_USE_FONTCONFIG 0

#define JUCE_PLUGINHOST_VST 0
#define JUCE_PLUGINHOST_VST3 0
#define JUCE_PLUGINHOST_AU 0
#define JUCE_PLUGINHOST_LADSPA 0
#define JUCE_PLUGINHOST_LV2 0
#define JUCE_PLUGINHOST_ARA 0
