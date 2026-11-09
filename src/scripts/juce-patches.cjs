#!/usr/bin/env node

/**
 * juce-patches.cjs
 *
 * Local patches to the vendored JUCE modules (ios/juce_modules). vendor-juce.cjs applies them
 * after copying; run this file directly to (re)apply them to the existing vendored tree.
 * Each patch is an exact text replacement. Applying is idempotent (an already patched file is
 * left alone) and fails loudly when neither the original nor the patched text is found, which
 * means the JUCE version changed and the patch must be re-checked.
 * Patch texts are written with "\n"; in a file with CRLF line endings they are matched and
 * written with CRLF.
 *
 * test/check-juce-patches.cjs proves that every patched vendored file equals pristine JUCE plus
 * these patches.
 *
 * Usage: node src/scripts/juce-patches.cjs
 */

const fs = require('fs');
const path = require('path');

const ROOT = path.resolve(__dirname, '../..');
const DEST = path.join(ROOT, 'ios', 'juce_modules');

const PATCHES = [
  {
    // AudioProcessorGraph rendered with a heap allocation on the audio thread: perform() resized
    // currentAudioOutputBuffer to every block's length with avoidReallocating = false, so each
    // change of block length (and the first block after every prepare, whose channel count
    // differs) reallocated it. Preallocate it for at least 8 channels at the prepared block size
    // and resize within that allocation. The harness (test/harness) counts allocations inside
    // the device callback and fails on any.
    id: 'graph-output-buffer-no-realloc',
    file: 'juce_audio_processors/processors/juce_AudioProcessorGraph.cpp',
    replacements: [
      {
        from: 'currentAudioOutputBuffer.setSize (numBuffersNeeded + 1, blockSize);',
        to: 'currentAudioOutputBuffer.setSize (jmax (numBuffersNeeded + 1, 8), blockSize); // expo-juce patch: graph-output-buffer-no-realloc',
      },
      {
        from: 'currentAudioOutputBuffer.setSize (jmax (1, buffer.getNumChannels()), numSamples);',
        to: 'currentAudioOutputBuffer.setSize (jmax (1, buffer.getNumChannels()), numSamples, false, false, true); // expo-juce patch: graph-output-buffer-no-realloc',
      },
    ],
  },
  {
    // A cold CoreMIDI handshake on the main thread: AudioDeviceManager's member initialiser
    // `midiDeviceListConnection = MidiDeviceListConnection::make (...)` made merely constructing
    // one enumerate MIDI devices (MidiDeviceListConnectionBroadcaster::get() -> State{} ->
    // Midi{In,Out}put::getAvailableDevices()), which on Apple platforms creates the global MIDI
    // client and can block for seconds. The connection is made lazily instead, the first time
    // the host uses MIDI devices (ensureMidiDeviceListConnection, next patch).
    id: 'midi-device-list-lazy (header)',
    file: 'juce_audio_devices/audio_io/juce_AudioDeviceManager.h',
    replacements: [
      {
        from: [
          '    AudioBuffer<float> tempBuffer;',
          '    MidiDeviceListConnection midiDeviceListConnection = MidiDeviceListConnection::make ([this]',
          '    {',
          '        midiDeviceListChanged();',
          '    });',
          '',
        ].join('\n'),
        to: [
          '    AudioBuffer<float> tempBuffer;',
          '',
          '    // PATCHED (expo-juce): this was eagerly initialised with',
          '    // MidiDeviceListConnection::make(...), which made simply constructing an',
          '    // AudioDeviceManager reach MidiDeviceListConnectionBroadcaster::get() ->',
          '    // State{} -> Midi{In,Out}put::getAvailableDevices() -> getGlobalMidiClient()',
          '    // -> MIDIClientCreate(). On a cold CoreMIDI that call blocks on a',
          '    // synchronous XPC handshake with midiserver for seconds, and because JUCE',
          '    // requires MIDI calls on the message thread it blocked the main thread and',
          "    // tripped the app's hang monitor.",
          '    //',
          '    // A default-constructed MidiDeviceListConnection is documented as inactive',
          '    // and touches nothing, so hosts that never use MIDI (this one only uses',
          '    // juce::MidiBuffer for processing) never initialise CoreMIDI at all. The',
          '    // real connection is made on first use by ensureMidiDeviceListConnection().',
          '    MidiDeviceListConnection midiDeviceListConnection;',
          '    bool midiDeviceListConnectionCreated = false;',
          '',
        ].join('\n'),
      },
      {
        from: [
          '    void midiDeviceListChanged();',
          '',
          '    String restartDevice (',
        ].join('\n'),
        to: [
          '    void midiDeviceListChanged();',
          '',
          '    // PATCHED (expo-juce): subscribes to MIDI device list changes the first time',
          '    // the host actually touches MIDI. Must be called on the message thread, as',
          '    // the underlying broadcaster asserts that.',
          '    void ensureMidiDeviceListConnection();',
          '',
          '    String restartDevice (',
        ].join('\n'),
      },
    ],
  },
  {
    // The other half of midi-device-list-lazy: ensureMidiDeviceListConnection() and a call to it
    // from every entry point through which a host starts using MIDI devices, so MIDI hosts keep
    // their device-list (hot-plug) notifications.
    id: 'midi-device-list-lazy (source)',
    file: 'juce_audio_devices/audio_io/juce_AudioDeviceManager.cpp',
    replacements: [
      {
        from: [
          '//==============================================================================',
          'static void addIfNotNull (OwnedArray<AudioIODeviceType>& list, AudioIODeviceType* const device)',
        ].join('\n'),
        to: [
          '// PATCHED (expo-juce): see the note on midiDeviceListConnection in the header.',
          '// Making the subscription here rather than in a member initialiser keeps',
          '// CoreMIDI untouched for hosts that never use MIDI devices.',
          'void AudioDeviceManager::ensureMidiDeviceListConnection()',
          '{',
          '    JUCE_ASSERT_MESSAGE_THREAD',
          '',
          '    if (midiDeviceListConnectionCreated)',
          '        return;',
          '',
          '    // Set before make() so that a midiDeviceListChanged() callback arriving',
          '    // during construction cannot re-enter this function.',
          '    midiDeviceListConnectionCreated = true;',
          '',
          '    midiDeviceListConnection = MidiDeviceListConnection::make ([this]',
          '    {',
          '        midiDeviceListChanged();',
          '    });',
          '}',
          '',
          '//==============================================================================',
          'static void addIfNotNull (OwnedArray<AudioIODeviceType>& list, AudioIODeviceType* const device)',
        ].join('\n'),
      },
      {
        from: [
          'void AudioDeviceManager::openLastRequestedMidiDevices (const Array<MidiDeviceInfo>& desiredInputs, const MidiDeviceInfo& defaultOutput)',
          '{',
          '',
        ].join('\n'),
        to: [
          'void AudioDeviceManager::openLastRequestedMidiDevices (const Array<MidiDeviceInfo>& desiredInputs, const MidiDeviceInfo& defaultOutput)',
          '{',
          '    // PATCHED (expo-juce): restoring MIDI devices from saved state means the',
          '    // host does want MIDI, so subscribe now. Re-entry from',
          '    // midiDeviceListChanged() is a no-op once the connection exists.',
          '    ensureMidiDeviceListConnection();',
          '',
          '',
        ].join('\n'),
      },
      {
        from: [
          'void AudioDeviceManager::setMidiInputDeviceEnabled (const String& identifier, bool enabled)',
          '{',
          '',
        ].join('\n'),
        to: [
          'void AudioDeviceManager::setMidiInputDeviceEnabled (const String& identifier, bool enabled)',
          '{',
          '    ensureMidiDeviceListConnection();',
          '',
          '',
        ].join('\n'),
      },
      {
        from: [
          'void AudioDeviceManager::addMidiInputDeviceCallback (const String& identifier, MidiInputCallback* callbackToAdd)',
          '{',
          '',
        ].join('\n'),
        to: [
          'void AudioDeviceManager::addMidiInputDeviceCallback (const String& identifier, MidiInputCallback* callbackToAdd)',
          '{',
          '    ensureMidiDeviceListConnection();',
          '',
          '',
        ].join('\n'),
      },
      {
        from: [
          'void AudioDeviceManager::setDefaultMidiOutputDevice (const String& identifier)',
          '{',
          '',
        ].join('\n'),
        to: [
          'void AudioDeviceManager::setDefaultMidiOutputDevice (const String& identifier)',
          '{',
          '    ensureMidiDeviceListConnection();',
          '',
          '',
        ].join('\n'),
      },
    ],
  },
];

// Applies `patch` to `text` (a file's contents) and returns { text, changed }. Throws when a
// replacement finds neither its patched text nor exactly one copy of its original text.
function applyPatchToText(patch, text) {
  const eol = text.includes('\r\n') ? '\r\n' : '\n';
  const withEol = (s) => (eol === '\n' ? s : s.split('\n').join(eol));
  let changed = false;
  for (const replacement of patch.replacements) {
    const from = withEol(replacement.from);
    const to = withEol(replacement.to);
    if (text.includes(to)) continue;
    const count = text.split(from).length - 1;
    if (count !== 1) {
      throw new Error(`JUCE patch ${patch.id}: expected exactly one "${replacement.from}" in ${patch.file}, found ${count}`);
    }
    text = text.replace(from, () => to);
    changed = true;
  }
  return { text, changed };
}

function applyPatches(modulesDir = DEST) {
  for (const patch of PATCHES) {
    const file = path.join(modulesDir, patch.file);
    const result = applyPatchToText(patch, fs.readFileSync(file, 'utf8'));
    if (result.changed) fs.writeFileSync(file, result.text);
    console.log(`  patch ${patch.id}: ${result.changed ? 'applied' : 'already applied'}`);
  }
}

module.exports = { applyPatches, applyPatchToText, PATCHES };

if (require.main === module) applyPatches();
