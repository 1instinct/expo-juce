import { requireNativeModule } from 'expo-modules-core';

import type {
  Analysis,
  ArpOptions,
  AudioDiagnostics,
  BackingTrackGapless,
  BackingTrackInfo,
  BackingTrackOptions,
  DrumKitParams,
  DrumState,
  DrumVoiceParams,
  EffectsState,
  MetronomeOptions,
  SamplerLoadResult,
  SamplerLoop,
  SamplerSettings,
  SamplerSlotParams,
  SamplerState,
} from './ExpoJuce.types';

/** A partial slot update as sent to native code (`loop: false` = no loop). */
export type NativeSamplerSlotParams = Partial<Omit<SamplerSlotParams, 'loop'>> & { loop?: SamplerLoop | false };

// The native module (ios/ExpoJuceModule.swift; android/src/main/java/expo/modules/juce/ExpoJuceModule.kt). Functions added after 0.5.0 are optional,
// because an older binary does not export them: callers check `typeof fn === 'function'`.
export type ExpoJuceNativeModule = {
  PI: number;
  hello(): string;

  // Lifecycle. Binaries older than 0.5.2 take no argument (see setup() in index.ts).
  setup(options?: { bufferSize?: number; sampleRate?: number }): Promise<void>;
  shutdown(): Promise<void>;
  hasJucePadView?(): boolean;

  // Touch velocity (0.10.0-rc.3+; iOS has it, Android and web return false). The config as
  // touchVelocityConfigForNative sends it.
  hasTouchVelocity?(): boolean;
  setTouchVelocity?(config: Record<string, unknown>): boolean;

  // Diagnostics
  getDiagnostics(): AudioDiagnostics;
  playTestTone(durationMs: number): boolean;

  // Display-rate hint (0.9.0-rc.4+; iOS applies it, Android returns false)
  setDisplayRateHint?(hz: number): boolean;

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
  // `gapless` may also carry `sourceBpm` (0.8.0-rc.3+; see loadBackingTrack in index.ts).
  loadBackingTrack?(uri: string, gapless: (Partial<BackingTrackGapless> & { sourceBpm?: number }) | null): Promise<BackingTrackInfo>;
  unloadBackingTrack?(): void;
  getAnalysis?(): Analysis;

  // Arpeggiator (0.7.0+)
  setArpeggiator?(options: Partial<ArpOptions>): boolean;
  getArpeggiatorState?(): ArpOptions;

  // Drums (0.8.0+)
  hasDrums?(): boolean;
  drumHit?(voice: number, velocity: number): void;
  setDrumVoice?(voice: number, params: Partial<DrumVoiceParams>): boolean;
  setDrumKit?(params: Partial<DrumKitParams>): boolean;
  getDrumState?(): DrumState;

  // Sampler (0.10.0+, iOS). Params as index.ts sends them: finite numbers / booleans only, and
  // `loop` false (no loop) instead of null.
  hasSampler?(): boolean;
  loadSamplerSlot?(slot: number, uri: string, params: NativeSamplerSlotParams | null): Promise<SamplerLoadResult>;
  unloadSamplerSlot?(slot: number): boolean;
  setSamplerSlot?(slot: number, params: NativeSamplerSlotParams): boolean;
  setSampler?(settings: Partial<SamplerSettings>): boolean;
  samplerHit?(slot: number, velocity: number): void;
  samplerRelease?(slot: number): void; // releases every holder of the slot (per slot, not per caller)
  samplerNoteOn?(note: number, velocity: number): void;
  samplerNoteOff?(note: number): void;
  getSamplerState?(): SamplerState;

  // MIDI byte input (0.9.0-rc.2+, iOS)
  hasMidiInput?(): boolean;
  setMidiInputMap?(actions: number[], velocity: number[] | null): boolean;
  setMidiInputEnabled?(enabled: boolean): void;
  releaseMidiInputHolds?(): number;
  getMidiInputClock?(): number;
  midiInputBytes?(bytes: number[], format: number, arrival: number | null): number;

  // Transport
  startTransport(): void;
  stopTransport(): void;
  setTempo(bpm: number): void;
  setTransportPosition(beat: number): void;
  getTransportPosition(): number;
  getTempo(): number;
  isTransportPlaying(): boolean;

  // Loop sequence (0.9.0-rc.5+, iOS): flat data, 6 numbers per event (see setSequence in index.ts)
  hasSequencePlayer?(): boolean;
  // 0.10.0-rc.2+: kinds 2 (sampler pad hit) and 3 (sampler key note) are played.
  hasSequenceSampler?(): boolean;
  setSequence?(data: number[], loopBeats: number, fromBeat: number, endBeat: number): boolean;
  setSequenceMuted?(muted: boolean): void;
  setSequenceNotesListening?(listening: boolean): void;

  // Background, Now Playing, remote commands (0.9.0-rc.5+, iOS)
  setBackgroundPolicy?(options: { keepAlive?: boolean; idleStopSeconds?: number }): void;
  setNowPlaying?(info: { title: string; artist?: string; album?: string } | null): boolean;
  enableRemoteCommands?(commands: string[]): boolean;

  // Events
  setValueAsync(value: string): Promise<void>;
  // Event-emitter hooks (keeps the type compatible with expo-modules-core's EventEmitter).
  addListener(eventName: string): void;
  removeListeners(count: number): void;
};

// It loads the native module object from the JSI or falls back to
// the bridge module (from NativeModulesProxy) if the remote debugger is on.
export default requireNativeModule<ExpoJuceNativeModule>('ExpoJuce');
