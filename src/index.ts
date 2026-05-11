import { NativeModulesProxy, EventEmitter, Subscription } from 'expo-modules-core';

import ExpoJuceModule from './ExpoJuceModule';
import ExpoJuceView from './ExpoJuceView';
import { ChangeEventPayload, BeatEventPayload, ExpoJuceViewProps } from './ExpoJuce.types';

// ── Lifecycle ────────────────────────────────────────────────────

export async function setup(): Promise<void> {
  return ExpoJuceModule.setup();
}

export async function shutdown(): Promise<void> {
  return ExpoJuceModule.shutdown();
}

// ── Polyphonic Synth (MIDI-style) ────────────────────────────────

export async function noteOn(note: number, velocity: number): Promise<void> {
  return ExpoJuceModule.noteOn(note, velocity);
}

export async function noteOff(note: number): Promise<void> {
  return ExpoJuceModule.noteOff(note);
}

export async function allNotesOff(): Promise<void> {
  return ExpoJuceModule.allNotesOff();
}

// ── Global Synth Params ──────────────────────────────────────────

export async function setWaveform(waveform: 'sine' | 'square' | 'saw' | 'triangle'): Promise<void> {
  return ExpoJuceModule.setWaveform(waveform);
}

export async function setFilterCutoff(hz: number): Promise<void> {
  return ExpoJuceModule.setFilterCutoff(hz);
}

export async function setFilterResonance(q: number): Promise<void> {
  return ExpoJuceModule.setFilterResonance(q);
}

export async function setAttack(ms: number): Promise<void> {
  return ExpoJuceModule.setAttack(ms);
}

export async function setDecay(ms: number): Promise<void> {
  return ExpoJuceModule.setDecay(ms);
}

export async function setSustain(level: number): Promise<void> {
  return ExpoJuceModule.setSustain(level);
}

export async function setRelease(ms: number): Promise<void> {
  return ExpoJuceModule.setRelease(ms);
}

export async function setDetune(cents: number): Promise<void> {
  return ExpoJuceModule.setDetune(cents);
}

export async function setMasterLevel(level: number): Promise<void> {
  return ExpoJuceModule.setMasterLevel(level);
}

// ── Transport ────────────────────────────────────────────────────

export async function startTransport(): Promise<void> {
  return ExpoJuceModule.startTransport();
}

export async function stopTransport(): Promise<void> {
  return ExpoJuceModule.stopTransport();
}

export async function setTempo(bpm: number): Promise<void> {
  return ExpoJuceModule.setTempo(bpm);
}

export async function setTransportPosition(beat: number): Promise<void> {
  return ExpoJuceModule.setTransportPosition(beat);
}

export async function getTransportPosition(): Promise<number> {
  return ExpoJuceModule.getTransportPosition();
}

export async function getTempo(): Promise<number> {
  return ExpoJuceModule.getTempo();
}

export async function isTransportPlaying(): Promise<boolean> {
  return ExpoJuceModule.isTransportPlaying();
}

// ── Events ───────────────────────────────────────────────────────

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
