#pragma once

#include "Voice.h"

// 16: a 16-note chord through the arpeggiator, or ratchets with long releases, without stealing.
inline constexpr int MAX_VOICES = 16;

class VoiceAllocator {
public:
    VoiceAllocator();

    // A stolen voice's tail is rendered with the given (current, smoothed) parameters.
    int noteOn(int note, float velocity, Voice* voices,
               Waveform waveform, float filterCutoff, float filterResonance,
               float masterLevel, uint64_t seq = 0, VoiceSource source = VoiceSource::Player);

    void noteOff(int note, Voice* voices);
    void allNotesOff(Voice* voices);

private:
    int findVoice(Voice* voices);
    int noteMap[128];
};
