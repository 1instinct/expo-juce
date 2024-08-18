import { NativeModulesProxy, EventEmitter, Subscription } from 'expo-modules-core';

// Import the native module. On web, it will be resolved to ExpoJuce.web.ts
// and on native platforms to ExpoJuce.ts
import ExpoJuceModule from './ExpoJuceModule';
import ExpoJuceView from './ExpoJuceView';
import { ChangeEventPayload, ExpoJuceViewProps } from './ExpoJuce.types';

// Get the native constant value.
export const PI = ExpoJuceModule.PI;

export function hello(): string {
  return ExpoJuceModule.hello();
}

export function getApiKey(): string {
  return ExpoJuceModule.getApiKey();
}

export async function setValueAsync(value: string) {
  return await ExpoJuceModule.setValueAsync(value);
}

const emitter = new EventEmitter(ExpoJuceModule ?? NativeModulesProxy.ExpoJuce);

export function addChangeListener(listener: (event: ChangeEventPayload) => void): Subscription {
  return emitter.addListener<ChangeEventPayload>('onChange', listener);
}

export { ExpoJuceView, ExpoJuceViewProps, ChangeEventPayload };
