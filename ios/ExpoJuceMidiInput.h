#pragma once

// MIDI byte input for native code (plain C, safe to include from C, Objective-C, Swift bridging
// headers and C++). The app's own native code (a Bluetooth notification callback, a serial port)
// hands raw bytes to the engine here, with no JavaScript in between: the bytes are parsed, the
// input map pushed from JS (setMidiInputMap) decides what plays, a mapped note-on goes straight
// onto the engine's note queue, and every parsed message is also delivered to JS in batches as an
// `onMidiInput` event (see MidiInput.h and the README).
//
// Linking weakly. Code that must also run without this library (or with an older version of it)
// should look the function up at run time instead of linking it:
//
//     typedef int (*ExpoJuceMidiInputBytesFn)(const uint8_t *, size_t, int, double);
//     ExpoJuceMidiInputBytesFn fn = (ExpoJuceMidiInputBytesFn)dlsym(RTLD_DEFAULT, "expojuce_midi_input_bytes");
//
// A release build that strips global symbols from the executable (Strip Style "All Symbols") hides
// it from dlsym; +[ExpoJuceBridge midiInputBytesFunction] returns the same pointer through the
// Objective-C runtime, which stripping does not affect:
//
//     Class bridge = NSClassFromString(@"ExpoJuceBridge");
//     SEL sel = NSSelectorFromString(@"midiInputBytesFunction");
//     if (fn == NULL && [bridge respondsToSelector:sel])
//         fn = (ExpoJuceMidiInputBytesFn)((void *(*)(id, SEL))objc_msgSend)(bridge, sel);

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// One BLE-MIDI packet per call (header, timestamp bytes, running status; SysEx skipped).
#define EXPOJUCE_MIDI_FORMAT_BLE_MIDI 0
// Raw MIDI bytes, any split across calls.
#define EXPOJUCE_MIDI_FORMAT_RAW_STREAM 1
// Raw MIDI where each call must be complete MIDI on its own (starts with a status byte, whole
// channel voice messages only); anything else is ignored whole, so text a serial bridge
// interleaves never plays.
#define EXPOJUCE_MIDI_FORMAT_RAW_PACKETS 2

// Parses `length` bytes in `format`, plays what the input map says and records an event per
// message for JS. `arrivalSeconds`: when the bytes arrived, in seconds on CACurrentMediaTime()'s
// clock (mach_absolute_time); 0 or less = now. Safe to call from any thread at any time (before
// setup the bytes are parsed and reported, nothing plays). Never blocks for long: it parses,
// pushes to the lock-free note queue and takes a short spinlock shared only with other input
// calls. Returns the number of MIDI messages parsed, or -1 for an unknown format.
int expojuce_midi_input_bytes(const uint8_t *bytes, size_t length, int format, double arrivalSeconds);

#ifdef __cplusplus
}
#endif
