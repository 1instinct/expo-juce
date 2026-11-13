#!/usr/bin/env node
// Compiles every ios/*.mm and ios/*.m for the iOS simulator with `clang -fsyntax-only`, using the
// settings CocoaPods gives the ExpoJuce target: the podspec's language standard, header search
// paths, OTHER_CPLUSPLUSFLAGS, preprocessor definitions, per-file ARC (requires_arc) and minimum
// iOS version, plus the prefix header CocoaPods generates for every pod (UIKit first). Debug
// configuration defines (DEBUG=1), so code inside jassert() is checked too.
//
// Needs macOS with Xcode and CocoaPods (`pod ipc spec` reads the podspec). Swift and linking are
// not covered; an app build is.
//
//   node test/check-ios-syntax.cjs [file.mm ...]
const { execFileSync, spawn } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

const ios = path.join(__dirname, '..', 'ios');
const spec = JSON.parse(execFileSync('pod', ['ipc', 'spec', path.join(ios, 'ExpoJuce.podspec')], {
  cwd: ios,
  encoding: 'utf8',
  stdio: ['ignore', 'pipe', 'inherit'],
}));
const xcconfig = spec.pod_target_xcconfig || {};
const minIos = spec.platforms && spec.platforms.ios;
if (!minIos) throw new Error('podspec has no iOS platform version');

const xcrun = (...args) => execFileSync('xcrun', args, { encoding: 'utf8' }).trim();
const sdk = xcrun('--sdk', 'iphonesimulator', '--show-sdk-path');
const clang = xcrun('--sdk', 'iphonesimulator', '--find', 'clang');
const clangxx = xcrun('--sdk', 'iphonesimulator', '--find', 'clang++');
const arch = process.arch === 'arm64' ? 'arm64' : 'x86_64';
const triple = `${arch}-apple-ios${minIos}-simulator`;

// Xcode build-setting values are space-separated, with "quoted" items; $(inherited) is dropped.
const words = (value) => [...String(value || '').matchAll(/"([^"]*)"|(\S+)/g)]
  .map((m) => (m[1] !== undefined ? m[1] : m[2]))
  .filter((w) => w !== '$(inherited)')
  .map((w) => w.replace(/\$\(PODS_TARGET_SRCROOT\)/g, ios));

const glob = (pattern) => new RegExp(`^${pattern.replace(/[.+^${}()|[\]\\]/g, '\\$&')
  .replace(/\*/g, '[^/]*').replace(/\?/g, '.')}$`);
const arc = spec.requires_arc === undefined ? true : spec.requires_arc;
const arcPatterns = Array.isArray(arc) ? arc.map(glob) : typeof arc === 'string' ? [glob(arc)] : null;
const usesArc = (file) => (arcPatterns ? arcPatterns.some((re) => re.test(file)) : arc === true);

// The prefix header CocoaPods writes to Target Support Files/<pod>/<pod>-prefix.pch.
const pch = path.join(fs.mkdtempSync(path.join(os.tmpdir(), 'expojuce-')), 'ExpoJuce-prefix.pch');
fs.writeFileSync(pch, [
  '#ifdef __OBJC__',
  '#import <UIKit/UIKit.h>',
  '#else',
  '#ifndef FOUNDATION_EXPORT',
  '#if defined(__cplusplus)',
  '#define FOUNDATION_EXPORT extern "C"',
  '#else',
  '#define FOUNDATION_EXPORT extern',
  '#endif',
  '#endif',
  '#endif',
  '',
].join('\n'));

const common = [
  '-fsyntax-only', '-target', triple, '-isysroot', sdk, '-include', pch,
  ...words(xcconfig.HEADER_SEARCH_PATHS).map((p) => `-I${p}`),
  ...['POD_CONFIGURATION_DEBUG=1', 'DEBUG=1', 'COCOAPODS=1', ...words(xcconfig.GCC_PREPROCESSOR_DEFINITIONS)]
    .map((d) => `-D${d}`),
];
const flagsFor = (file) => {
  const objcxx = file.endsWith('.mm');
  return [
    ...(objcxx
      ? ['-x', 'objective-c++', `-std=${xcconfig.CLANG_CXX_LANGUAGE_STANDARD || 'gnu++17'}`,
        '-stdlib=libc++', ...words(xcconfig.OTHER_CPLUSPLUSFLAGS)]
      : ['-x', 'objective-c', ...words(xcconfig.OTHER_CFLAGS)]),
    ...common,
    usesArc(file) ? '-fobjc-arc' : '-fno-objc-arc',
    path.join(ios, file),
  ];
};

const files = process.argv.length > 2
  ? process.argv.slice(2).map((f) => path.basename(f))
  : fs.readdirSync(ios).filter((f) => /\.(mm|m)$/.test(f)).sort();
// ExpoModulesCore is not on the search path: a file that includes it needs a stub include directory.
for (const f of files) {
  if (/ExpoModulesCore/.test(fs.readFileSync(path.join(ios, f), 'utf8'))) {
    throw new Error(`${f} includes ExpoModulesCore headers, which this check does not provide`);
  }
}

console.log(`${xcrun('clang', '--version').split('\n')[0]}; SDK ${path.basename(sdk)}; ${triple}`);
console.log(`ARC: ${files.filter(usesArc).join(', ')}`);
console.log(`no ARC: ${files.filter((f) => !usesArc(f)).join(', ')}`);
console.log(`flags (${files.find((f) => f.endsWith('.mm'))}): ${flagsFor(files.find((f) => f.endsWith('.mm'))).join(' ')}\n`);

const run = (file) => new Promise((resolve) => {
  const started = Date.now();
  const child = spawn(file.endsWith('.mm') ? clangxx : clang, flagsFor(file), { stdio: ['ignore', 'pipe', 'pipe'] });
  let out = '';
  child.stdout.on('data', (d) => { out += d; });
  child.stderr.on('data', (d) => { out += d; });
  child.on('close', (code) => {
    const secs = ((Date.now() - started) / 1000).toFixed(1);
    const warnings = (out.match(/: warning: /g) || []).length;
    if (code === 0) {
      console.log(`ok    ${file} (${secs}s${warnings ? `, ${warnings} warnings` : ''})`);
    } else {
      console.log(`FAIL  ${file} (${secs}s)\n${out}`);
    }
    resolve(code === 0);
  });
});

(async () => {
  const queue = [...files];
  const results = [];
  const worker = async () => {
    while (queue.length) results.push(await run(queue.shift()));
  };
  await Promise.all(Array.from({ length: Math.max(1, os.cpus().length) }, worker));
  const failed = results.filter((ok) => !ok).length;
  console.log(`\n${files.length - failed}/${files.length} files compile for the iOS simulator`);
  process.exit(failed ? 1 : 0);
})();
