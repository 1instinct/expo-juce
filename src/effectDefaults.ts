import type { EffectId, EffectsState } from './ExpoJuce.types';

// Default effect settings, same as the native table (ios/FxChainProcessor.h, kFxSpecs). Used
// where there is no native chain: the web stub and binaries older than 0.6.0.
const DEFAULT_PARAMS: Record<EffectId, Record<string, number>> = {
  drive: { amount: 0.4, tone: 0.6, mix: 1 },
  crush: { bits: 8, downsample: 4, noise: 0.1, mix: 1 },
  eq: { low: 0, mid: 0, midFreq: 1000, high: 0 },
  chorus: { rate: 0.8, depth: 0.4, mix: 0.5 },
  phaser: { rate: 0.4, depth: 0.6, feedback: 0.5, mix: 0.5 },
  echo: { time: 0.375, sync: 1, division: 2, feedback: 0.4, tone: 0.5, pingPong: 0, mix: 0.35 },
  reverb: { size: 0.6, damping: 0.5, width: 1, mix: 0.3 },
  pump: { depth: 0.6, shape: 0.5, division: 0 },
  compressor: { threshold: -18, ratio: 4, attack: 10, release: 120, makeup: 4 },
};

/** Every effect disabled, at its default parameters. */
export function defaultEffectsState(): EffectsState {
  const state = {} as EffectsState;
  for (const id of Object.keys(DEFAULT_PARAMS) as EffectId[]) {
    state[id] = { enabled: false, params: { ...DEFAULT_PARAMS[id] } };
  }
  return state;
}
