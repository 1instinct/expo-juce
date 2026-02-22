import { NativeModulesProxy, EventEmitter, Subscription } from 'expo-modules-core';

import ExpoJuceModule from './ExpoJuceModule';
import ExpoJuceView from './ExpoJuceView';
import { ChangeEventPayload, BeatEventPayload, ExpoJuceViewProps } from './ExpoJuce.types';

export const PI = ExpoJuceModule.PI;

export function hello(): string {
  return ExpoJuceModule.hello();
}

// ── Synth ───────────────────────────────────────────────────────────

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

// ── DSP Params ──────────────────────────────────────────────────────

export function setAttack(ms: number): void {
  ExpoJuceModule.setAttack(ms);
}

export function setRelease(ms: number): void {
  ExpoJuceModule.setRelease(ms);
}

export function setFilterCutoff(hz: number): void {
  ExpoJuceModule.setFilterCutoff(hz);
}

export function setFilterResonance(q: number): void {
  ExpoJuceModule.setFilterResonance(q);
}

export function setDetune(cents: number): void {
  ExpoJuceModule.setDetune(cents);
}

export function setDSPValue(param: string, value: number): void {
  ExpoJuceModule.setDSPValue(param, value);
}

// ── Transport ───────────────────────────────────────────────────────

export function startTransport(): void {
  ExpoJuceModule.startTransport();
}

export function stopTransport(): void {
  ExpoJuceModule.stopTransport();
}

export function setTempo(bpm: number): void {
  ExpoJuceModule.setTempo(bpm);
}

export function setTransportPosition(beat: number): void {
  ExpoJuceModule.setTransportPosition(beat);
}

export function getTransportPosition(): number {
  return ExpoJuceModule.getTransportPosition();
}

export function getTempo(): number {
  return ExpoJuceModule.getTempo();
}

export function isTransportPlaying(): boolean {
  return ExpoJuceModule.isTransportPlaying();
}

// ── Events ──────────────────────────────────────────────────────────

export async function setValueAsync(value: string) {
  return await ExpoJuceModule.setValueAsync(value);
}

const emitter = new EventEmitter(ExpoJuceModule ?? NativeModulesProxy.ExpoJuce);

export function addChangeListener(listener: (event: ChangeEventPayload) => void): Subscription {
  return emitter.addListener<ChangeEventPayload>('onChange', listener);
}

export function addBeatListener(listener: (event: BeatEventPayload) => void): Subscription {
  return emitter.addListener<BeatEventPayload>('onBeat', listener);
}

export { ExpoJuceView, ExpoJuceViewProps, ChangeEventPayload, BeatEventPayload };
