// JuceModules.mm — Compiles vendored JUCE modules as a single ObjC++ translation unit.
// The .mm extension ensures Objective-C++ mode so JUCE's Apple platform code compiles correctly.
//
// Only safe modules are compiled here. juce_events is EXCLUDED because its
// static MessageManager initializer conflicts with React Native's app lifecycle.
// The dependency chain is: juce_dsp -> juce_audio_formats -> juce_audio_basics -> juce_core
// None of these modules depend on juce_events.

#include "JuceConfig.h"

#include "juce_modules/juce_core/juce_core.cpp"
#include "juce_modules/juce_audio_basics/juce_audio_basics.cpp"
#include "juce_modules/juce_audio_formats/juce_audio_formats.cpp"
#include "juce_modules/juce_dsp/juce_dsp.cpp"
