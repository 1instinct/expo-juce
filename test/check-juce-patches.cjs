#!/usr/bin/env node
// Every vendored JUCE file that src/scripts/juce-patches.cjs patches must equal pristine JUCE
// 8.0.1 plus those patches (line endings aside), and applying the patches again must change
// nothing. A re-vendor (npm run vendor-juce) therefore reproduces the committed tree exactly.
//
// Pristine sources come from JUCE/JUCE-8.0.1/modules (npm run download-juce) when present,
// otherwise from the 8.0.1 tag on GitHub (cached in node_modules/.cache/juce-8.0.1).
//
// Usage: node test/check-juce-patches.cjs [--pristine <JUCE modules dir>]
const fs = require('fs');
const path = require('path');
const { PATCHES, applyPatchToText } = require('../src/scripts/juce-patches.cjs');

const ROOT = path.join(__dirname, '..');
const VENDORED = path.join(ROOT, 'ios', 'juce_modules');
const JUCE_TAG = '8.0.1';
const argIndex = process.argv.indexOf('--pristine');
const LOCAL_PRISTINE = argIndex > 0 ? path.resolve(process.argv[argIndex + 1])
                                    : path.join(ROOT, 'JUCE', `JUCE-${JUCE_TAG}`, 'modules');
const CACHE = path.join(ROOT, 'node_modules', '.cache', `juce-${JUCE_TAG}`);

const lf = (s) => s.split('\r\n').join('\n');

async function pristine(file) {
  const local = path.join(LOCAL_PRISTINE, file);
  if (fs.existsSync(local)) return { text: fs.readFileSync(local, 'utf8'), from: local };
  const cached = path.join(CACHE, file);
  if (!fs.existsSync(cached)) {
    const url = `https://raw.githubusercontent.com/juce-framework/JUCE/${JUCE_TAG}/modules/${file}`;
    const response = await fetch(url);
    if (!response.ok) throw new Error(`GET ${url}: HTTP ${response.status}`);
    fs.mkdirSync(path.dirname(cached), { recursive: true });
    fs.writeFileSync(cached, await response.text());
  }
  return { text: fs.readFileSync(cached, 'utf8'), from: cached };
}

async function main() {
  const failures = [];
  const files = [...new Set(PATCHES.map((p) => p.file))];
  for (const file of files) {
    const vendored = fs.readFileSync(path.join(VENDORED, file), 'utf8');
    const patches = PATCHES.filter((p) => p.file === file);

    // 1. pristine + patches == vendored
    const source = await pristine(file);
    let expected = lf(source.text);
    for (const patch of patches) {
      const result = applyPatchToText(patch, expected);
      if (!result.changed) failures.push(`${patch.id}: did not change pristine ${file}`);
      expected = result.text;
    }
    if (expected !== lf(vendored)) {
      const a = expected.split('\n');
      const b = lf(vendored).split('\n');
      let line = 0;
      while (line < a.length && a[line] === b[line]) ++line;
      failures.push(`${file}: vendored differs from pristine ${JUCE_TAG} + patches at line ${line + 1}\n` +
                    `    expected: ${JSON.stringify(a[line])}\n    vendored: ${JSON.stringify(b[line])}`);
    }

    // 2. idempotent: applying again to the vendored file (its own line endings) changes nothing
    let again = vendored;
    for (const patch of patches) {
      const result = applyPatchToText(patch, again);
      if (result.changed) failures.push(`${patch.id}: not idempotent on vendored ${file}`);
      again = result.text;
    }

    // 3. one line-ending style per file, so a re-vendor + patch keeps it
    if (vendored.includes('\r\n') && /(^|[^\r])\n/.test(vendored)) failures.push(`${file}: mixed line endings`);

    console.log(`  ${file}: ${patches.map((p) => p.id).join(', ')} (pristine: ${path.relative(ROOT, source.from)})`);
  }
  if (failures.length) {
    console.error(`JUCE patch check FAILED:\n  ${failures.join('\n  ')}`);
    process.exit(1);
  }
  console.log(`JUCE patches ok: ${files.length} vendored files == pristine ${JUCE_TAG} + ${PATCHES.length} patches, idempotent`);
}

main().catch((error) => {
  console.error(error);
  process.exit(1);
});
