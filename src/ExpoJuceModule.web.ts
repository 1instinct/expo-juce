import { EventEmitter } from 'expo-modules-core';

import type { EffectsState } from './ExpoJuce.types';
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
};
