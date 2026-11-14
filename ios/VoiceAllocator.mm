#include "VoiceAllocator.h"
#include <cstring>
#include <cmath>

VoiceAllocator::VoiceAllocator() {
    std::memset(noteMap, -1, sizeof(noteMap));
}

int VoiceAllocator::findVoice(Voice* voices) {
    for (int i = 0; i < MAX_VOICES; i++) {
        if (!voices[i].isActive()) {
            return i;
        }
    }

    int quietest = 0;
    float quietestLevel = voices[0].getCurrentLevel();
    for (int i = 1; i < MAX_VOICES; i++) {
        float level = voices[i].getCurrentLevel();
        if (level < quietestLevel) {
            quietestLevel = level;
            quietest = i;
        }
    }
    return quietest;
}

int VoiceAllocator::noteOn(int note, float velocity, Voice* voices,
                           Waveform waveform, double detuneCents,
                           float filterCutoff, float filterResonance,
                           float masterLevel, uint64_t seq, VoiceSource source) {
    // Retrigger: the latest voice of this note stops, unless one of the two belongs to the loop
    // sequence and the other does not (see VoiceSource).
    if (note >= 0 && note < 128 && noteMap[note] >= 0) {
        int existing = noteMap[note];
        if (existing < MAX_VOICES && voices[existing].getNoteNumber() == note
            && (voices[existing].source == VoiceSource::Sequence) == (source == VoiceSource::Sequence)) {
            voices[existing].stopNote();
        }
        noteMap[note] = -1;
    }

    int voiceIdx = findVoice(voices);
    if (voiceIdx < 0) return -1;

    Voice& voice = voices[voiceIdx];

    if (voice.isActive()) {
        int oldNote = voice.getNoteNumber();
        double oldFreq = 440.0 * std::pow(2.0, (oldNote - 69) / 12.0);
        if (std::fabs(detuneCents) > 0.01) {
            oldFreq *= std::pow(2.0, detuneCents / 1200.0);
        }

        voice.captureAndRelease(waveform, oldFreq,
                                filterCutoff, filterResonance, masterLevel);

        if (oldNote >= 0 && oldNote < 128) {
            noteMap[oldNote] = -1;
        }
    }

    voice.startNote(note, velocity);
    voice.startSeq = seq;
    voice.source = source;

    if (note >= 0 && note < 128) {
        noteMap[note] = voiceIdx;
    }

    return voiceIdx;
}

void VoiceAllocator::noteOff(int note, Voice* voices) {
    if (note < 0 || note >= 128) return;

    int voiceIdx = noteMap[note];
    if (voiceIdx >= 0 && voiceIdx < MAX_VOICES) {
        if (voices[voiceIdx].getNoteNumber() == note) {
            voices[voiceIdx].stopNote();
        }
        noteMap[note] = -1;
    }
}

void VoiceAllocator::allNotesOff(Voice* voices) {
    for (int i = 0; i < MAX_VOICES; i++) {
        if (voices[i].isActive()) {
            voices[i].stopNote();
        }
    }
    std::memset(noteMap, -1, sizeof(noteMap));
}
