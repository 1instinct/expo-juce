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
  /** The velocity the note or hit played at: estimated (with `touch`) or the fixed `velocity` prop. */
  velocity: number;
  /** The touch-down's signals, only when its velocity was estimated (`touchVelocity`, iOS, 0.10.0-rc.3+). */
  touch?: TouchSignals;
};

// ── Touch velocity (iOS, 0.10.0-rc.3+) ──────────────────────────────

/** How strength maps to velocity: `min + (max − min) · strength^γ`, γ 0.6 / 1.0 / 1.6. */
export type TouchVelocityCurve = 'soft' | 'normal' | 'hard';

/**
 * Where each signal's soft and hard ends lie, and how much each counts. Radius in points
 * (`UITouch.majorRadius`), impact in g (peak deviation of the acceleration from its pre-touch
 * baseline), weights 0–1 (relative; 0 leaves a signal out).
 */
export type TouchVelocityCalibration = {
  radiusSoft: number;
  radiusHard: number;
  impactSoft: number;
  impactHard: number;
  radiusWeight: number;
  impactWeight: number;
};

export type TouchVelocityConfig = {
  /** Off: every pad plays its fixed `velocity`. */
  enabled: boolean;
  /** Default `'normal'`. */
  curve?: TouchVelocityCurve;
  /** Velocity at strength 0 (default 20; 1–127). */
  minVelocity?: number;
  /** Velocity at strength 1 (default 127; 1–127). */
  maxVelocity?: number;
  /** null / omitted: the uncalibrated defaults (`TOUCH_VELOCITY_DEFAULTS.calibration`). */
  calibration?: TouchVelocityCalibration | null;
};

/** What a touch-down told the estimator (`onNoteOn` `touch`, diagnostics). */
export type TouchSignals = {
  /** `UITouch.majorRadius` (pt) of the strongest new touch. */
  radius: number;
  radiusTolerance: number;
  /**
   * `UITouch.force` / `maximumPossibleForce`; maxForce 0 = no force sensing. Force sets the strength
   * alone only above 5 % of maxForce.
   */
  force: number;
  maxForce: number;
  /** Device impact (g); null when the accelerometer had no fresh samples (off, stale or unavailable). */
  impact: number | null;
  /**
   * Age of the newest accelerometer sample at the touch handler (ms); null with no sample at all.
   * Signed: negative means a sample was stamped after the handler started (clock mismatch).
   */
  accelAgeMs: number | null;
  /** 0–1, before the curve. */
  strength: number;
};

/** One estimate in `getDiagnostics().touchVelocity`. */
export type TouchVelocityEstimate = TouchSignals & {
  velocity: number;
  /** Force alone set the strength. */
  forceUsed: boolean;
  /** Peak impact in the 50 ms after the touch (what waiting would have seen); null until known or when unavailable. */
  impactLate: number | null;
  /** UITouch.timestamp (s, the `clockSec` clock). */
  atSec: number;
};

/** `getDiagnostics().touchVelocity` (iOS, 0.10.0-rc.3+). */
export type TouchVelocityDiagnostics = {
  supported: boolean;
  enabled: boolean;
  curve: TouchVelocityCurve;
  min: number;
  max: number;
  /** The calibration as set (sanitised); null = defaults. */
  calibration: TouchVelocityCalibration | null;
  accel: {
    running: boolean;
    /** The device has an accelerometer (known once it was first started). */
    available: boolean;
    requestedHz: number;
    /** Samples per second over the last second. */
    measuredHz: number;
    /** Samples received since launch. */
    samples: number;
  };
  /** A pad's trait collection reports 3D Touch, or a touch carried force. */
  forceCapable: boolean;
  /** Pads with `touchVelocity` in a window. */
  pads: number;
  /** CACurrentMediaTime() when the diagnostics were read (s). */
  clockSec: number;
  last: TouchVelocityEstimate | null;
  /** The last 16 estimates, oldest first. */
  recent: TouchVelocityEstimate[];
};

/** Payload of `JucePadView` `onNoteOff`. */
export type PadNoteOffEvent = {
  note: number;
};

/** What a `JucePadView` plays (0.8.0+; `sampler` 0.10.0+). */
export type PadMode = 'notes' | 'drums' | 'sampler';

export type JucePadViewProps = {
  /**
   * MIDI note triggered while the pad is touched; in `drums` mode the drum voice (0–8); in
   * `sampler` mode the sampler slot (0–15).
   */
  note: number;
  /**
   * `notes` (default): the pad holds `note` on the synth while touched. `drums`: every touch-down
   * hits drum voice `note` natively (one-shot, same velocity; `onNoteOn` per hit, `onNoteOff` when
   * the last touch lifts). `sampler` (0.10.0+): every touch-down hits sampler slot `note` at its
   * root note (a second finger retriggers) and the last touch-up releases the slot (a one-shot
   * slot ignores the release and plays to its end). The release is per slot: it also ends a gated
   * sound another pad or `samplerHit` started on the same slot. Changing the mode releases the pad. Binaries
   * older than 0.8.0 ignore the mode; binaries older than 0.10.0 treat `sampler` as `notes`.
   */
  mode?: PadMode;
  /** MIDI velocity 0-127 (default 100). */
  velocity?: number;
  /**
   * Estimate each touch-down's velocity from how hard it lands (contact size, device impact,
   * force where the hardware has it) instead of playing `velocity`. Takes effect only while
   * `setTouchVelocity({ enabled: true })` is set; `onNoteOn` then carries `touch`. Default false.
   * iOS, 0.10.0-rc.3+ (older binaries, Android and web ignore it and play `velocity`).
   */
  touchVelocity?: boolean;
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

/** A sampler slot's loop (0.10.0+): ms of the sample; `endMs` -1 = the sample's end. */
export type SamplerLoop = { startMs: number; endMs: number };

/**
 * One sampler slot's playing parameters (0.10.0+). Slots are 0–15. Times are ms of the sample;
 * values are clamped natively, out-of-range or non-finite numbers never throw.
 */
export type SamplerSlotParams = {
  /** MIDI note that plays the sample at its own pitch; pads play this note (default 60). */
  rootNote: number;
  /** Key zone, 0–127 (default 0..127). `lowNote > highNote`: no key plays the slot (pads still do). */
  lowNote: number;
  highNote: number;
  /** Playback starts here (default 0). */
  startMs: number;
  /** Playback ends here; -1 = the sample's end (default). */
  endMs: number;
  /** Loop between two points of the sample, crossfaded over 5 ms; null = no loop (default). Gated slots only. */
  loop: SamplerLoop | null;
  /** true (default): plays to its end, ignoring note-offs and releases. false: plays while held. */
  oneShot: boolean;
  /** −60..24 dB (default 0). */
  gainDb: number;
  /** −1 (left) .. 1 (right), a balance (default 0). */
  pan: number;
  /** Semitones, −48..48 (default 0). */
  tune: number;
  /** Cents, −100..100 (default 0). */
  fineCents: number;
  /** Linear attack, 0–10000 ms (default 1). */
  attackMs: number;
  /** Linear release, 1–10000 ms (default 50). */
  releaseMs: number;
  /** 0 = every hit at full level, 1 = level follows velocity (default 1). */
  velocitySens: number;
  /** 0 = none (default), 1–8: a voice starting in a group fades the group's other voices out (5 ms). */
  chokeGroup: number;
};

/** Sampler-wide settings (0.10.0+). */
export type SamplerSettings = {
  /** 0–1 (default 0.8). */
  level: number;
  /** Silences the sampler (default false). */
  mute: boolean;
  /** Voices the sampler may use, 1–32 (default 32). */
  polyphony: number;
};

/** A slot in `getSamplerState()` (0.10.0+): its parameters (as stored) and what is loaded. */
export type SamplerSlotState = SamplerSlotParams & {
  loaded: boolean;
  /** Frames of the playable buffer (0 = empty). */
  frames: number;
  durationMs: number;
};

/** `getSamplerState()` (0.10.0+): the settings and all 16 slots. */
export type SamplerState = SamplerSettings & { slots: SamplerSlotState[] };

/** What `loadSamplerSlot` resolves with (0.10.0+): the playable buffer. */
export type SamplerLoadResult = {
  frames: number;
  /** Rate of the playable buffer: the engine's output rate when it was known, else the file's. */
  sampleRate: number;
  /** Of the file (1 = mono, played on both sides). */
  channels: number;
  durationMs: number;
};

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
  /** Sampler pad hits and key note-ons the engine consumed (cumulative since launch, 0.10.0+). */
  samplerHits?: number;
  /** Sampler voices sounding at the last block (0.10.0+). */
  samplerVoices?: number;
  /** Peak of the sampler part, before the effects (decaying hold, 0.10.0+). */
  samplerPeak?: number;
  /** Bytes of sample memory loaded, all slots (0.10.0+). */
  samplerBytes?: number;
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
   * iOS (0.9.0-rc.1+): since the last `setup()` (engine rebuilds keep counting), summed over every
   * device the engine opened (JUCE's count: gaps in the device's sample time plus callbacks that
   * overran their time budget).
   */
  xrunCount?: number;
  /** Android: the app is in the background and the audio device is closed (0.9.0+). */
  backgrounded?: boolean;
  /** Android: times the device was closed because the app went to the background (0.9.0+). */
  backgroundStops?: number;
  /** Android: times it was reopened when the app came back (0.9.0+). */
  foregroundRestarts?: number;

  // ── iOS, 0.9.0-rc.1+ (absent on older binaries and on Android) ──
  /**
   * The session's sample rate at setup, before the device was created (Hz): the route's own rate on
   * a fresh session, else the rate last preferred in this process.
   */
  hardwareSampleRate?: number;
  /**
   * The sample rate the engine asked the device for: the session's rate (`preferredSampleRate` is
   * only a hint). After a route change it changes only when the new route cannot run the old rate.
   */
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
  /** Touch velocity estimator: settings, accelerometer and the last estimates (iOS, 0.10.0-rc.3+). */
  touchVelocity?: TouchVelocityDiagnostics;
  /** Gesture recognizers and scroll views between the most recently attached pad and its window (at most 30). */
  touchPath?: TouchPathEntry[];
  /** The last pad touch (at most one report every 0.5 s): its delivery time, position and recognizers. */
  lastTouch?: LastTouchReport | Record<string, never>;
  /** Longest gap between main-thread display-link ticks in the last 10 s (ms); 0 when not set up. */
  mainThreadMaxGapMs?: number;
  /** Main-thread gaps longer than 50 ms since launch. */
  mainThreadStalls50?: number;
  // ── iOS, 0.9.0-rc.4+ ──
  /**
   * The last 32 main-thread gaps longer than 50 ms, oldest first: when each began (`atSec`, on the
   * `clockSec` clock) and how long it was.
   */
  mainThreadStallLog?: StallLogEntry[];
  /** Median time between the starts of consecutive stalls in `mainThreadStallLog` (ms); 0 with fewer than two. */
  mainThreadStallIntervalP50Ms?: number;
  /**
   * `CACurrentMediaTime()` when this snapshot was taken: seconds since boot, the clock of the stall
   * log's `atSec` (and of `UITouch.timestamp`).
   */
  clockSec?: number;
  /**
   * The main display link's measured rate (Hz): the median of its frame duration over its last 32
   * ticks; 0 while it does not run (no setup and no hint above 60, or the app inactive). Without a
   * hint above 60 the link is held to 60, so 60 here does not prove the display itself ran at 60.
   */
  displayHz?: number;
  /** The screen's `maximumFramesPerSecond` (120 on ProMotion); 0 until the link first ran. */
  maxFramesPerSecond?: number;
  /** The last `setDisplayRateHint` value (rounded, at most 240; 0 = none). */
  displayRateHint?: number;
  /** MIDI byte input (0.9.0-rc.2+, iOS): counters since launch and the arrival → render leg. */
  midiInput?: MidiInputDiagnostics;
  /** The native sequence player (0.9.0-rc.5+, iOS). */
  sequence?: SequenceDiagnostics;
  /** 0.9.0-rc.5+, iOS. */
  backgroundState?: BackgroundState;
  /** When the idle stop is due (clockSec seconds), -1 when none is pending. 0.9.0-rc.5+, iOS. */
  idleStopAt?: number;
  backgroundPolicy?: BackgroundPolicyDiagnostics;
  /** Idle stops since launch. */
  idleStops?: number;
  /** clockSec of the last idle stop, -1 = none. */
  lastIdleStopAt?: number;
  /** Why the last restart from the background failed (iOS refused the session); empty = none. */
  backgroundStartError?: string;
  nowPlaying?: NowPlayingDiagnostics;
};

// ── Loop sequence (0.9.0-rc.5, iOS) ──────────────────────────────

/**
 * What a sequence event plays:
 * - 'note': a synth note (through the arpeggiator when it is on), on at `start`, off after `length`;
 * - 'drum': a drum voice hit (one-shot);
 * - 'samplerHit' (0.10.0-rc.2): a sampler pad hit on slot `note` (0–15), released after
 *   `length` (a gated slot stops there, a one-shot slot plays on);
 * - 'samplerKey' (0.10.0-rc.2): a sampler key note `note` (0–127) on every slot whose key zone
 *   holds it, on at `start`, off after `length`.
 */
export type SequenceEventKind = 'note' | 'drum' | 'samplerHit' | 'samplerKey';

/** One event of the loop the native sequence player plays (setSequence). */
export type SequenceEvent = {
  /** Required here; an event without one (JS from before the kinds) plays as 'note'. */
  kind: SequenceEventKind;
  /** MIDI note 0–127, the drum voice 0–8, the sampler slot 0–15 (samplerHit) or key 0–127 (samplerKey). */
  note: number;
  /** 1–127. */
  velocity: number;
  /** Beats from the loop start, 0 <= start < loopBeats. */
  start: number;
  /**
   * Beats (> 0). Ignored for drums (a hit). For samplerHit: beats until the slot is released (a
   * one-shot slot plays on, but still needs a length > 0); for samplerKey: beats until key off. A
   * note or sampler event with length <= 0 is skipped (diagnostics `sequence.invalidEvents`).
   */
  length: number;
  /**
   * Absolute transport beat before which this event does not fire (it fires from its next
   * occurrence at or after it). For an event the app already played itself.
   */
  skipBefore?: number;
};

/**
 * Why setSequence refused a sequence (lastSequenceError / validateSequence). `index` is the event
 * that caused it (for a binary without sampler events: the first sampler event), or -1 when it is
 * not about one event (no native player, too many events, refused by the native player).
 */
export type SequenceError = {
  index: number;
  message: string;
};

export type SequenceOptions = {
  /** The loop length in beats: at least 1/64 when there are events (else setSequence returns false). */
  loopBeats: number;
  /** At most SEQUENCE_MAX_EVENTS. */
  events: SequenceEvent[];
  /** Silences the sequence without replacing it (setSequenceMuted). Default false. */
  muted?: boolean;
  /** Absolute transport beat: nothing fires before it (a count-in). Default: no bound. */
  fromBeat?: number;
  /** Absolute transport beat: nothing fires at or after it (a single pass). Default: no bound. */
  endBeat?: number;
};

/** One note the sequence player started or released (in an onSequenceNotes batch). */
export type SequenceNote = {
  /** The event's note: MIDI note, drum voice, sampler slot (samplerHit) or sampler key (samplerKey). */
  note: number;
  kind: SequenceEventKind;
  /**
   * True for the start; drums only report starts. Sampler events report each start's own release
   * (two overlapping hits on one slot give on, on, off, off), synth notes one off per pitch.
   */
  on: boolean;
  /** The transport beat it fired at. */
  atBeat: number;
};

/** An onSequenceNotes batch, about every 16 ms while the sequence plays and a listener is set. */
export type SequenceNotesBatch = {
  notes: SequenceNote[];
  /** CACurrentMediaTime() seconds at delivery. */
  now: number;
};

export type SequenceDiagnostics = {
  events: number;
  loopBeats: number;
  /** The player fires notes now (transport playing, events set, not muted). */
  playing: boolean;
  muted: boolean;
  /** Synth notes and sampler events the player holds now. */
  activeNotes: number;
  notesFired: number;
  /** Notes fired more than a block late (a block that started past the note). */
  lateFires: number;
  /**
   * Firings lost since launch (cumulative): starts over a block's limit (512), notes over 256
   * sounding at once, or loop passes over a block's limit. Never a note-off.
   */
  dropped: number;
  /** Invalid events the last accepted setSequence skipped (bad note, length or start). */
  invalidEvents: number;
  /**
   * The event that made the last refused setSequence refuse the whole sequence (unknown kind,
   * sampler slot or key out of range), -1 after one that was taken. 0.10.0-rc.2.
   */
  rejectedEvent?: number;
  /** Why (empty when none). 0.10.0-rc.2. */
  rejectReason?: string;
  version: number;
  /** onSequenceNotes batches are built (a listener is set). */
  listening: boolean;
  /** Notes not reported because the batch ring was full. */
  notesLost: number;
};

// ── Background playback, Now Playing, remote commands (0.9.0-rc.5, iOS) ──

export type BackgroundPolicy = {
  /**
   * true (default): in the background the engine keeps running while it plays something. false:
   * leaving the app stops the transport and releases every note.
   */
  keepAlive?: boolean;
  /**
   * Seconds idle in the background before the engine stops its audio device and lets iOS suspend
   * the app (default 30, 1–3600). Coming back to the foreground restarts it.
   */
  idleStopSeconds?: number;
};

export type BackgroundState = 'foreground' | 'background' | 'idleStopped';

export type NowPlayingInfo = {
  title: string;
  artist?: string;
  album?: string;
};

export type RemoteCommand = 'play' | 'pause' | 'togglePlayPause';

export type RemoteCommandEvent = {
  command: RemoteCommand;
  /** The transport's state after the command. */
  playing: boolean;
  /** A play that could not restart the audio device (see AudioDiagnostics.backgroundStartError). */
  failed?: boolean;
};

export type AudioInterruptionEvent = {
  type: 'began' | 'ended';
  /** began: the transport is stopped (it does not resume by itself). */
  playing?: boolean;
  /** began: the transport was playing when the interruption came. */
  wasPlaying?: boolean;
};

/** The native side started or stopped the transport (not a call from JS). */
export type TransportChangeEvent = {
  playing: boolean;
  reason: 'interruption' | 'remoteCommand' | 'background';
};

export type BackgroundStateEvent = {
  state: BackgroundState;
};

export type BackgroundPolicyDiagnostics = {
  /** setBackgroundPolicy was called since the app's JS started. */
  set: boolean;
  keepAlive: boolean;
  idleStopSeconds: number;
};

export type NowPlayingDiagnostics = {
  set: boolean;
  /** The session does not mix with other apps' audio now (needed for Now Playing). */
  exclusiveSession: boolean;
  remoteCommands: RemoteCommand[];
};

export type MidiInputDiagnostics = {
  /** The map is on (setMidiInputEnabled). */
  enabled: boolean;
  /** Maps set since launch. */
  mapVersion: number;
  /** Byte batches received (native calls and midiInputBytes). */
  packets: number;
  /** Complete MIDI messages parsed. */
  messages: number;
  /** Note-ons played by the map (synth and drums). */
  played: number;
  /** Note-ons passed to the app (map off, or the note unmapped). */
  passed: number;
  /** Note-ons the map would have played that the engine refused (not set up, queue full); passed to the app. */
  dropped: number;
  /** Raw-packet batches that were not complete MIDI (ignored whole). */
  rejectedPackets: number;
  /** Events lost because the ring was full. */
  eventsDropped: number;
  /** Input notes holding a synth note now. */
  held: number;
  /** releaseMidiInputHolds / allNotesOff count. */
  epoch: number;
  /** Bytes' arrival → the render block that consumed the note-on / hit (the same leg as `touchLatency.midiInputToRenderMs`). */
  midiInputToRenderMs: LatencyLegStats;
};

/** Byte formats of `midiInputBytes` and the native `expojuce_midi_input_bytes`. */
export type MidiInputFormat = 'bleMidi' | 'rawStream' | 'rawPackets';

/**
 * What an input note plays: a synth note (0–127), a drum voice (0–8), or nothing (`null`: the
 * note-on passes to the app, as every note does while the map is off).
 */
export type MidiInputAction = { note: number } | { drum: number } | null;

export type MidiInputMap = {
  /** Per input note 0–127 (index); missing entries pass. */
  actions: MidiInputAction[];
  /** Input velocity (index 0–127) → velocity played, 1–127. Omitted: the input's own. */
  velocity?: number[];
};

/**
 * One parsed message. `played`: handled natively (a note-on that sounded, a note-off that ended
 * the input's own hold); then `part` / `out` say what it played or released. Anything not played
 * is the app's to handle.
 */
export type MidiInputEvent = {
  type: 'noteOn' | 'noteOff' | 'polyPressure' | 'cc' | 'pitchBend';
  /** 0–15. */
  channel: number;
  /** noteOn / noteOff / polyPressure. */
  note?: number;
  /** noteOn / noteOff (0 for a note-on with velocity 0). */
  velocity?: number;
  pressure?: number;
  controller?: number;
  /** cc: 0–127; pitchBend: 0–16383 (8192 = centre). */
  value?: number;
  /** When the bytes arrived, seconds on the input clock (getMidiInputClock()). */
  arrival: number;
  /** The release epoch it was handled in (see releaseMidiInputHolds). */
  epoch: number;
  played: boolean;
  part?: 'synth' | 'drums';
  /** The synth note (or drum voice) played, or the synth note a note-off released. */
  out?: number;
  /** noteOn: the velocity played (after the curve). */
  outVelocity?: number;
  /** noteOff: the input's last hold of `out` ended. */
  last?: boolean;
  /** A note-off made natively: a second note-on of the same input note released the first. */
  synthetic?: boolean;
};

/** One onMidiInput batch: events since the previous one, oldest first. */
export type MidiInputBatch = {
  events: MidiInputEvent[];
  /** Input clock at delivery (seconds). */
  now: number;
  /** The current release epoch. */
  epoch: number;
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
  /**
   * The device was stopped and opened again. False when it already ran with the requested setup
   * (nothing was touched).
   */
  opened: boolean;
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
  /** MIDI byte input arrival → render block (0.9.0-rc.2+; see `midiInput`). */
  midiInputToRenderMs?: LatencyLegStats;
  /** Every pad touch and how many took over 100 ms / 1 s to be delivered (iOS, since launch). */
  slowTouches?: SlowTouchCounters;
  /** The last 8 pad touches delivered more than 100 ms late, oldest first (iOS). */
  slowTouchLog?: SlowTouchEntry[];
};

/** `TouchLatencyDiagnostics.slowTouches`: counted per touch (each finger), not per note. */
export type SlowTouchCounters = {
  touches: number;
  over100ms: number;
  over1s: number;
};

/**
 * One entry of `TouchLatencyDiagnostics.slowTouchLog`: a pad touch whose `touchesBegan` entry came
 * more than 100 ms after its `UITouch.timestamp`. Times are `CACurrentMediaTime()` seconds (the
 * clock of `clockSec`). Fields marked "after the fact" stay at -1 / '' / false until filled.
 */
export type SlowTouchEntry = {
  /** `touchesBegan` entry − `UITouch.timestamp` (ms). */
  deliveryMs: number;
  /** `UITouch.timestamp` as read at `touchesBegan` entry (s). */
  touchTimestamp: number;
  /** `touchesBegan` entry (s). */
  entryTimestamp: number;
  /** `UIEvent.timestamp` of the event that delivered it (s); 0 without an event. */
  eventTimestamp: number;
  /** `touchesBegan` entry − `UIEvent.timestamp` (ms); -1 without an event. */
  eventDeliveryMs: number;
  /** `UITouch.phase` at `touchesBegan` entry: 'began', 'moved', 'stationary', 'ended', 'cancelled' or 'other(n)'. */
  phase: string;
  tapCount: number;
  /** `UITouch.majorRadius` (points). */
  majorRadius: number;
  /** Distance to the nearest window edge (points); -1 without a window. */
  edgeDistance: number;
  /** Touches in this `touchesBegan` call. */
  touchesInCall: number;
  /** Touches the pad already held before this call. */
  heldBefore: number;
  /** Touches of this call the pad already held (delivered to `touchesBegan` twice). */
  alreadyHeld: number;
  /** `UIEvent.allTouches.count`: every touch of the event, on any view. */
  eventTouches: number;
  /** The pad's mode: 'notes', 'drums' or 'sampler'. */
  mode: string;
  /** Whether this call started a note or hit. */
  startedNote: boolean;
  /** `UITouch.gestureRecognizers` at entry (at most 12). */
  recognizers: SlowTouchRecognizer[];
  /** `UIApplication.applicationState` at entry: 'active', 'inactive', 'background' (or 'unknown'). */
  appState: string;
  /** Seconds from the last didBecomeActive / willResignActive to entry; -1 if none seen since the first pad was created. */
  sinceDidBecomeActiveSec: number;
  sinceWillResignActiveSec: number;
  /** A didBecomeActive or willResignActive fell between `touchTimestamp` and `entryTimestamp`. */
  activityChangedDuringDelivery: boolean;
  /** After the fact: 'ended' or 'cancelled' once a pad handled the touch's end; '' until then (or if no pad saw it). */
  endPhase: string;
  /** After the fact: `touchesBegan` entry → the pad handling the touch's end (ms). */
  endedAfterMs: number;
  /** After the fact: `endedAfterMs` ≤ 50, i.e. began and end were delivered together. */
  endedWithin50ms: boolean;
  /** After the fact: `UITouch.timestamp` at the end − `touchTimestamp` (ms): how long the finger was down. */
  heldMs: number;
  /** After the fact: the end's own delivery, handled − `UITouch.timestamp` at the end (ms). */
  endDeliveryMs: number;
  /**
   * After the fact (0.2–0.5 s after entry): the longest main-thread stall (display-link gap over 50 ms)
   * overlapping `touchTimestamp` … `entryTimestamp` (ms); 0 if none; -1 if the stall probe was not running.
   */
  deliveryGapMs: number;
  /** As `deliveryGapMs`, over `touchTimestamp` … the end being handled; -1 until the end is seen (and for 0.2–0.5 s after it). */
  lifetimeGapMs: number;
};

/** A recognizer the slow touch was delivered to. */
export type SlowTouchRecognizer = {
  class: string;
  /** 'possible', 'began', 'changed', 'ended', 'cancelled', 'failed' (or 'unknown(n)'). */
  state: string;
  delaysTouchesBegan: boolean;
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

/** One entry of `AudioDiagnostics.mainThreadStallLog`. */
export type StallLogEntry = {
  /** When the gap began (the display-link tick before it), `CACurrentMediaTime()` seconds. */
  atSec: number;
  /** Its length (ms), over 50. */
  ms: number;
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
