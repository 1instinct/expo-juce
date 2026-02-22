import { NativeModulesProxy, EventEmitter, Subscription } from 'expo-modules-core';

import ExpoJuceModule from './ExpoJuceModule';
import ExpoJuceView from './ExpoJuceView';
import { ChangeEventPayload, ExpoJuceViewProps } from './ExpoJuce.types';

export const PI = ExpoJuceModule.PI;

export function hello(): string {
  return ExpoJuceModule.hello();
}

export function playTone(frequency: number, duration: number): string {
  return ExpoJuceModule.playTone(frequency, duration);
}

export function setFrequency(frequency: number): void {
  ExpoJuceModule.setFrequency(frequency);
}

export function setLevel(level: number): void {
  ExpoJuceModule.setLevel(level);
}

export function setWaveform(waveform: 'sine' | 'square' | 'saw' | 'triangle'): void {
  ExpoJuceModule.setWaveform(waveform);
}

export function setHarmonicLevels(levels: number[]): void {
  ExpoJuceModule.setHarmonicLevels(levels);
}

export function noteOn(): void {
  ExpoJuceModule.noteOn();
}

export function noteOff(): void {
  ExpoJuceModule.noteOff();
}

export function setDSPValue(param: string, value: number): void {
  ExpoJuceModule.setDSPValue(param, value);
}

export async function setValueAsync(value: string) {
  return await ExpoJuceModule.setValueAsync(value);
}

const emitter = new EventEmitter(ExpoJuceModule ?? NativeModulesProxy.ExpoJuce);

export function addChangeListener(listener: (event: ChangeEventPayload) => void): Subscription {
  return emitter.addListener<ChangeEventPayload>('onChange', listener);
}

export { ExpoJuceView, ExpoJuceViewProps, ChangeEventPayload };
