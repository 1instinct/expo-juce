import { EventEmitter } from 'expo-modules-core';

import type {
  Analysis,
  BackingTrackGapless,
  BackingTrackInfo,
  BackingTrackOptions,
  EffectsState,
  MetronomeOptions,
} from './ExpoJuce.types';
import { defaultEffectsState } from './effectDefaults';

const emitter = new EventEmitter({} as any);

export default {
  PI: Math.PI,
  async setValueAsync(value: string): Promise<void> {
    emitter.emit('onChange', { value });
  },
  hello() {
    return 'Hello world! 👋';
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
      lastNote: -1,
      bpm: 120,
      beat: 0,
      playing: false,
    };
  },
};
