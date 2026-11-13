// Pure C++ (no Objective-C): the C entry point of the MIDI byte input (ExpoJuceMidiInput.h). The
// engine (ExpoJuceAudioEngine.mm) installs its process-lifetime MidiInput and the function that
// schedules an event drain; the offline harness (test/harness, case "mic") installs its own and
// calls the exported function from several threads.

#include "ExpoJuceMidiInput.h"
#include "MidiInput.h"

#include <algorithm>
#include <cmath>

namespace midiin {

namespace {
std::atomic<MidiInput*> gInput{nullptr};
std::atomic<void (*)()> gScheduleDrain{nullptr};
} // namespace

void installEntryPoint(MidiInput* input, void (*scheduleDrain)()) {
    gScheduleDrain.store(scheduleDrain, std::memory_order_release);
    gInput.store(input, std::memory_order_release);
}

} // namespace midiin

// Exported with a stable C name; `used` keeps it through dead-code stripping although nothing in
// the binary calls it directly (callers look it up at run time).
extern "C" __attribute__((visibility("default"), used))
int expojuce_midi_input_bytes(const uint8_t *bytes, size_t length, int format, double arrivalSeconds) {
    midiin::MidiInput* input = midiin::gInput.load(std::memory_order_acquire);
    if (input == nullptr) return -1;
    const double now = midiin::NoteQueueSink::nowSeconds();
    const double arrival = (std::isfinite(arrivalSeconds) && arrivalSeconds > 0.0) ? std::min(arrivalSeconds, now) : now;
    bool drain = false;
    const int parsed = input->input(bytes, length, format, arrival, &drain);
    if (drain) {
        if (void (*schedule)() = midiin::gScheduleDrain.load(std::memory_order_acquire)) schedule();
    }
    return parsed;
}
