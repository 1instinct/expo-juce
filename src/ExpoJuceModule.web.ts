import { EventEmitter } from 'expo-modules-core';

import type {
  Analysis,
  ArpOptions,
  BackingTrackGapless,
  BackingTrackInfo,
  BackingTrackOptions,
  DrumKitParams,
  DrumState,
  DrumVoiceParams,
  EffectsState,
  MetronomeOptions,
  SamplerLoadResult,
  SamplerSettings,
  SamplerState,
} from './ExpoJuce.types';
import type { NativeSamplerSlotParams } from './ExpoJuceModule';
import { defaultEffectsState } from './effectDefaults';
import { arpDefaults } from './arpDefaults';
import { drumDefaults } from './drumDefaults';
import { samplerDefaults } from './samplerDefaults';

const emitter = new EventEmitter({} as any);

export default {
  PI: Math.PI,
  async setValueAsync(value: string): Promise<void> {
    emitter.emit('onChange', { value });
  },
  hello() {
    return 'Hello world! 👋';
  },
  // No display-rate hint on web.
  setDisplayRateHint(_hz: number): boolean {
    return false;
  },
  // No touch velocity on web: pads play their fixed velocity.
  hasTouchVelocity(): boolean {
    return false;
  },
  setTouchVelocity(_config: Record<string, unknown>): boolean {
    return false;
  },
  // No audio engine on web: effects calls are no-ops (false = not applied).
  setEffectEnabled(_id: string, _enabled: boolean): boolean {
    return false;
  },
  setEffectParam(_id: string, _param: string, _value: number): boolean {
    return false;
  },
  getEffectsState(): EffectsState {
    return defaultEffectsState();
  },
  // No metronome / backing track / analysis on web either.
  setMetronome(_options: MetronomeOptions): boolean {
    return false;
  },
  setBackingTrackOptions(_options: BackingTrackOptions): void {},
  async loadBackingTrack(_uri: string, _gapless: BackingTrackGapless | null): Promise<BackingTrackInfo> {
    const error = new Error('backing tracks are not supported on web');
    (error as Error & { code?: string }).code = 'ERR_UNSUPPORTED';
    throw error;
  },
  unloadBackingTrack(): void {},
  getAnalysis(): Analysis {
    return {
      peak: 0,
      rms: 0,
      scope: new Array(128).fill(0),
      bands: new Array(24).fill(0),
      sampleRate: 0,
      lastNote: -1,
      bpm: 120,
      beat: 0,
      playing: false,
      arp: { active: false, step: 0, steps: 16, note: -1 },
    };
  },
  // No arpeggiator on web: nothing applied, defaults reported.
  setArpeggiator(_options: Partial<ArpOptions>): boolean {
    return false;
  },
  getArpeggiatorState(): ArpOptions {
    return arpDefaults();
  },
  // No drums on web: hits are ignored, nothing applied, defaults reported.
  hasDrums(): boolean {
    return false;
  },
  drumHit(_voice: number, _velocity: number): void {},
  setDrumVoice(_voice: number, _params: Partial<DrumVoiceParams>): boolean {
    return false;
  },
  setDrumKit(_params: Partial<DrumKitParams>): boolean {
    return false;
  },
  getDrumState(): DrumState {
    return drumDefaults();
  },
  // No sampler on web: loads reject, hits are ignored, nothing applied, defaults reported.
  hasSampler(): boolean {
    return false;
  },
  // No sequence player on web (setSequence returns false), so no sampler events in it either.
  hasSequenceSampler(): boolean {
    return false;
  },
  async loadSamplerSlot(_slot: number, _uri: string, _params: NativeSamplerSlotParams | null): Promise<SamplerLoadResult> {
    const error = new Error('the sampler is not supported on web');
    (error as Error & { code?: string }).code = 'ERR_UNSUPPORTED';
    throw error;
  },
  unloadSamplerSlot(_slot: number): boolean {
    return false;
  },
  setSamplerSlot(_slot: number, _params: NativeSamplerSlotParams): boolean {
    return false;
  },
  setSampler(_settings: Partial<SamplerSettings>): boolean {
    return false;
  },
  samplerHit(_slot: number, _velocity: number): void {},
  samplerRelease(_slot: number): void {},
  samplerNoteOn(_note: number, _velocity: number): void {},
  samplerNoteOff(_note: number): void {},
  getSamplerState(): SamplerState {
    return samplerDefaults();
  },
};
