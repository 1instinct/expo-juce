#pragma once

#include "Voice.h"

inline constexpr int MAX_VOICES = 8;

class VoiceAllocator {
public:
    VoiceAllocator();

    int noteOn(int note, float velocity, Voice* voices,
               Waveform waveform, double detuneCents,
               float filterCutoff, float filterResonance,
               float masterLevel, uint64_t seq = 0);

    void noteOff(int note, Voice* voices);
    void allNotesOff(Voice* voices);

private:
    int findVoice(Voice* voices);
    int noteMap[128];
};
