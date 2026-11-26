# expo-juce

An Expo module that embeds JUCE 8 (8.0.1) in an Expo / React Native app: a JUCE-owned audio device
drives an `AudioProcessorGraph` (poly synth, drums, sampler, effects chain, transport,
metronome and backing track), controlled from JS through synchronous JSI calls. iOS is
complete, Android is a partial port (0.9.0+) and web is a stub. [README.md](README.md) is the
API reference; [.github/copilot_instructions.md](.github/copilot_instructions.md) has the file
map and native conventions.

## Where this repo sits

expo-juce is one of the DNA boilerplates, with `1instinct/dna-admin` and
`1instinct/dna-frontend`. Apps depend on it as the `expo-juce` npm package rather than forking
it, so every release reaches every app that upgrades.

## The rule: generalized, product-agnostic features only

1. The module is a general-purpose audio engine. No product names, devices, instruments,
   presets, copy or app-specific behaviour in code, comments, tests, docs or the example app.
1. A capability an app needs (a MIDI input path, a pad view mode, a diagnostics field) is
   built here in general form and configured from JS. The app's own instruments, sounds and
   device mappings live in the app.
1. When an app's need is product specific, it stays in the app; expo-juce at most exposes the
   general hook it needs.

## Working here

1. `main` is the trunk; work lands by PR. Pushing a tag `v<version>` publishes to npm (`next`
   for `-rc` versions, `latest` otherwise); see [RELEASING.md](RELEASING.md).
1. The audio thread never locks, allocates or calls into JS. Note and parameter changes cross
   threads through the lock-free queues; keep to that.
1. JUCE is vendored into `ios/juce_modules` (`npm run vendor-juce`) with the patches in
   `src/scripts/juce-patches.cjs`; edit the patches, not the vendored files.
1. CI runs `npm ci` (which builds and type-checks `src/`), `npm run test:touch-velocity`, a
   package-contents check, the offline audio harness in `test/harness` and an iOS syntax check.
   `npm run check:headers` and `npm run check:juce-patches` are local checks to run when
   touching the podspec or the vendored JUCE.
1. Record user-visible changes in [CHANGELOG.md](CHANGELOG.md) and the README section for the
   feature.

## Commits and PRs

No attribution lines (no `Co-Authored-By`, no generated-by footers). Lists use repeated `1.` so
items can be reordered without renumbering.

```
[Feature|Fix|Chore]: Sentence-case summary of the outcome, changes:
1. What changed, naming the file or symbol, and why
1. Next change
```

PR titles match the commit subject. PR bodies use `## What`, `## Why`, `## Changes` and
`## Verify` sections with `1.` lists.
