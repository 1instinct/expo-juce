import { requireNativeModule } from 'expo-modules-core';

import type {
  Analysis,
  ArpOptions,
  AudioDiagnostics,
  BackingTrackGapless,
  BackingTrackInfo,
  BackingTrackOptions,
  EffectsState,
  MetronomeOptions,
} from './ExpoJuce.types';

// The native module (ios/ExpoJuceModule.swift). Functions added after 0.5.0 are optional,
// because an older binary does not export them: callers check `typeof fn === 'function'`.
export type ExpoJuceNativeModule = {
  PI: number;
  hello(): string;

  // Lifecycle. Binaries older than 0.5.2 take no argument (see setup() in index.ts).
  setup(options?: { bufferSize?: number; sampleRate?: number }): Promise<void>;
  shutdown(): Promise<void>;
  hasJucePadView?(): boolean;

  // Diagnostics
  getDiagnostics(): AudioDiagnostics;
  playTestTone(durationMs: number): boolean;

  // Synth
  noteOn(note: number, velocity: number): void;
  noteOff(note: number): void;
  allNotesOff(): void;
  setWaveform(waveform: string): void;
  setFilterCutoff(hz: number): void;
  setFilterResonance(q: number): void;
  setAttack(ms: number): void;
  setDecay(ms: number): void;
  setSustain(level: number): void;
  setRelease(ms: number): void;
  setDetune(cents: number): void;
  setMasterLevel(level: number): void;

  // Effects (0.6.0+)
  setEffectEnabled?(id: string, enabled: boolean): boolean;
  setEffectParam?(id: string, param: string, value: number): boolean;
  getEffectsState?(): EffectsState;

  // Metronome, backing track, analysis (0.6.0+)
  setMetronome?(options: MetronomeOptions): boolean;
  setBackingTrackOptions?(options: BackingTrackOptions): void;
  loadBackingTrack?(uri: string, gapless: BackingTrackGapless | null): Promise<BackingTrackInfo>;
  unloadBackingTrack?(): void;
  getAnalysis?(): Analysis;

  // Arpeggiator (0.7.0+)
  setArpeggiator?(options: Partial<ArpOptions>): boolean;
  getArpeggiatorState?(): ArpOptions;

  // Transport
  startTransport(): void;
  stopTransport(): void;
  setTempo(bpm: number): void;
  setTransportPosition(beat: number): void;
  getTransportPosition(): number;
  getTempo(): number;
  isTransportPlaying(): boolean;

  // Events
  setValueAsync(value: string): Promise<void>;
  // Event-emitter hooks (keeps the type compatible with expo-modules-core's EventEmitter).
  addListener(eventName: string): void;
  removeListeners(count: number): void;
};

// It loads the native module object from the JSI or falls back to
// the bridge module (from NativeModulesProxy) if the remote debugger is on.
export default requireNativeModule<ExpoJuceNativeModule>('ExpoJuce');
