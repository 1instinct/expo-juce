#!/usr/bin/env node
// Every C++ header in ios/ must be listed in the podspec's private_header_files. CocoaPods puts any
// header not listed there into the module's umbrella header, where it is compiled as Objective-C
// and the first C++ include (<algorithm>, <atomic>, ...) fails the whole module build.
const fs = require('fs');
const path = require('path');

const ios = path.join(__dirname, '..', 'ios');
const podspec = fs.readFileSync(path.join(ios, 'ExpoJuce.podspec'), 'utf8');
const block = podspec.match(/private_header_files\s*=\s*\[([\s\S]*?)\]/);
if (!block) {
  console.error('private_header_files not found in ExpoJuce.podspec');
  process.exit(1);
}
const listed = new Set([...block[1].matchAll(/"([^"]+\.h)"/g)].map((m) => m[1]));
const cpp = /#include\s*<|\bnamespace\b|\btemplate\s*<|\bstd::|^\s*(class|struct)\s+\w+[^;]*\{/m;

const missing = fs.readdirSync(ios)
  .filter((f) => f.endsWith('.h'))
  .filter((f) => cpp.test(fs.readFileSync(path.join(ios, f), 'utf8')))
  .filter((f) => !listed.has(f));

if (missing.length) {
  console.error(`C++ headers missing from private_header_files: ${missing.join(', ')}`);
  process.exit(1);
}
console.log(`podspec headers ok (${listed.size} private)`);
