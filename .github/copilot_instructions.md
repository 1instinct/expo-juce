# Copilot Instructions for expo-juce

## Project Overview

**expo-juce** is an Expo module that enables realtime DSP (Digital Signal Processing) with C++ and JUCE in React Native applications. It bridges JavaScript/TypeScript with native audio processing for ultra-low latency audio playback and synthesis.

### Key Technologies
- **Expo Modules API** - Modern React Native native module framework
- **JUCE Framework** - C++ audio framework (v8.0.1)
- **AVFoundation** - iOS audio framework for low-latency playback
- **Swift** - iOS module implementation
- **TypeScript** - API surface and type definitions

## Architecture

### File Structure
```
expo-juce/
├── src/                          # TypeScript API layer
│   ├── index.ts                  # Main exports, event emitters
│   ├── ExpoJuceModule.ts         # Native module bridge
│   ├── ExpoJuceModule.web.ts     # Web fallback implementation
│   ├── ExpoJuceView.tsx          # Native view component
│   ├── ExpoJuce.types.ts         # TypeScript type definitions
│   └── scripts/
│       └── bundle-juce.js        # Downloads JUCE on install
├── ios/                          # iOS native implementation
│   ├── ExpoJuceModule.swift      # Expo Module definition
│   ├── ExpoJuceBridge.h/m        # Objective-C wrapper
│   ├── JuceToneGenerator.h/mm    # Audio engine (Obj-C++)
│   ├── ExpoJuceView.swift        # Native view
│   ├── ExpoJuce.podspec          # CocoaPods specification
│   └── ExpoJuce-Bridging-Header.h
├── android/                      # Android native (future)
├── example/                      # Test app
└── JUCE/                         # Downloaded JUCE framework
```

### Data Flow
```
JavaScript/TypeScript (App.tsx)
    ↓ calls
ExpoJuceModule (index.ts)
    ↓ bridges to
ExpoJuceModule.swift
    ↓ calls
ExpoJuceBridge.m (Obj-C wrapper)
    ↓ calls
JuceToneGenerator.mm (Audio Engine)
    ↓ uses
AVFoundation (iOS Audio APIs)
```

## Core Components

### 1. Audio Engine (`JuceToneGenerator.mm`)
- **Purpose**: Real-time audio synthesis with minimal latency
- **Technology**: AVAudioEngine + AVAudioSourceNode
- **Features**:
  - 5ms buffer size for ultra-low latency
  - Real-time audio callback on dedicated thread
  - Sine wave oscillator in C++
  - Singleton pattern for lifecycle management
- **Key Methods**:
  - `initialize()` - Sets up audio session and engine
  - `playToneWithFrequency:duration:` - Plays sine wave
  - `shutdown()` - Cleans up audio resources

### 2. Objective-C Bridge (`ExpoJuceBridge`)
- **Purpose**: Simplify Swift ↔ Objective-C++ interop
- **Why Needed**: Swift cannot directly import Objective-C++ headers
- **Pattern**: Thin wrapper around JuceToneGenerator
- **Note**: Keep this layer minimal and stateless

### 3. Expo Module (`ExpoJuceModule.swift`)
- **Purpose**: Define JavaScript API using Expo Modules DSL
- **Key Patterns**:
  - `OnCreate` - Initialize audio on module load
  - `OnDestroy` - Cleanup on module unload
  - `Function` - Sync JavaScript functions
  - `AsyncFunction` - Promise-based async functions
  - `Constants` - Compile-time constants
  - `Events` - Event emitters for JS

### 4. TypeScript API (`src/index.ts`)
- **Purpose**: Type-safe JavaScript interface
- **Features**:
  - Named exports for tree-shaking
  - Event subscription management
  - Platform-specific module resolution (`.web.ts`)

## Development Guidelines

### When Adding New Features

1. **Audio DSP Features**:
   - Add to `JuceToneGenerator.mm` first
   - Keep audio callback lock-free and real-time safe
   - Use atomic types for thread-safe parameter changes
   - Test latency with high-frequency parameter changes

2. **JavaScript API**:
   - Add TypeScript types in `ExpoJuce.types.ts`
   - Export from `src/index.ts`
   - Update README with usage examples
   - Implement web fallback in `ExpoJuceModule.web.ts`

3. **Native Bridge**:
   - Add Swift function in `ExpoJuceModule.swift`
   - Add Objective-C wrapper in `ExpoJuceBridge.m`
   - Ensure proper Swift/Objective-C naming conventions

### Code Style

**Swift**:
- Use Expo Modules DSL patterns
- Prefer `Function` over `AsyncFunction` for low-latency calls
- Use explicit return types for clarity

**Objective-C/C++**:
- Keep C++ isolated to `.mm` files
- Use modern Objective-C features (`@property`, blocks)
- Follow Apple naming conventions

**TypeScript**:
- Use strict typing
- Export types alongside functions
- Prefer named exports over default

### Build System

**iOS CocoaPods**:
- `ExpoJuce.podspec` defines module configuration
- Key settings:
  - `SWIFT_OBJC_BRIDGING_HEADER` for Swift/ObjC interop
  - `CLANG_CXX_LANGUAGE_STANDARD = c++17` for C++
  - `frameworks = 'AVFoundation'` for audio
- Podfile in example includes:
  - `ENABLE_USER_SCRIPT_SANDBOXING = NO` for Xcode 15+
  - Post-install hooks for project setup

**TypeScript Build**:
- `expo-module build` compiles TypeScript
- Output goes to `build/` directory
- Source maps generated for debugging

**Postinstall**:
- `bundle-juce.js` downloads JUCE framework automatically
- Caches in `JUCE/` directory
- Only downloads if not present

### Common Issues & Solutions

**"Cannot find 'JuceToneGenerator' in scope"**:
- Bridging header not configured correctly
- Solution: Ensure `ExpoJuce-Bridging-Header.h` imports `ExpoJuceBridge.h`
- Use Objective-C wrapper, not direct Objective-C++ imports

**"Module cache" errors**:
- Stale Xcode derived data
- Solution: `rm -rf ~/Library/Developer/Xcode/DerivedData/*`

**"User Script Sandboxing" errors**:
- Xcode 15+ security feature blocking build scripts
- Solution: Podfile sets `ENABLE_USER_SCRIPT_SANDBOXING = NO`

**CocoaPods PATH issues**:
- Ruby/Node version conflicts
- Solution: Use consistent Ruby version (via RVM or rbenv)
- Ensure Node is in PATH when running pod commands

## Testing

### Example App
- Located in `example/`
- Demonstrates tone generation button
- Shows module constants and functions
- Run with: `cd example && yarn ios`

### Manual Testing Checklist
- [ ] Tone plays immediately (< 10ms latency)
- [ ] No audio glitches or pops
- [ ] Module initializes without errors
- [ ] Hot reload works
- [ ] Multiple tones can be triggered rapidly
- [ ] App survives backgrounding/foregrounding

## Future Roadmap

### Planned Features
1. **Android Support**:
   - Oboe library for low-latency audio
   - Similar architecture to iOS
   - JNI bridge for C++ audio engine

2. **Advanced Audio**:
   - ADSR envelope generators
   - Multiple oscillator types (saw, square, triangle)
   - Audio file playback
   - Effects (reverb, delay, filters)
   - MIDI input support

3. **JUCE Integration**:
   - Expose more JUCE modules
   - Plugin hosting (VST/AU)
   - Audio graph routing
   - Real-time parameter automation

4. **Performance**:
   - Benchmark latency
   - Optimize for battery life
   - Profile audio thread CPU usage

## Contributing

### Before Making Changes
1. Read this document completely
2. Test on real devices (simulators have different audio behavior)
3. Ensure no regressions in example app
4. Update TypeScript types if changing API
5. Update README with usage examples

### Pull Request Checklist
- [ ] Code builds without warnings on iOS
- [ ] TypeScript compiles without errors
- [ ] Example app runs and demonstrates feature
- [ ] README updated if API changed
- [ ] No breaking changes (or documented as such)

## Resources

- [Expo Modules API](https://docs.expo.dev/modules/module-api/)
- [JUCE Documentation](https://juce.com/learn/documentation)
- [AVFoundation Guide](https://developer.apple.com/av-foundation/)
- [Real-Time Audio Programming](https://www.rossbencina.com/code/real-time-audio-programming-101-time-waits-for-nothing)

## Notes for AI Assistants

- Always preserve low-latency audio characteristics
- Avoid allocations in audio callback
- Test on real hardware, not simulators
- Swift/Objective-C++ requires bridging - use wrapper pattern
- JUCE download happens in postinstall
- Module lifecycle (OnCreate/OnDestroy) is critical for audio cleanup
