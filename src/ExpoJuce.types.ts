import type * as React from 'react';
import type { StyleProp, ViewStyle } from 'react-native';

export type ChangeEventPayload = {
  value: string;
};

export type BeatEventPayload = {
  beat: number;
  bpm: number;
};

export type SetupOptions = {
  /** Preferred I/O buffer size in samples (default 128). The device grants the nearest size it supports. */
  bufferSize?: number;
  /** Preferred sample rate in Hz (default 48000). */
  sampleRate?: number;
};

/** Payload of `JucePadView` `onNoteOn`. */
export type PadNoteOnEvent = {
  note: number;
  velocity: number;
};

/** Payload of `JucePadView` `onNoteOff`. */
export type PadNoteOffEvent = {
  note: number;
};

export type JucePadViewProps = {
  /** MIDI note triggered while the pad is touched. */
  note: number;
  /** MIDI velocity 0-127 (default 100). */
  velocity?: number;
  /** When false, touches pass through and no note is played (default true). */
  enabled?: boolean;
  /**
   * Touch area as a polygon: flat `[x0, y0, x1, y1, …]` normalised to the view (0–1). Touches
   * outside it pass through to the views underneath. Empty, odd-length or fewer than 3 points:
   * the whole view (default). iOS, 0.6.0+.
   */
  hitPolygon?: number[];
  /** Fired on the first touch, after the note has been sent to the engine (visuals/haptics only). */
  onNoteOn?: (event: PadNoteOnEvent) => void;
  /** Fired when the last touch lifts or is cancelled, after the note-off was sent. */
  onNoteOff?: (event: PadNoteOffEvent) => void;
  style?: StyleProp<ViewStyle>;
  children?: React.ReactNode;
};

/**
 * Effects of the master chain (synth → effects → output), in processing order. A safety
 * limiter (-0.3 dBFS ceiling) always runs after them and is not user-facing.
 */
export type EffectId =
  | 'drive'
  | 'crush'
  | 'eq'
  | 'chorus'
  | 'phaser'
  | 'echo'
  | 'reverb'
  | 'pump'
  | 'compressor';

/** Parameter names per effect (ranges and defaults: see README "Effects"). */
export type EffectParams = {
  drive: 'amount' | 'tone' | 'mix';
  crush: 'bits' | 'downsample' | 'noise' | 'mix';
  eq: 'low' | 'mid' | 'midFreq' | 'high';
  chorus: 'rate' | 'depth' | 'mix';
  phaser: 'rate' | 'depth' | 'feedback' | 'mix';
  echo: 'time' | 'sync' | 'division' | 'feedback' | 'tone' | 'pingPong' | 'mix';
  reverb: 'size' | 'damping' | 'width' | 'mix';
  pump: 'depth' | 'shape' | 'division';
  compressor: 'threshold' | 'ratio' | 'attack' | 'release' | 'makeup';
};

/** One effect's state as stored natively (values after clamping). */
export type EffectState = {
  enabled: boolean;
  params: Record<string, number>;
};

/** Returned by `getEffectsState()`: every effect, enabled or not. */
export type EffectsState = Record<EffectId, EffectState>;

/** Metronome click sounds. */
export type MetronomeStyle = 'classic' | 'wood' | 'beep' | 'cowbell' | 'hat';

/** `setMetronome()` options; omitted keys keep their current value. */
export type MetronomeOptions = {
  /** Clicks while the transport plays (default false). */
  enabled?: boolean;
  /** Click sound (default 'classic'). */
  style?: MetronomeStyle;
  /** 0–1 (default 0.8). */
  level?: number;
  /** Higher click on beat 1 of the bar (default true). */
  accent?: boolean;
  /** 1–16, rounded (default 4). */
  beatsPerBar?: number;
};

/**
 * Gapless info of an MP3 (frames at the file's own rate, from its LAME/Xing tag): lets
 * `loadBackingTrack` cut the exact loop out of what the decoder returns.
 */
export type BackingTrackGapless = {
  loopSamples: number;
  leadingSamples: number;
  trailingSamples: number;
};

/** Resolved by `loadBackingTrack()`. Sample counts are frames at the file's own rate. */
export type BackingTrackInfo = {
  /** Length of the loop region in seconds. */
  durationSec: number;
  /** The file's sample rate (playback is resampled to the device rate). */
  sampleRate: number;
  channels: number;
  /** Where the loop region starts in the decoded audio. */
  loopStartSample: number;
  /** Length of the loop region. */
  loopSamples: number;
  /** Frames the decoder produced before the region was cut (absent on web). */
  decodedSamples?: number;
  /** Decoder used (e.g. "CoreAudio", "MP3 file", "WAV file"). */
  decoder?: string;
};

/** `setBackingTrackOptions()` options; omitted keys keep their current value. */
export type BackingTrackOptions = {
  /** 0–1 (default 0.8). */
  level?: number;
  /** Loop the track (default true); otherwise it plays once and then is silent. */
  loop?: boolean;
  /** Plays with the transport (default true). */
  enabled?: boolean;
};

/** Returned by `getAnalysis()`: the device output (synth, effects, metronome, backing track). */
export type Analysis = {
  /** Max |sample| of the last block, 0–1+. */
  peak: number;
  /** RMS of the last block (both channels). */
  rms: number;
  /** 128 points, oldest first, decimated (every 16th sample) from the last 2048 output samples (mono). */
  scope: number[];
  /** Last note the synth started; -1 if none yet. */
  lastNote: number;
  bpm: number;
  /** Transport position in beats. */
  beat: number;
  playing: boolean;
};

/** Native engine lifecycle state reported in `AudioDiagnostics.engineState`. */
export type EngineState = 'ready' | 'recovering' | 'failed' | 'uninitialized';

/**
 * Snapshot of native audio-engine state, returned by `getDiagnostics()`.
 * Counters are cumulative since setup; peaks are a decaying hold (~300 ms half-life)
 * of the max |sample| per block. All fields have safe defaults when not set up.
 */
export type AudioDiagnostics = {
  /** Native engine finished setup. */
  isReady: boolean;
  /** Last setup step reached (0-11); -1 if setup never ran. On failure, the failing step. */
  setupStep: number;
  /** Setup failure and/or non-fatal setup notes; empty string if none. */
  lastError: string;
  deviceName: string;
  deviceType: string;
  sampleRate: number;
  bufferSize: number;
  outputLatencySamples: number;
  /** Device IO callbacks received. */
  callbackCount: number;
  /** AudioProcessorGraph::processBlock calls made by the device callback. */
  graphProcessCount: number;
  /** PolySynthProcessor::processBlock calls. */
  synthProcessCount: number;
  /** Peak of the graph output written to the device (before test tone). */
  outputPeak: number;
  /** Peak of the synth output inside PolySynthProcessor. */
  synthPeak: number;
  /** Device blocks containing a non-finite (NaN/Inf) output sample. */
  nanCount: number;
  /** Cumulative test-tone samples actually mixed into the device output (audio thread). */
  toneSamplesMixed: number;
  /** Note-ons accepted into the note queue (cumulative since launch, across engine rebuilds). */
  notesQueued: number;
  /** Note-ons the synth has started (cumulative since launch). */
  notesConsumed: number;
  /** Note-ons dropped because the note queue was full. Note releases are never dropped. */
  notesDropped: number;
  activeVoices: number;
  /** ms from noteOn enqueue to the synth consuming it; -1 if none yet. */
  lastNoteToRenderMs: number;
  /** ms from the UITouch hardware timestamp (JucePadView) to the synth consuming the note; -1 if none yet. */
  lastTouchToRenderMs: number;
  /** Buffer size (samples) requested at setup; compare with `bufferSize` (granted). */
  preferredBufferSize: number;
  /** Sample rate (Hz) requested at setup; compare with `sampleRate` (granted). */
  preferredSampleRate: number;
  /** Times another component changed the session category and it was re-asserted to Playback. */
  sessionReassertCount: number;
  /** AVAudioSession interruptions (calls, Siri, alarms) begun since launch. */
  interruptionCount: number;
  /** AVAudioSession media-services resets handled (full engine rebuild) since launch. */
  mediaResetCount: number;
  /** Last session-recovery problem; empty string if none. */
  lastRecoveryError: string;
  /**
   * Engine lifecycle (0.5.2+; absent on older binaries): "ready", "recovering" (rebuild running,
   * retry scheduled, or a lifecycle section in progress), "failed" (setup failed or rebuild
   * retries exhausted), "uninitialized" (never set up, or shut down).
   */
  engineState?: EngineState;
  /** JS-thread calls dropped because the engine lifecycle lock was busy (0.5.2+). */
  lockBusyDrops?: number;
  /** Times the audio callback was found stalled and the device was reopened (0.5.2+). */
  deadDeviceRestarts?: number;
  /** FxChainProcessor::processBlock calls (0.6.0+). */
  fxProcessCount?: number;
  /** Lowest safety-limiter gain in the last effects block, 1 = not limiting (0.6.0+). */
  fxLimiterGain?: number;
  /** Effect chunks whose output went non-finite and were silenced + reset (0.6.0+). */
  fxNonFiniteResets?: number;
  /** Metronome clicks started (cumulative in the current engine, 0.6.0+). */
  metronomeClicks?: number;
  /** A backing track is loaded (0.6.0+). */
  backingTrackLoaded?: boolean;
  /** Backing-track playback position in seconds; -1 when not playing (0.6.0+). */
  backingTrackPositionSec?: number;
  sessionCategory: string;
  sessionSampleRate: number;
  sessionIOBufferMs: number;
  sessionOutputLatencyMs: number;
  otherAudioPlaying: boolean;
  /** AVAudioSession current route output port types. */
  routeOutputs: string[];
};
