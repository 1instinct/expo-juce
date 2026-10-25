import { NativeModulesProxy, EventEmitter, Subscription } from 'expo-modules-core';

import ExpoJuceModule from './ExpoJuceModule';
import JucePadView from './JucePadView';
import {
  ChangeEventPayload,
  BeatEventPayload,
  AudioDiagnostics,
  EngineState,
  SetupOptions,
  JucePadViewProps,
  PadNoteOnEvent,
  PadNoteOffEvent,
  EffectId,
  EffectParams,
  EffectState,
  EffectsState,
  MetronomeStyle,
  MetronomeOptions,
  BackingTrackGapless,
  BackingTrackInfo,
  BackingTrackOptions,
  Analysis,
} from './ExpoJuce.types';
import { defaultEffectsState } from './effectDefaults';

// ── Lifecycle ────────────────────────────────────────────────────

// Rejects with code 'ERR_AUDIO_SETUP' and message "step N: <error>" if native setup fails.
// options: preferred buffer size (samples, default 128) and sample rate (Hz, default 48000).
// No effect if the engine is already set up.
// Binary compatibility: binaries older than 0.5.2 export a zero-argument native setup and reject
// an options argument; 0.5.2+ binaries (detected by hasJucePadView) require the options argument
// (the native signature is setup(options, promise), so its required-argument count is 1).
export async function setup(options?: SetupOptions): Promise<void> {
  if (typeof ExpoJuceModule?.hasJucePadView !== 'function') {
    return ExpoJuceModule.setup();
  }
  const nativeOptions: { bufferSize?: number; sampleRate?: number } = {};
  if (options?.bufferSize !== undefined) nativeOptions.bufferSize = options.bufferSize;
  if (options?.sampleRate !== undefined) nativeOptions.sampleRate = options.sampleRate;
  return ExpoJuceModule.setup(nativeOptions);
}

// True if the native binary includes JucePadView (false on older binaries and on web).
export function isJucePadViewAvailable(): boolean {
  return (
    typeof ExpoJuceModule?.hasJucePadView === 'function' && ExpoJuceModule.hasJucePadView() === true
  );
}

export async function shutdown(): Promise<void> {
  return ExpoJuceModule.shutdown();
}

// ── Diagnostics ──────────────────────────────────────────────────

export function getDiagnostics(): AudioDiagnostics {
  return ExpoJuceModule.getDiagnostics();
}

// Mixes a 440 Hz sine directly into the device output (bypassing graph + synth).
// Returns true if the tone was queued; false if the engine is not ready or the
// audio device is not running (nothing is played in that case).
export function playTestTone(durationMs: number): boolean {
  return ExpoJuceModule.playTestTone(durationMs);
}

// ── Polyphonic Synth (MIDI-style) ────────────────────────────────
// Synchronous — uses lock-free SPSC queue, no main thread required

export function noteOn(note: number, velocity: number): void {
  ExpoJuceModule.noteOn(note, velocity);
}

export function noteOff(note: number): void {
  ExpoJuceModule.noteOff(note);
}

export function allNotesOff(): void {
  ExpoJuceModule.allNotesOff();
}

// ── Global Synth Params ──────────────────────────────────────────
// Synchronous — all params are std::atomic

export function setWaveform(waveform: 'sine' | 'square' | 'saw' | 'triangle'): void {
  ExpoJuceModule.setWaveform(waveform);
}

export function setFilterCutoff(hz: number): void {
  ExpoJuceModule.setFilterCutoff(hz);
}

export function setFilterResonance(q: number): void {
  ExpoJuceModule.setFilterResonance(q);
}

export function setAttack(ms: number): void {
  ExpoJuceModule.setAttack(ms);
}

export function setDecay(ms: number): void {
  ExpoJuceModule.setDecay(ms);
}

export function setSustain(level: number): void {
  ExpoJuceModule.setSustain(level);
}

export function setRelease(ms: number): void {
  ExpoJuceModule.setRelease(ms);
}

export function setDetune(cents: number): void {
  ExpoJuceModule.setDetune(cents);
}

export function setMasterLevel(level: number): void {
  ExpoJuceModule.setMasterLevel(level);
}

// ── Effects (0.6.0+) ─────────────────────────────────────────────
// Master chain: synth → drive → crush → eq → chorus → phaser → echo → reverb → pump →
// compressor → safety limiter → output. Synchronous and lock-free; settings are remembered
// natively whether or not the engine is running (before setup, across engine rebuilds).
// Binaries older than 0.6.0 have no chain: the setters return false and getEffectsState
// returns the defaults.

// Turns an effect on or off (short crossfade; an effect starts from a clear state; echo and
// reverb tails ring out after they are turned off).
// Returns false for an unknown effect, or if the binary has no effects chain.
export function setEffectEnabled(id: EffectId, enabled: boolean): boolean {
  if (typeof ExpoJuceModule?.setEffectEnabled !== 'function') return false;
  return ExpoJuceModule.setEffectEnabled(id, enabled) === true;
}

// Sets one parameter; the value is clamped to its range (switches / divisions rounded).
// Returns false for an unknown effect or parameter, a non-finite value, or no effects chain.
export function setEffectParam<T extends EffectId>(
  id: T,
  param: EffectParams[T] | (string & {}),
  value: number
): boolean {
  if (typeof ExpoJuceModule?.setEffectParam !== 'function') return false;
  return ExpoJuceModule.setEffectParam(id, param, value) === true;
}

// Every effect's enabled flag and parameter values, as stored natively (after clamping).
export function getEffectsState(): EffectsState {
  if (typeof ExpoJuceModule?.getEffectsState !== 'function') return defaultEffectsState();
  return ExpoJuceModule.getEffectsState();
}

// ── Metronome + backing track (0.6.0+) ───────────────────────────
// Both are summed into the output after the effects chain and follow the transport. Options
// are synchronous and lock-free, remembered natively before setup and across engine rebuilds.
// Binaries older than 0.6.0: the setters are no-ops and loadBackingTrack rejects with
// 'ERR_UNSUPPORTED'.

// Click on every beat while the transport plays, sample-accurate; accent on beat 1 of the bar.
export function setMetronome(options: MetronomeOptions): void {
  if (typeof ExpoJuceModule?.setMetronome !== 'function') return;
  ExpoJuceModule.setMetronome(options);
}

// Decodes a local audio file (file:// URI or absolute path; WAV, AIFF, FLAC, MP3, plus CoreAudio
// formats such as M4A on iOS) off the audio thread, resamples it to the device rate and swaps it
// in. It plays while the transport plays, from the bar the transport started on, and seeks with
// setTransportPosition. `gapless` (MP3 LAME/Xing tag values) picks the exact loop region: decoded
// length == loopSamples → [0, loopSamples); == 1152 + leading + loopSamples + trailing →
// [1152 + leading, +loopSamples); == leading + loopSamples + trailing → [leading, +loopSamples);
// otherwise the whole file. A later load or unloadBackingTrack()
// supersedes a load in flight, which then rejects with 'ERR_BACKING_TRACK_SUPERSEDED'.
// Other rejections: ERR_BACKING_TRACK_URI, _FILE, _FORMAT, _TOO_LONG (over 600 s).
export async function loadBackingTrack(
  uri: string,
  gapless?: BackingTrackGapless
): Promise<BackingTrackInfo> {
  if (typeof ExpoJuceModule?.loadBackingTrack !== 'function') {
    const error = new Error('this binary has no backing-track support (expo-juce 0.6.0+)');
    (error as Error & { code?: string }).code = 'ERR_UNSUPPORTED';
    throw error;
  }
  return ExpoJuceModule.loadBackingTrack(uri, gapless ?? null);
}

export function unloadBackingTrack(): void {
  if (typeof ExpoJuceModule?.unloadBackingTrack !== 'function') return;
  ExpoJuceModule.unloadBackingTrack();
}

export function setBackingTrackOptions(options: BackingTrackOptions): void {
  if (typeof ExpoJuceModule?.setBackingTrackOptions !== 'function') return;
  ExpoJuceModule.setBackingTrackOptions(options);
}

// ── Analysis (0.6.0+) ────────────────────────────────────────────

const emptyAnalysis = (): Analysis => ({
  peak: 0,
  rms: 0,
  scope: new Array(128).fill(0),
  lastNote: -1,
  bpm: 120,
  beat: 0,
  playing: false,
});

// Output level, a 128-point oscilloscope and transport state for HUDs. Synchronous and cheap
// (call it every frame). Zeros when the engine is not running or the binary is older than 0.6.0.
export function getAnalysis(): Analysis {
  if (typeof ExpoJuceModule?.getAnalysis !== 'function') return emptyAnalysis();
  return ExpoJuceModule.getAnalysis();
}

// ── Transport ────────────────────────────────────────────────────

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

export {
  JucePadView,
  JucePadViewProps,
  PadNoteOnEvent,
  PadNoteOffEvent,
  SetupOptions,
  ChangeEventPayload,
  BeatEventPayload,
  AudioDiagnostics,
  EngineState,
  EffectId,
  EffectParams,
  EffectState,
  EffectsState,
  MetronomeStyle,
  MetronomeOptions,
  BackingTrackGapless,
  BackingTrackInfo,
  BackingTrackOptions,
  Analysis,
};
