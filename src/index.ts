import { NativeModulesProxy, EventEmitter, Subscription } from 'expo-modules-core';

import ExpoJuceModule from './ExpoJuceModule';
import JucePadView from './JucePadView';
import {
  ChangeEventPayload,
  BeatEventPayload,
  AudioDiagnostics,
  DeviceOpenRecord,
  LatencyLegStats,
  TouchLatencyDiagnostics,
  TouchPathEntry,
  LastTouchReport,
  StallLogEntry,
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
  BackingTrackRegionRule,
  BackingTrackOptions,
  BackingTrackLoadOptions,
  Analysis,
  ArpMode,
  ArpRate,
  ArpOptions,
  ArpAnalysis,
  PadMode,
  DrumVoiceParams,
  DrumKitParams,
  DrumState,
  MidiInputDiagnostics,
  MidiInputFormat,
  MidiInputAction,
  MidiInputMap,
  MidiInputEvent,
  MidiInputBatch,
  SequenceEvent,
  SequenceOptions,
  SequenceNote,
  SequenceNotesBatch,
  SequenceDiagnostics,
  BackgroundPolicy,
  BackgroundState,
  NowPlayingInfo,
  RemoteCommand,
  RemoteCommandEvent,
  AudioInterruptionEvent,
  TransportChangeEvent,
  BackgroundStateEvent,
  BackgroundPolicyDiagnostics,
  NowPlayingDiagnostics,
} from './ExpoJuce.types';
import { defaultEffectsState } from './effectDefaults';
import { ARP_DEFAULTS, arpDefaults } from './arpDefaults';
import { DRUM_KIT_DEFAULTS, DRUM_VOICE_COUNT, DRUM_VOICE_DEFAULTS, drumDefaults } from './drumDefaults';

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

// ── Display rate (0.9.0-rc.4+, iOS) ──────────────────────────────
// UIKit delivers touches on display frames, so a ProMotion screen held at 60 Hz can deliver a touch
// up to a 60 Hz frame late. A hint above 60 makes a main-thread CADisplayLink ask for (80, hz, hz)
// clamped to the screen's maximum, which keeps the display at that rate; 0 or 60 removes it. On
// iPhone the app must also set CADisableMinimumFrameDurationOnPhone in its Info.plist, or the
// display stays at 60. The hint does not need setup() and survives engine rebuilds; the link
// pauses while the app is inactive. Other display links and animations in the app may then run
// faster too: cap the ones that should not. getDiagnostics() reports displayHz (the link's
// measured rate), maxFramesPerSecond and displayRateHint.

/**
 * Asks for a display refresh rate in Hz (0 = no hint). Returns true when the binary took it (iOS,
 * applied on the main thread); false on Android, on web, on binaries older than 0.9.0-rc.4 and for
 * a non-finite value.
 */
export function setDisplayRateHint(hz: number): boolean {
  if (typeof ExpoJuceModule?.setDisplayRateHint !== 'function' || !Number.isFinite(hz)) return false;
  return ExpoJuceModule.setDisplayRateHint(hz) === true;
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
// Returns false (nothing applied) for an unknown style, or if the binary has no metronome.
export function setMetronome(options: MetronomeOptions): boolean {
  if (typeof ExpoJuceModule?.setMetronome !== 'function') return false;
  return ExpoJuceModule.setMetronome(options) !== false;
}

// Decodes a local audio file (file:// URI or absolute path; WAV, AIFF, FLAC, MP3, plus CoreAudio
// formats such as M4A on iOS) off the audio thread at the device rate and swaps it in (5 ms
// crossfade). It plays while the transport plays; track sample 0 is beat 0, so starts, resumes
// and setTransportPosition place it at beat x 60 / bpm. Without `options.sourceBpm` it plays at its
// own speed (bpm is the transport's, and a tempo change does not move it). With the file's own
// tempo (0.8.0-rc.3+) bpm is that tempo and the track follows the transport tempo at its own
// pitch, each beat on the transport's beat; at exactly that tempo it is the file untouched. The
// result's `sourceBpm` (absent on older binaries) says whether the binary does this.
// `gapless` (MP3 LAME/Xing tag values) picks the exact loop region: decoded length ==
// loopSamples -> [0, loopSamples) ('tag'); == 1152 + leading + loopSamples + trailing ->
// [1152 + leading, +loopSamples) ('padded'); == leading + loopSamples + trailing ->
// [leading, +loopSamples) ('paddedNoTagFrame'); otherwise the whole file ('whole'); the result's
// regionRule says which. A later load or unloadBackingTrack() supersedes a load in flight, which
// then rejects with 'ERR_BACKING_TRACK_SUPERSEDED'. Other rejections: ERR_BACKING_TRACK_URI,
// _FILE, _FORMAT, _TOO_LONG (over 6 min), _MEMORY (not enough memory available).
export async function loadBackingTrack(
  uri: string,
  gapless?: BackingTrackGapless,
  options?: BackingTrackLoadOptions
): Promise<BackingTrackInfo> {
  if (typeof ExpoJuceModule?.loadBackingTrack !== 'function') {
    const error = new Error('this binary has no backing-track support (expo-juce 0.6.0+)');
    (error as Error & { code?: string }).code = 'ERR_UNSUPPORTED';
    throw error;
  }
  // The tempo travels in the gapless dictionary, so the native signature is unchanged and older
  // binaries simply ignore it.
  const sourceBpm = options?.sourceBpm;
  if (typeof sourceBpm === 'number' && Number.isFinite(sourceBpm) && sourceBpm > 0) {
    return ExpoJuceModule.loadBackingTrack(uri, { ...(gapless ?? {}), sourceBpm });
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
  bands: new Array(24).fill(0),
  sampleRate: 0,
  lastNote: -1,
  bpm: 120,
  beat: 0,
  playing: false,
  arp: { active: false, step: 0, steps: 16, note: -1 },
});

// Output level (peak hold and RMS since the previous call), a 128-point oscilloscope, 24 spectrum
// bands (0.7.0+), the transport and the arpeggiator state for HUDs. Synchronous and lock-free
// (one 2048-point FFT per call): call it every frame, from one place. Zeros when the engine is
// not running or the binary is older than 0.6.0; binaries older than 0.7.0 return no `bands` /
// `sampleRate` / `arp`, which are filled in here.
export function getAnalysis(): Analysis {
  if (typeof ExpoJuceModule?.getAnalysis !== 'function') return emptyAnalysis();
  const native = ExpoJuceModule.getAnalysis() as Partial<Analysis>;
  if (native.bands !== undefined && native.arp !== undefined) return native as Analysis;
  return { ...emptyAnalysis(), ...native };
}

// ── Arpeggiator (0.7.0+) ─────────────────────────────────────────
// Between the note queue and the voices: while enabled, held notes (pads, keys, noteOn) are
// arpeggiated on the transport's grid (sample-accurate; free-running at the tempo while the
// transport is stopped). Options are synchronous and lock-free, remembered natively before
// setup() and across engine rebuilds. Binaries older than 0.7.0: setArpeggiator returns false
// and getArpeggiatorState returns the defaults.

export { ARP_DEFAULTS };

// Sets any subset of the options (omitted keys keep their value; numbers are clamped; `pattern`
// is 1–16 booleans, all or nothing). Returns false, applying nothing, for an unknown `mode` /
// `rate`, a `pattern` that is not 1–16 booleans, or a binary without an arpeggiator. Disabling stops the arp's sounding notes at once.
export function setArpeggiator(options: Partial<ArpOptions>): boolean {
  if (typeof ExpoJuceModule?.setArpeggiator !== 'function') return false;
  return ExpoJuceModule.setArpeggiator(options) === true;
}

// Every option as stored natively (after clamping).
export function getArpeggiatorState(): ArpOptions {
  if (typeof ExpoJuceModule?.getArpeggiatorState !== 'function') return arpDefaults();
  return ExpoJuceModule.getArpeggiatorState();
}

// ── Drums (0.8.0+) ───────────────────────────────────────────────
// Nine synthesized voices: 0 kick, 1 snare, 2 closed hat, 3 open hat (choked by the closed hat),
// 4 clap, 5 low tom, 6 high tom, 7 rim, 8 mid tom (0.8.0-rc.2+; older binaries have 0-7 and ignore
// 8: read getDrumState().voices.length). Each voice is monophonic: a hit restarts it.
// The drum part is mixed with the synth before the effects chain; the arpeggiator never applies
// to it. Parameters are synchronous and lock-free, remembered natively before setup() and across
// engine rebuilds. Binaries older than 0.8.0: hasDrums() is false, drumHit is a no-op, the setters
// return false and getDrumState returns the defaults.

export { DRUM_VOICE_COUNT, DRUM_VOICE_DEFAULTS, DRUM_KIT_DEFAULTS };

const DRUM_VOICE_KEYS = ['tune', 'decay', 'tone', 'level'] as const;

const isDrumVoice = (voice: number): boolean =>
  Number.isInteger(voice) && voice >= 0 && voice < DRUM_VOICE_COUNT;

/** True when this binary has the drum part (0.8+). */
export function hasDrums(): boolean {
  return typeof ExpoJuceModule?.hasDrums === 'function' && ExpoJuceModule.hasDrums() === true;
}

/** Trigger voice 0..8 at velocity 1..127. Thread-safe, lock-free; same queue as noteOn. Out-of-range voice is ignored. */
export function drumHit(voice: number, velocity: number): void {
  if (typeof ExpoJuceModule?.drumHit !== 'function') return;
  if (!isDrumVoice(voice) || !Number.isFinite(velocity)) return;
  ExpoJuceModule.drumHit(voice, Math.min(127, Math.max(1, Math.round(velocity))));
}

/** Partial update of one voice; unknown keys ignored; values clamped. Returns false if unsupported. */
export function setDrumVoice(voice: number, params: Partial<DrumVoiceParams>): boolean {
  if (typeof ExpoJuceModule?.setDrumVoice !== 'function') return false;
  if (!isDrumVoice(voice)) return false;
  const clean: Partial<DrumVoiceParams> = {};
  for (const key of DRUM_VOICE_KEYS) {
    const value = params?.[key];
    if (typeof value === 'number' && Number.isFinite(value)) clean[key] = value;
  }
  return ExpoJuceModule.setDrumVoice(voice, clean) === true;
}

/** Partial update of the kit (level 0–1, mute, drive 0–1); unknown keys ignored; values clamped. Returns false if unsupported. */
export function setDrumKit(params: Partial<DrumKitParams>): boolean {
  if (typeof ExpoJuceModule?.setDrumKit !== 'function') return false;
  const clean: Partial<DrumKitParams> = {};
  if (typeof params?.level === 'number' && Number.isFinite(params.level)) clean.level = params.level;
  if (typeof params?.drive === 'number' && Number.isFinite(params.drive)) clean.drive = params.drive;
  if (typeof params?.mute === 'boolean') clean.mute = params.mute;
  return ExpoJuceModule.setDrumKit(clean) === true;
}

/** The kit and the 8 voices as stored natively (after clamping); the defaults on older binaries. */
export function getDrumState(): { kit: DrumKitParams; voices: DrumVoiceParams[] } {
  if (typeof ExpoJuceModule?.getDrumState !== 'function') return drumDefaults();
  return ExpoJuceModule.getDrumState();
}

// ── MIDI byte input (0.9.0-rc.2+, iOS) ───────────────────────────
// For MIDI that arrives in the app's own native code (a Bluetooth notification callback, a serial
// port): that code calls the C function expojuce_midi_input_bytes (ios/ExpoJuceMidiInput.h) with
// the raw bytes, on its own thread. The engine parses them, plays what the input map says
// straight from the note queue (no JS on the way), and reports every parsed message here in
// batches (onMidiInput, about 4 ms after the first message of a burst) with its arrival time and
// whether it was played. The map is pushed from JS and swapped atomically. Note-offs always end
// the holds the input started, whatever the map says by then; a note another source of the
// engine holds (noteOn from JS, a JucePadView) is left to that source's release. Binaries without
// it: hasMidiInput() is false, the setters return false and no events arrive.

export const MIDI_INPUT_FORMATS: Record<MidiInputFormat, number> = {
  bleMidi: 0,
  rawStream: 1,
  rawPackets: 2,
};

/** True when this binary has the MIDI byte input. */
export function hasMidiInput(): boolean {
  return typeof ExpoJuceModule?.hasMidiInput === 'function' && ExpoJuceModule.hasMidiInput() === true;
}

const DRUM_ACTION_BASE = 256;

function encodeMidiInputAction(a: MidiInputAction | undefined): number {
  if (a == null) return -1;
  if ('drum' in a) return isDrumVoice(a.drum) ? DRUM_ACTION_BASE + a.drum : -1;
  return Number.isInteger(a.note) && a.note >= 0 && a.note <= 127 ? a.note : -1;
}

/**
 * Replaces the input map (whole; readers see the old map or the new one). Returns false if the
 * binary has no MIDI input. Does not change `enabled` (setMidiInputEnabled).
 */
export function setMidiInputMap(map: MidiInputMap): boolean {
  if (typeof ExpoJuceModule?.setMidiInputMap !== 'function') return false;
  const actions = new Array<number>(128);
  for (let i = 0; i < 128; i++) actions[i] = encodeMidiInputAction(map.actions?.[i]);
  let velocity: number[] | null = null;
  if (Array.isArray(map.velocity)) {
    velocity = new Array<number>(128);
    for (let i = 0; i < 128; i++) {
      const v = map.velocity[i];
      velocity[i] = typeof v === 'number' && Number.isFinite(v) ? Math.min(127, Math.max(1, Math.round(v))) : Math.max(1, i);
    }
  }
  return ExpoJuceModule.setMidiInputMap(actions, velocity) === true;
}

/** Off (the default): every note-on passes to the app. Returns false if unsupported. */
export function setMidiInputEnabled(enabled: boolean): boolean {
  if (typeof ExpoJuceModule?.setMidiInputEnabled !== 'function') return false;
  ExpoJuceModule.setMidiInputEnabled(enabled === true);
  return true;
}

/**
 * Ends every hold the input started (its device went away) and resets the parsers. Returns the
 * new release epoch (events from before it carry a lower one), or -1 if unsupported.
 */
export function releaseMidiInputHolds(): number {
  if (typeof ExpoJuceModule?.releaseMidiInputHolds !== 'function') return -1;
  return ExpoJuceModule.releaseMidiInputHolds();
}

/** Now on the input clock (seconds; `MidiInputEvent.arrival` uses it), or -1 if unsupported. */
export function getMidiInputClock(): number {
  if (typeof ExpoJuceModule?.getMidiInputClock !== 'function') return -1;
  return ExpoJuceModule.getMidiInputClock();
}

/**
 * Bytes from JS through the same path: one call is one packet, at most 512 bytes. Returns the
 * messages parsed, or -1 (unknown format, more than 512 bytes, unsupported binary).
 */
export function midiInputBytes(bytes: ArrayLike<number>, format: MidiInputFormat, arrivalSeconds?: number): number {
  if (typeof ExpoJuceModule?.midiInputBytes !== 'function') return -1;
  const code = MIDI_INPUT_FORMATS[format];
  if (code === undefined) return -1;
  return ExpoJuceModule.midiInputBytes(Array.from(bytes), code, arrivalSeconds ?? null);
}

/** Every parsed message, in batches. */
export function addMidiInputListener(listener: (batch: MidiInputBatch) => void): Subscription {
  return emitter.addListener<MidiInputBatch>('onMidiInput', listener);
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

// ── Loop sequence (0.9.0-rc.5+, iOS) ─────────────────────────────
// The native player fires the loop's notes on the audio thread at their exact sample against the
// transport, so it keeps time with the screen off and the JS thread busy. A pad, the MIDI input or
// noteOn holding the same note does not cut a sequence note, nor a sequence note-off a pad's.
// Jumps (setTransportPosition), stop, a new loop length and allNotesOff release what it holds; a
// new sequence keeps the notes that are still in it. Older binaries, Android and web lack it:
// hasSequencePlayer() is false and the setters return false (play the loop from JS there).

export const SEQUENCE_MAX_EVENTS = 8192;
const SEQUENCE_STRIDE = 6;

export function hasSequencePlayer(): boolean {
  return typeof ExpoJuceModule?.hasSequencePlayer === 'function' && ExpoJuceModule.hasSequencePlayer() === true;
}

/**
 * Replaces the loop (atomically: the audio thread never sees half of it). False (nothing changes)
 * without the native player, over SEQUENCE_MAX_EVENTS events or with a bad loop length; invalid
 * events (note out of range, length <= 0, start outside the loop) are skipped.
 */
export function setSequence(sequence: SequenceOptions): boolean {
  if (typeof ExpoJuceModule?.setSequence !== 'function') return false;
  const events = sequence.events ?? [];
  if (events.length > SEQUENCE_MAX_EVENTS) return false;
  const data = new Array<number>(events.length * SEQUENCE_STRIDE);
  for (let i = 0; i < events.length; i++) {
    const e = events[i];
    const o = i * SEQUENCE_STRIDE;
    data[o] = e.start;
    data[o + 1] = e.length;
    data[o + 2] = e.note;
    data[o + 3] = e.velocity;
    data[o + 4] = e.kind === 'drum' ? 1 : 0;
    data[o + 5] = typeof e.skipBefore === 'number' ? e.skipBefore : NaN;
  }
  const fromBeat = typeof sequence.fromBeat === 'number' ? sequence.fromBeat : -Infinity;
  const endBeat = typeof sequence.endBeat === 'number' ? sequence.endBeat : Infinity;
  if (ExpoJuceModule.setSequence(data, sequence.loopBeats, fromBeat, endBeat) !== true) return false;
  if (sequence.muted !== undefined) setSequenceMuted(sequence.muted);
  return true;
}

/** Removes the loop (its notes are released). */
export function clearSequence(): boolean {
  return setSequence({ loopBeats: 0, events: [] });
}

/** Silences the loop (its notes are released) or lets it play again. False without the player. */
export function setSequenceMuted(muted: boolean): boolean {
  if (typeof ExpoJuceModule?.setSequenceMuted !== 'function') return false;
  ExpoJuceModule.setSequenceMuted(muted === true);
  return true;
}

/**
 * The notes the sequence plays, in batches (about every 16 ms). The engine builds them only while
 * a listener is set. Nothing arrives without the native player.
 */
export function addSequenceNotesListener(listener: (batch: SequenceNotesBatch) => void): Subscription {
  const subscription = emitter.addListener<SequenceNotesBatch>('onSequenceNotes', listener);
  const counted = typeof ExpoJuceModule?.setSequenceNotesListening === 'function';
  if (counted) ExpoJuceModule.setSequenceNotesListening!(true);
  let removed = false;
  return {
    remove() {
      if (removed) return;
      removed = true;
      subscription.remove();
      if (counted) ExpoJuceModule.setSequenceNotesListening!(false);
    },
  } as Subscription;
}

// ── Background playback, Now Playing, remote commands (0.9.0-rc.5+, iOS) ──
// The host app needs the "audio" background mode. Until setBackgroundPolicy is called nothing
// changes when the app goes to the background. Each returns false where unsupported.

/** See BackgroundPolicy. Applies until the app's JS reloads. */
export function setBackgroundPolicy(policy: BackgroundPolicy): boolean {
  if (typeof ExpoJuceModule?.setBackgroundPolicy !== 'function') return false;
  const options: { keepAlive?: boolean; idleStopSeconds?: number } = {};
  if (policy.keepAlive !== undefined) options.keepAlive = policy.keepAlive === true;
  if (typeof policy.idleStopSeconds === 'number' && Number.isFinite(policy.idleStopSeconds)) {
    options.idleStopSeconds = policy.idleStopSeconds;
  }
  ExpoJuceModule.setBackgroundPolicy(options);
  return true;
}

/**
 * The lock screen / Control Center entry (null clears it). While it is set and the transport plays
 * (or the app is in the background) the audio session does not mix with other apps, which iOS
 * requires to show it: other apps' audio pauses.
 */
export function setNowPlaying(info: NowPlayingInfo | null): boolean {
  if (typeof ExpoJuceModule?.setNowPlaying !== 'function') return false;
  if (info === null) return ExpoJuceModule.setNowPlaying(null) === true;
  const clean: { title: string; artist?: string; album?: string } = { title: info.title };
  if (info.artist) clean.artist = info.artist;
  if (info.album) clean.album = info.album;
  return ExpoJuceModule.setNowPlaying(clean) === true;
}

/**
 * Lock screen / headphone commands handled by the native transport (it starts or stops at once,
 * also with the JS thread suspended); onRemoteCommand tells the app. [] removes them.
 */
export function enableRemoteCommands(commands: RemoteCommand[]): boolean {
  if (typeof ExpoJuceModule?.enableRemoteCommands !== 'function') return false;
  return ExpoJuceModule.enableRemoteCommands(commands.slice()) === true;
}

export function addRemoteCommandListener(listener: (event: RemoteCommandEvent) => void): Subscription {
  return emitter.addListener<RemoteCommandEvent>('onRemoteCommand', listener);
}

/** An audio interruption (a call, Siri) began or ended. The transport stops and does not resume. */
export function addAudioInterruptionListener(listener: (event: AudioInterruptionEvent) => void): Subscription {
  return emitter.addListener<AudioInterruptionEvent>('onAudioInterruption', listener);
}

/** The native side started or stopped the transport (interruption, remote command, background). */
export function addTransportChangeListener(listener: (event: TransportChangeEvent) => void): Subscription {
  return emitter.addListener<TransportChangeEvent>('onTransportChange', listener);
}

export function addBackgroundStateListener(listener: (event: BackgroundStateEvent) => void): Subscription {
  return emitter.addListener<BackgroundStateEvent>('onBackgroundState', listener);
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
  DeviceOpenRecord,
  LatencyLegStats,
  TouchLatencyDiagnostics,
  TouchPathEntry,
  LastTouchReport,
  StallLogEntry,
  EngineState,
  EffectId,
  EffectParams,
  EffectState,
  EffectsState,
  MetronomeStyle,
  MetronomeOptions,
  BackingTrackGapless,
  BackingTrackInfo,
  BackingTrackRegionRule,
  BackingTrackOptions,
  BackingTrackLoadOptions,
  Analysis,
  ArpMode,
  ArpRate,
  ArpOptions,
  ArpAnalysis,
  PadMode,
  DrumVoiceParams,
  DrumKitParams,
  DrumState,
  MidiInputDiagnostics,
  MidiInputFormat,
  MidiInputAction,
  MidiInputMap,
  MidiInputEvent,
  MidiInputBatch,
  SequenceEvent,
  SequenceOptions,
  SequenceNote,
  SequenceNotesBatch,
  SequenceDiagnostics,
  BackgroundPolicy,
  BackgroundState,
  NowPlayingInfo,
  RemoteCommand,
  RemoteCommandEvent,
  AudioInterruptionEvent,
  TransportChangeEvent,
  BackgroundStateEvent,
  BackgroundPolicyDiagnostics,
  NowPlayingDiagnostics,
};
