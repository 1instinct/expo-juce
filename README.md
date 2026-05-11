# expo-juce

Realtime DSP w/C++ & JUCE

## Roadmap: Full Audio Workstation

expo-juce is being built toward full audio workstation capabilities. The work is decomposed into independent sub-projects, each building on the previous:

### Sub-project 1: Polyphonic Synth Engine (current)
**Status:** In progress — [spec](docs/superpowers/specs/2026-05-11-polyphonic-synth-engine-design.md)

Replace the mono SynthEngine + AVAudioEngine with a JUCE-owned audio stack built on `AudioProcessorGraph`. 8-voice polyphonic synth with per-voice oscillator, SVF filter, and full ADSR envelope. MIDI-style API (`noteOn(note, velocity)` / `noteOff(note)`). JUCE owns device management and output for cross-platform support (iOS, macOS, Windows, Linux).

### Sub-project 2: Effects Chain
**Status:** Planned

Add `AudioProcessor` nodes for standard effects: reverb, delay, distortion, EQ, compressor. Inserted into the graph after the synth node on a master effects bus. Each effect is an independent processor — can be bypassed, reordered, or removed.

### Sub-project 3: Sample Playback / Drum Machine
**Status:** Planned

`SamplerProcessor` node that loads audio files and triggers playback via `noteOn`. Supports multi-sample mapping (e.g., different samples per MIDI note). Integrates with the transport for sequenced playback.

### Sub-project 4: Audio Input Processing
**Status:** Planned

Route mic/line input through the `AudioProcessorGraph` for realtime effects processing (guitar pedal board style). Audio session is already configured as `playAndRecord` from sub-project 1. Add an input node to the graph routed through effects processors.

### Sub-project 5: Modular Routing
**Status:** Planned

Expose the `AudioProcessorGraph` topology to TypeScript — add, remove, and connect nodes from JS. Enables user-configurable signal chains, parallel processing, send/return buses, and sidechain routing.

### Sub-project 6: Electron / Desktop
**Status:** Planned

Compile the same C++ engine for macOS, Windows, and Linux. JUCE abstracts platform audio (CoreAudio, WASAPI, ALSA). Package as an Electron native addon. Same graph, same processors, same API — different platform.

## Quick Start

### Installation

```bash
npm install expo-juce
```

### iOS Setup

After installing, run:

```bash
npx pod-install
```

### Android Setup

No additional setup required.

## Basic Usage

### Playing a Tone

```tsx
import { Button } from 'react-native';
import * as ExpoJuce from 'expo-juce';

function App() {
  const playTone = () => {
    const frequency = 440; // A4 note (Hz)
    const duration = 0.5;  // seconds
    ExpoJuce.playTone(frequency, duration);
  };

  return <Button title="Play Tone" onPress={playTone} />;
}
```

### Available Functions

- `hello()` - Returns a greeting string
- `getApiKey()` - Returns the API key
- `playTone(frequency: number, duration: number)` - Plays a sine wave tone at the specified frequency for the given duration
- `setValueAsync(value: string)` - Async function that emits a change event
- `PI` - Constant value of π

### Events

Listen to change events:

```tsx
import { addChangeListener } from 'expo-juce';

const subscription = addChangeListener((event) => {
  console.log('Value changed:', event.value);
});

// Clean up
subscription.remove();
```

## Low-Latency Audio

The module is configured for low-latency audio playback:
- 5ms buffer size
- Real-time audio processing thread
- AVAudioEngine on iOS
- Optimized for minimal latency

## Development

### Running the Example App

```bash
cd example
yarn install
yarn ios  # or yarn android
```

### Building the Module

```bash
npm run build
```

## Testing & Distribution

### For End Users to Test

Since this is a native module, users cannot test it with Expo Go. You have several options:

#### Option 1: Expo EAS Build (Recommended)
Install prebuilt version from EAS
<https://expo.dev/accounts/aaronsmulktis/projects/expo-juce-example/builds/a8f46b6b-60a4-4bfa-98fc-5c5458b45c60>

Create a development build that users can install on their devices:

```bash
cd example
eas build --profile development --platform ios
```

This creates a custom development client with your native module. Share the build URL from EAS.

#### Option 2: TestFlight (iOS)
1. Create a production build:
   ```bash
   cd example
   eas build --profile production --platform ios
   ```
2. Submit to TestFlight:
   ```bash
   eas submit --platform ios
   ```
3. Invite testers via App Store Connect

#### Option 3: Internal Distribution (iOS)
1. Build an ad-hoc IPA:
   ```bash
   cd example/ios
   xcodebuild -workspace expojuceexample.xcworkspace \
              -scheme expojuceexample \
              -configuration Release \
              -archivePath build/expojuceexample.xcarchive \
              archive
   ```
2. Export and share the IPA file
3. Users install via device provisioning profiles

#### Option 4: GitHub Releases
1. Build APK/IPA files
2. Create a GitHub Release
3. Attach builds as release assets
4. Add installation instructions in release notes

**Note**: Native modules require a custom development build. Users cannot simply scan a QR code like with Expo Go.

# API documentation

- [Documentation for the main branch](https://github.com/expo/expo/blob/main/docs/pages/versions/unversioned/sdk/juce.md)
- [Documentation for the latest stable release](https://docs.expo.dev/versions/latest/sdk/juce/)

# Installation in managed Expo projects

For [managed](https://docs.expo.dev/archive/managed-vs-bare/) Expo projects, please follow the installation instructions in the [API documentation for the latest stable release](#api-documentation). If you follow the link and there is no documentation available then this library is not yet usable within managed projects &mdash; it is likely to be included in an upcoming Expo SDK release.

# Installation in bare React Native projects

For bare React Native projects, you must ensure that you have [installed and configured the `expo` package](https://docs.expo.dev/bare/installing-expo-modules/) before continuing.

### Add the package to your npm dependencies

```
npm install expo-juce
```

### Configure for iOS

Run `npx pod-install` after installing the npm package.


### Configure for Android

## Running the Example App

1. `cd example`
1. `cd ios`
1. `npx pod-install`
1. Open workspace in XCode
1. Update `ENABLE_USER_SCRIPT_SANDBOXING` to NO in Build Settings

# Contributing

Contributions are very welcome! Please refer to guidelines described in the [contributing guide]( https://github.com/expo/expo#contributing).
