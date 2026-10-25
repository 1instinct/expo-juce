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
  /** Fired on the first touch, after the note has been sent to the engine (visuals/haptics only). */
  onNoteOn?: (event: PadNoteOnEvent) => void;
  /** Fired when the last touch lifts or is cancelled, after the note-off was sent. */
  onNoteOff?: (event: PadNoteOffEvent) => void;
  style?: StyleProp<ViewStyle>;
  children?: React.ReactNode;
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
  sessionCategory: string;
  sessionSampleRate: number;
  sessionIOBufferMs: number;
  sessionOutputLatencyMs: number;
  otherAudioPlaying: boolean;
  /** AVAudioSession current route output port types. */
  routeOutputs: string[];
};
