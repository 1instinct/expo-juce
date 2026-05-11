// JuceModules.mm — Compiles vendored JUCE modules as a single ObjC++ translation unit.
// The .mm extension ensures Objective-C++ mode so JUCE's Apple platform code compiles correctly.
//
// All 11 JUCE modules are compiled here. juce_events is now enabled —
// MessageManager is initialized explicitly in BeeperAudioEngine::setup()
// and attaches to React Native's existing CFRunLoop (no conflict).

#include "JuceConfig.h"

// Foundation
#include "juce_modules/juce_core/juce_core.cpp"
#include "juce_modules/juce_events/juce_events.cpp"
#include "juce_modules/juce_data_structures/juce_data_structures.cpp"

// Graphics (compile-time dependency of juce_audio_processors, not used for rendering)
#include "juce_modules/juce_graphics/juce_graphics.cpp"
#include "juce_modules/juce_gui_basics/juce_gui_basics.cpp"
#include "juce_modules/juce_gui_extra/juce_gui_extra.cpp"

// Audio
#include "juce_modules/juce_audio_basics/juce_audio_basics.cpp"
#include "juce_modules/juce_audio_formats/juce_audio_formats.cpp"
#include "juce_modules/juce_audio_devices/juce_audio_devices.cpp"
#include "juce_modules/juce_audio_processors/juce_audio_processors.cpp"
#include "juce_modules/juce_dsp/juce_dsp.cpp"
