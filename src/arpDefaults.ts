import type { ArpOptions } from './ExpoJuce.types';

/**
 * Arpeggiator defaults (0.7.0+): what `getArpeggiatorState()` returns on a fresh engine, on web
 * and on binaries without an arpeggiator. Same values as `ArpSettings::defaults()` natively.
 */
export const ARP_DEFAULTS: Readonly<ArpOptions> = Object.freeze({
  enabled: false,
  mode: 'up',
  rate: '1/16',
  octaves: 1,
  gate: 0.6,
  swing: 0,
  latch: false,
  pattern: Object.freeze(new Array<boolean>(16).fill(true)) as boolean[],
  accent: 0,
  ratchet: 1,
  transposeSteps: 0,
  transposeDistance: 0,
});

/** A fresh, mutable copy of `ARP_DEFAULTS`. */
export function arpDefaults(): ArpOptions {
  return { ...ARP_DEFAULTS, pattern: [...ARP_DEFAULTS.pattern] };
}
