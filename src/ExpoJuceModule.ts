import { requireNativeModule } from 'expo-modules-core';

import type { EffectsState } from './ExpoJuce.types';

// Native functions added after 0.5.x: optional, because an older binary does not export them
// (callers check `typeof fn === 'function'`). The rest of the module stays untyped.
export type ExpoJuceNativeEffects = {
  setEffectEnabled?(id: string, enabled: boolean): boolean;
  setEffectParam?(id: string, param: string, value: number): boolean;
  getEffectsState?(): EffectsState;
  // Event-emitter hooks (keeps the type compatible with expo-modules-core's EventEmitter).
  addListener?: any;
  removeListeners?: any;
};

// It loads the native module object from the JSI or falls back to
// the bridge module (from NativeModulesProxy) if the remote debugger is on.
export default requireNativeModule('ExpoJuce') as ExpoJuceNativeEffects & { [name: string]: any };
