import type * as React from 'react';
import type { StyleProp, ViewStyle } from 'react-native';

export type ChangeEventPayload = {
  value: string;
};

export type BeatEventPayload = {
  /** Transport position in beats (fractional) at the end of the audio block that crossed a beat. */
  beat: number;
  bpm: number;
  /** Transport position in beats (fractional) when the event was sent (0.8.0+; absent on older binaries). */
  position?: number;
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

/** What a `JucePadView` plays (0.8.0+). */
export type PadMode = 'notes' | 'drums';

export type JucePadViewProps = {
  /** MIDI note triggered while the pad is touched; in `drums` mode the drum voice (0–8). */
  note: number;
  /**
   * `notes` (default): the pad holds `note` on the synth while touched. `drums`: every touch-down
   * hits drum voice `note` natively (one-shot, same velocity; `onNoteOn` per hit, `onNoteOff` when
   * the last touch lifts). Changing the mode releases the pad. Binaries older than 0.8.0 ignore it.
   */
  mode?: PadMode;
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

/** Loop region chosen by `loadBackingTrack()` (see `BackingTrackInfo.regionRule`). */
export type BackingTrackRegionRule = 'tag' | 'padded' | 'paddedNoTagFrame' | 'whole';

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
  /** Frames the decoder produced before the region was cut. */
  decodedSamples?: number;
  /**
   * Which loop region was used (0.6.0-rc.1+): 'tag' (the decoder honoured the gapless tag),
   * 'padded' (tag frame decoded as silence, padding cut), 'paddedNoTagFrame' (tag frame skipped,
   * padding cut), 'whole' (no gapless info, or a length matching no case: the whole file).
   */
  regionRule?: BackingTrackRegionRule;
  /** Decoder used (e.g. "CoreAudio", "MP3 file", "WAV file"). */
  decoder?: string;
  /**
   * The tempo the track follows from (0.8.0-rc.3+): the `sourceBpm` passed in, 0 if none was or it
   * was not a positive finite number. Absent on older binaries, which play every track at its own
   * speed: use its presence to tell whether the running binary follows the tempo.
   */
  sourceBpm?: number;
};

/** `loadBackingTrack()` options beyond the file (0.8.0-rc.3+; older binaries ignore them). */
export type BackingTrackLoadOptions = {
  /**
   * The file's own tempo. With it the track follows the transport tempo at its own pitch (time
   * stretch), locked to the beat; at exactly this tempo it plays untouched. 0 / omitted: the track
   * plays at its own speed whatever the tempo.
   */
  sourceBpm?: number;
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

/** Arpeggiator note order (0.7.0+). */
export type ArpMode = 'up' | 'down' | 'upDown' | 'played' | 'random' | 'chord';

/**
 * Arpeggiator step rate (0.7.0+). Steps per beat: '1/4' 1, '1/8' 2, '1/8T' 3, '1/8D' 4/3,
 * '1/16' 4, '1/16T' 6, '1/32' 8.
 */
export type ArpRate = '1/4' | '1/8' | '1/8T' | '1/8D' | '1/16' | '1/16T' | '1/32';

/**
 * `setArpeggiator()` options / `getArpeggiatorState()` result (0.7.0+). In `setArpeggiator`
 * omitted keys keep their current value; numbers are clamped to their range; an unknown `mode`
 * or `rate` rejects the whole call (nothing applied).
 */
export type ArpOptions = {
  /** Arpeggiate held notes instead of playing them directly (default false). */
  enabled: boolean;
  /** Note order (default 'up'). */
  mode: ArpMode;
  /** Step rate relative to the transport tempo (default '1/16'). */
  rate: ArpRate;
  /** 1–4 octaves of the held notes in the cycle (default 1). */
  octaves: number;
  /** Note length, 0.05–1.5 of a step (default 0.6); > 1 overlaps the next step (legato). */
  gate: number;
  /** 0–0.5 of a step added to odd steps (default 0). */
  swing: number;
  /** Keep playing after every note is released; the next press starts a new chord (default false). */
  latch: boolean;
  /** 1–16 steps, true = the step plays (default 16 × true). */
  pattern: boolean[];
  /** 0–1 (default 0): steps other than step 0 play at velocity × (1 − 0.6 × accent). */
  accent: number;
  /** 1–4 hits per step (default 1). */
  ratchet: number;
  /** 0–8 pattern passes before the transposition wraps (default 0 = off). */
  transposeSteps: number;
  /** −12..12 semitones added per pass (default 0). */
  transposeDistance: number;
};

/**
 * One drum voice's parameters (0.8.0+). Voices in pad order: 0 kick, 1 snare, 2 closed hat,
 * 3 open hat, 4 clap, 5 low tom, 6 high tom, 7 rim, 8 mid tom (0.8.0-rc.2+).
 */
export type DrumVoiceParams = {
  /** Semitones, −24..24 (default 0). */
  tune: number;
  /** 0–1 (default 0.5): scales the voice's envelopes, 0 = ×0.25, 0.5 = ×1, 1 = ×4. */
  decay: number;
  /** 0–1 (default 0.5): voice-specific brightness / noise mix (see README "Drums"). */
  tone: number;
  /** 0–1 (default 0.8). */
  level: number;
};

/** Kit-wide drum parameters (0.8.0+). */
export type DrumKitParams = {
  /** 0–1 (default 0.8). */
  level: number;
  /** Silences the kit (default false). */
  mute: boolean;
  /** 0–1 soft saturation of the kit sum (default 0 = clean). */
  drive: number;
};

/** `getDrumState()` (0.8.0+): the kit and every voice (9; 8 before 0.8.0-rc.2), as stored natively (after clamping). */
export type DrumState = { kit: DrumKitParams; voices: DrumVoiceParams[] };

/** Arpeggiator state in `Analysis.arp` (0.7.0+), as of the last rendered block. */
export type ArpAnalysis = {
  /** The arp is enabled and holds notes (a step is sounding or coming). */
  active: boolean;
  /** Pattern slot of the last step started (0-based). */
  step: number;
  /** Pattern length. */
  steps: number;
  /** Last note the arp started; -1 if none yet. */
  note: number;
};

/** Returned by `getAnalysis()`: the device output (synth, effects, metronome, backing track). */
export type Analysis = {
  /** Max |sample| since the previous getAnalysis() call (peak hold), 0–1+. */
  peak: number;
  /** RMS (both channels) since the previous getAnalysis() call. */
  rms: number;
  /** 128 points, oldest first, decimated (every 16th sample) from the last 2048 output samples (mono). */
  scope: number[];
  /**
   * 24 log-spaced bands, 40 Hz – 16 kHz, each 0–1 (−60..0 dBFS of the loudest FFT bin in the
   * band) from the last 2048 output samples (0.7.0+; 24 zeros on older binaries).
   */
  bands: number[];
  /** Sample rate the bands were computed at (0 when the engine is not running / older binaries). */
  sampleRate: number;
  /** Last note the synth started; -1 if none yet. */
  lastNote: number;
  /** Transport tempo, position (beats) and play state as of the last rendered block. */
  bpm: number;
  beat: number;
  playing: boolean;
  /** Arpeggiator state (0.7.0+; inactive on older binaries). */
  arp: ArpAnalysis;
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
  /** Transport tempo / the playing track's own tempo; 1 = untouched or no tempo (0.8.0-rc.3+). */
  backingTrackTempoRatio?: number;
  /** Drum hits the engine has started (cumulative since launch, 0.8.0+). */
  drumHits?: number;
  /** Peak of the drum part, before the effects (decaying hold, 0.8.0+). */
  drumPeak?: number;
  sessionCategory: string;
  sessionSampleRate: number;
  sessionIOBufferMs: number;
  sessionOutputLatencyMs: number;
  otherAudioPlaying: boolean;
  /** AVAudioSession current route output port types. */
  routeOutputs: string[];
  /** "android" on Android (0.9.0+); absent on iOS. The AVAudioSession fields above are iOS-only: empty / zero on Android. */
  platform?: 'android';
  /** Android: the device's native output sample rate (Hz) the engine asked for; 0 = unknown (0.9.0+). */
  nativeSampleRate?: number;
  /**
   * Output underruns. Android (0.9.0+): reported by the audio stream since it opened; -1 = unknown.
   * iOS (0.9.0-rc.1+): since setup, summed over every device the engine opened (JUCE's count: gaps
   * in the device's sample time plus callbacks that overran their time budget).
   */
  xrunCount?: number;
  /** Android: the app is in the background and the audio device is closed (0.9.0+). */
  backgrounded?: boolean;
  /** Android: times the device was closed because the app went to the background (0.9.0+). */
  backgroundStops?: number;
  /** Android: times it was reopened when the app came back (0.9.0+). */
  foregroundRestarts?: number;

  // ── iOS, 0.9.0-rc.1+ (absent on older binaries and on Android) ──
  /** The session's sample rate at setup, before the device was created: the route's own rate (Hz). */
  hardwareSampleRate?: number;
  /** The sample rate the engine asked the device for: the route's rate (`preferredSampleRate` is only a hint). */
  requestedSampleRate?: number;
  /** The sample rate the device runs at (same as `sampleRate`). */
  grantedSampleRate?: number;
  /** The buffer size the engine asked the device for: the size in `availableBufferSizes` nearest to `preferredBufferSize`. */
  requestedBufferSize?: number;
  /** The buffer size the device runs at (same as `bufferSize`). */
  grantedBufferSize?: number;
  /** Buffer sizes the device lists (what it was granted for 64, 128, 256 … frames at the current rate). */
  availableBufferSizes?: number[];
  /** The last 8 device opens the engine made (setup, recoveries, route changes), oldest first. */
  deviceOpens?: DeviceOpenRecord[];
  /** Touch → sound, per leg, over the last 64 notes / drum hits started by a native pad (`JucePadView`). */
  touchLatency?: TouchLatencyDiagnostics;
  /** Gesture recognizers and scroll views between the most recently attached pad and its window (at most 30). */
  touchPath?: TouchPathEntry[];
  /** The last pad touch (at most one report every 0.5 s): its delivery time, position and recognizers. */
  lastTouch?: LastTouchReport | Record<string, never>;
  /** Longest gap between main-thread display-link ticks in the last 10 s (ms); 0 when not set up. */
  mainThreadMaxGapMs?: number;
  /** Main-thread gaps longer than 50 ms since launch. */
  mainThreadStalls50?: number;
};

/** One device open made by the iOS engine (`AudioDiagnostics.deviceOpens`). */
export type DeviceOpenRecord = {
  /** "setup", "route change", "interruption", "dead device (…)", "stopped device (…)". */
  context: string;
  /** CACurrentMediaTime() in ms (time since boot), to order and space the entries. */
  atMs: number;
  preferredBufferSize: number;
  requestedBufferSize: number;
  grantedBufferSize: number;
  requestedSampleRate: number;
  grantedSampleRate: number;
  /** A new device object was created (a reopen) rather than the open one reconfigured. */
  newDevice: boolean;
  /** Empty when the open succeeded. */
  error: string;
};

/** last / p50 / p95 / max in ms over the last 64 values; count = values ever recorded. -1 when none. */
export type LatencyLegStats = {
  last: number;
  p50: number;
  p95: number;
  max: number;
  count: number;
};

export type TouchLatencyDiagnostics = {
  /** UITouch.timestamp → touchesBegan entry: iOS delivering the touch to the pad. */
  touchDeliveryMs: LatencyLegStats;
  /** touchesBegan entry → note queue push: the pad's own code. */
  touchHandlerMs: LatencyLegStats;
  /** Note queue push → the render block that consumed it. */
  queueToRenderMs: LatencyLegStats;
  /** UITouch.timestamp → render block (the whole in-app path; `lastTouchToRenderMs` is its `last`). */
  touchToRenderMs: LatencyLegStats;
  /** One device buffer (bufferSize / sampleRate) in ms. */
  bufferMs: number;
  /** AVAudioSession.outputLatency in ms. */
  outputLatencyMs: number;
};

/** A gesture recognizer or scroll view on the path from a pad to its window. depth 0 = the pad. */
export type TouchPathEntry =
  | {
      kind: 'recognizer';
      class: string;
      /** Class of the view the recognizer is attached to. */
      view: string;
      depth: number;
      enabled: boolean;
      delaysTouchesBegan: boolean;
      delaysTouchesEnded: boolean;
      cancelsTouchesInView: boolean;
    }
  | {
      kind: 'scrollView';
      class: string;
      depth: number;
      scrollEnabled: boolean;
      delaysContentTouches: boolean;
      canCancelContentTouches: boolean;
    };

/** `AudioDiagnostics.lastTouch`. Recognizers are the ones the touch was delivered to (`UITouch.gestureRecognizers`). */
export type LastTouchReport = {
  deliveryMs: number;
  x: number;
  y: number;
  windowWidth: number;
  windowHeight: number;
  /** Distance from the touch to the nearest window edge (points). */
  edgeDistance: number;
  recognizers: Omit<Extract<TouchPathEntry, { kind: 'recognizer' }>, 'depth'>[];
};
