#!/usr/bin/env node

/**
 * vendor-juce.js
 *
 * Copies the required JUCE modules from the local JUCE download
 * into ios/juce_modules/ for vendoring with the npm package.
 *
 * Strips unnecessary files (tests, demos, docs, non-iOS native code)
 * to keep the package size reasonable.
 *
 * Usage: node src/scripts/vendor-juce.js
 */

const fs = require('fs');
const path = require('path');

const ROOT = path.resolve(__dirname, '../..');
const JUCE_MODULES_SRC = path.join(ROOT, 'JUCE', 'JUCE-8.0.1', 'modules');
const DEST = path.join(ROOT, 'ios', 'juce_modules');

const MODULES_TO_VENDOR = [
  'juce_core',
  'juce_events',
  'juce_data_structures',
  'juce_audio_basics',
  'juce_audio_formats',
  'juce_dsp',
];

// Patterns to skip (relative to each module directory)
const SKIP_PATTERNS = [
  /[/\\]native[/\\]java[/\\]/,
  /[/\\]native[/\\]juce_android/,
  /[/\\]native[/\\]juce_linux/,
  /[/\\]native[/\\]juce_win/,
  /[/\\]native[/\\]juce_wasm/,
  /demo/i,
  /test/i,
  /\.txt$/i,
  /\.md$/i,
  /CMakeLists\.txt$/,
  /LICENSE$/,
];

function shouldSkip(relativePath) {
  return SKIP_PATTERNS.some(pattern => pattern.test(relativePath));
}

function copyDirRecursive(src, dest, baseDir) {
  if (!fs.existsSync(src)) return;

  fs.mkdirSync(dest, { recursive: true });

  const entries = fs.readdirSync(src, { withFileTypes: true });
  for (const entry of entries) {
    const srcPath = path.join(src, entry.name);
    const destPath = path.join(dest, entry.name);
    const relativePath = path.relative(baseDir, srcPath);

    if (shouldSkip(relativePath)) continue;

    if (entry.isDirectory()) {
      copyDirRecursive(srcPath, destPath, baseDir);
    } else {
      fs.copyFileSync(srcPath, destPath);
    }
  }
}

function getDirectorySize(dir) {
  let size = 0;
  if (!fs.existsSync(dir)) return 0;
  const entries = fs.readdirSync(dir, { withFileTypes: true });
  for (const entry of entries) {
    const fullPath = path.join(dir, entry.name);
    if (entry.isDirectory()) {
      size += getDirectorySize(fullPath);
    } else {
      size += fs.statSync(fullPath).size;
    }
  }
  return size;
}

function main() {
  // Verify JUCE source exists
  if (!fs.existsSync(JUCE_MODULES_SRC)) {
    console.error(`JUCE modules not found at: ${JUCE_MODULES_SRC}`);
    console.error('Run "npm run download-juce" first to download JUCE.');
    process.exit(1);
  }

  // Clean existing vendored modules
  if (fs.existsSync(DEST)) {
    console.log('Cleaning existing ios/juce_modules/...');
    fs.rmSync(DEST, { recursive: true });
  }

  console.log(`Vendoring ${MODULES_TO_VENDOR.length} JUCE modules...\n`);

  for (const mod of MODULES_TO_VENDOR) {
    const srcDir = path.join(JUCE_MODULES_SRC, mod);
    const destDir = path.join(DEST, mod);

    if (!fs.existsSync(srcDir)) {
      console.error(`Module not found: ${srcDir}`);
      process.exit(1);
    }

    copyDirRecursive(srcDir, destDir, srcDir);
    const size = (getDirectorySize(destDir) / (1024 * 1024)).toFixed(1);
    console.log(`  ${mod} → ${size} MB`);
  }

  const totalSize = (getDirectorySize(DEST) / (1024 * 1024)).toFixed(1);
  console.log(`\nTotal vendored size: ${totalSize} MB`);
  console.log(`Output: ${DEST}`);
}

main();
