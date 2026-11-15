// Touch velocity helpers that need no native code (pure, no runtime imports, so they also run in
// plain Node: test/touch-velocity.test.mjs).
import type {
  TouchSignals,
  TouchVelocityCalibration,
  TouchVelocityConfig,
  TouchVelocityCurve,
} from './ExpoJuce.types';

/** The estimator's defaults (ios/TouchVelocity.h). The calibration ends are first guesses. */
export const TOUCH_VELOCITY_DEFAULTS: {
  curve: TouchVelocityCurve;
  minVelocity: number;
  maxVelocity: number;
  calibration: TouchVelocityCalibration;
} = {
  curve: 'normal',
  minVelocity: 20,
  maxVelocity: 127,
  calibration: {
    radiusSoft: 20,
    radiusHard: 34,
    impactSoft: 0.02,
    impactHard: 0.25,
    radiusWeight: 0.4,
    impactWeight: 0.6,
  },
};

/** A signal separates when its hard median is at least this much above its soft median. */
export const TOUCH_VELOCITY_MIN_SEPARATION = 1.15;

const isFiniteNumber = (v: unknown): v is number => typeof v === 'number' && Number.isFinite(v);
const isPositive = (v: unknown): v is number => isFiniteNumber(v) && v > 0;

function median(values: number[]): number | null {
  if (values.length === 0) return null;
  const sorted = values.slice().sort((a, b) => a - b);
  const mid = sorted.length >> 1;
  return sorted.length % 2 === 1 ? sorted[mid] : (sorted[mid - 1] + sorted[mid]) / 2;
}

// Soft and hard medians of one signal, or null when it does not separate.
function ends(soft: number[], hard: number[]): { soft: number; hard: number } | null {
  const s = median(soft);
  const h = median(hard);
  // (The tolerance keeps exactly 15 % apart separating despite rounding.)
  if (s === null || h === null || !(s > 0) || h < s * TOUCH_VELOCITY_MIN_SEPARATION * (1 - 1e-9)) return null;
  return { soft: s, hard: h };
}

/**
 * Calibration from a "tap softly, then tap hard" session: the `touch` signals of each tap's
 * `onNoteOn`. Each signal's soft and hard medians become its ends; a signal whose hard median is
 * less than 15 % above its soft one gets weight 0 (its ends stay at the defaults). Taps without an
 * impact (null) are left out of the impact signal. null when neither signal separates (or a set is
 * empty): keep the current calibration.
 */
export function calibrateTouchVelocity(
  soft: TouchSignals[],
  hard: TouchSignals[]
): TouchVelocityCalibration | null {
  if (!Array.isArray(soft) || !Array.isArray(hard)) return null;
  const radii = (set: TouchSignals[]) => set.map((t) => t?.radius).filter(isPositive);
  const impacts = (set: TouchSignals[]) => set.map((t) => t?.impact).filter(isPositive);
  const radius = ends(radii(soft), radii(hard));
  const impact = ends(impacts(soft), impacts(hard));
  if (radius === null && impact === null) return null;
  const d = TOUCH_VELOCITY_DEFAULTS.calibration;
  return {
    radiusSoft: radius ? radius.soft : d.radiusSoft,
    radiusHard: radius ? radius.hard : d.radiusHard,
    impactSoft: impact ? impact.soft : d.impactSoft,
    impactHard: impact ? impact.hard : d.impactHard,
    radiusWeight: radius ? d.radiusWeight : 0,
    impactWeight: impact ? d.impactWeight : 0,
  };
}

const CALIBRATION_KEYS = [
  'radiusSoft',
  'radiusHard',
  'impactSoft',
  'impactHard',
  'radiusWeight',
  'impactWeight',
] as const;

/**
 * The config as sent to native code: known keys only, finite numbers, no nulls (native validates
 * and clamps the values; an omitted field takes its default).
 */
export function touchVelocityConfigForNative(config: TouchVelocityConfig): Record<string, unknown> {
  const out: Record<string, unknown> = { enabled: config?.enabled === true };
  if (config?.curve === 'soft' || config?.curve === 'normal' || config?.curve === 'hard') out.curve = config.curve;
  if (isFiniteNumber(config?.minVelocity)) out.minVelocity = config.minVelocity;
  if (isFiniteNumber(config?.maxVelocity)) out.maxVelocity = config.maxVelocity;
  const c = config?.calibration;
  if (c != null && typeof c === 'object') {
    const calibration: Record<string, number> = {};
    for (const key of CALIBRATION_KEYS) {
      const value = c[key];
      if (isFiniteNumber(value)) calibration[key] = value;
    }
    out.calibration = calibration;
  }
  return out;
}
