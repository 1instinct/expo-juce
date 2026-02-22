// JuceModules.mm — Compiles all vendored JUCE modules as a single ObjC++ translation unit.
// The .mm extension ensures Objective-C++ mode so JUCE's Apple platform code compiles correctly.

#include "JuceConfig.h"

#include "juce_modules/juce_core/juce_core.cpp"
#include "juce_modules/juce_events/juce_events.cpp"
#include "juce_modules/juce_data_structures/juce_data_structures.cpp"
#include "juce_modules/juce_audio_basics/juce_audio_basics.cpp"
#include "juce_modules/juce_audio_formats/juce_audio_formats.cpp"
#include "juce_modules/juce_dsp/juce_dsp.cpp"
