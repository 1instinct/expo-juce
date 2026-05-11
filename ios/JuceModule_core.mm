// Per-module JUCE compilation unit — each module must be its own translation unit
// to avoid static symbol collisions (e.g. getNullVarRef in both juce_core and juce_data_structures).
#include "JuceConfig.h"
#include "juce_modules/juce_core/juce_core.cpp"
