import type { DrumKitParams, DrumState, DrumVoiceParams } from './ExpoJuce.types';

/** Number of drum voices (0.8.0+): kick, snare, closed hat, open hat, clap, low tom, high tom, rim. */
export const DRUM_VOICE_COUNT = 8;

/**
 * Drum defaults (0.8.0+): what `getDrumState()` returns on a fresh engine, on web and on binaries
 * without drums. Same values as `kDrumVoiceDefaults` / `kDrumKitDefaults` natively.
 */
export const DRUM_VOICE_DEFAULTS: Readonly<DrumVoiceParams> = Object.freeze({ tune: 0, decay: 0.5, tone: 0.5, level: 0.8 });
export const DRUM_KIT_DEFAULTS: Readonly<DrumKitParams> = Object.freeze({ level: 0.8, mute: false, drive: 0 });

/** A fresh, mutable drum state at the defaults. */
export function drumDefaults(): DrumState {
  return {
    kit: { ...DRUM_KIT_DEFAULTS },
    voices: Array.from({ length: DRUM_VOICE_COUNT }, () => ({ ...DRUM_VOICE_DEFAULTS })),
  };
}
