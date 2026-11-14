import type { SamplerSettings, SamplerSlotParams, SamplerState } from './ExpoJuce.types';

/** Sampler slots (0.10.0+): 0–15. */
export const SAMPLER_SLOT_COUNT = 16;

/** Sampler voices shared by all slots (0.10.0+); `polyphony` limits it further. */
export const SAMPLER_VOICE_COUNT = 32;

/**
 * Sampler defaults (0.10.0+): what an empty slot / `getSamplerState()` reports on a fresh engine,
 * on web and on binaries without a sampler. Same values as `SamplerSlotParams` /
 * `SamplerSettings` natively (the harness checks this file against them).
 */
export const SAMPLER_SLOT_DEFAULTS: Readonly<SamplerSlotParams> = Object.freeze({ rootNote: 60, lowNote: 0, highNote: 127, startMs: 0, endMs: -1, loop: null, oneShot: true, gainDb: 0, pan: 0, tune: 0, fineCents: 0, attackMs: 1, releaseMs: 50, velocitySens: 1, chokeGroup: 0 });
export const SAMPLER_DEFAULTS: Readonly<SamplerSettings> = Object.freeze({ level: 0.8, mute: false, polyphony: 32 });

/** A fresh, mutable sampler state at the defaults (every slot empty). */
export function samplerDefaults(): SamplerState {
  return {
    ...SAMPLER_DEFAULTS,
    slots: Array.from({ length: SAMPLER_SLOT_COUNT }, () => ({
      ...SAMPLER_SLOT_DEFAULTS,
      loaded: false,
      frames: 0,
      durationMs: 0,
    })),
  };
}
