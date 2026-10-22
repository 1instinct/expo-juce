export type ChangeEventPayload = {
  value: string;
};

export type BeatEventPayload = {
  beat: number;
  bpm: number;
};

export type ExpoJuceViewProps = {
  name: string;
};

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
  notesQueued: number;
  notesConsumed: number;
  activeVoices: number;
  /** ms from noteOn enqueue to the synth consuming it; -1 if none yet. */
  lastNoteToRenderMs: number;
  sessionCategory: string;
  sessionSampleRate: number;
  sessionIOBufferMs: number;
  sessionOutputLatencyMs: number;
  otherAudioPlaying: boolean;
  /** AVAudioSession current route output port types. */
  routeOutputs: string[];
};
