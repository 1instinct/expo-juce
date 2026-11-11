#!/usr/bin/env node

/**
 * juce-patches.cjs
 *
 * Local patches to the vendored JUCE modules (ios/juce_modules). vendor-juce.cjs applies them
 * after copying; run this file directly to (re)apply them to the existing vendored tree.
 * Each patch is an exact text replacement. Applying is idempotent (an already patched file is
 * left alone) and fails loudly when neither the original nor the patched text is found, which
 * means the JUCE version changed and the patch must be re-checked.
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
];

function applyPatches(modulesDir = DEST) {
  for (const patch of PATCHES) {
    const file = path.join(modulesDir, patch.file);
    let text = fs.readFileSync(file, 'utf8');
    let changed = false;
    for (const { from, to } of patch.replacements) {
      if (text.includes(to)) continue;
      const count = text.split(from).length - 1;
      if (count !== 1) {
        throw new Error(`JUCE patch ${patch.id}: expected exactly one "${from}" in ${patch.file}, found ${count}`);
      }
      text = text.replace(from, to);
      changed = true;
    }
    if (changed) fs.writeFileSync(file, text);
    console.log(`  patch ${patch.id}: ${changed ? 'applied' : 'already applied'}`);
  }
}

module.exports = { applyPatches, PATCHES };

if (require.main === module) applyPatches();
