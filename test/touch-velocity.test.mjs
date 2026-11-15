// calibrateTouchVelocity and touchVelocityConfigForNative (src/touchVelocity.ts), run on the built
// output: `npx tsc -p .` (or `npm run prepare`), then `node test/touch-velocity.test.mjs`.
import assert from 'node:assert/strict';
import { test } from 'node:test';

import {
  TOUCH_VELOCITY_DEFAULTS,
  calibrateTouchVelocity,
  touchVelocityConfigForNative,
} from '../build/touchVelocity.js';

const tap = (radius, impact, extra = {}) => ({
  radius,
  radiusTolerance: 3,
  force: 0,
  maxForce: 0,
  impact,
  accelAgeMs: impact === null ? null : 4,
  strength: 0.5,
  ...extra,
});
const D = TOUCH_VELOCITY_DEFAULTS.calibration;

test('both signals separate: medians become the ends, default weights', () => {
  const soft = [tap(20, 0.03), tap(22, 0.05), tap(21, 0.04), tap(40, 0.02)];
  const hard = [tap(30, 0.3), tap(28, 0.2), tap(32, 0.4), tap(29, 0.25)];
  assert.deepEqual(calibrateTouchVelocity(soft, hard), {
    radiusSoft: 21.5, // median of 20, 21, 22, 40
    radiusHard: 29.5,
    impactSoft: 0.035,
    impactHard: 0.275,
    radiusWeight: D.radiusWeight,
    impactWeight: D.impactWeight,
  });
});

test('odd counts use the middle value', () => {
  const c = calibrateTouchVelocity([tap(18, 0.01), tap(20, 0.02), tap(25, 0.03)], [tap(30, 0.1), tap(31, 0.2), tap(50, 0.3)]);
  assert.equal(c.radiusSoft, 20);
  assert.equal(c.radiusHard, 31);
  assert.equal(c.impactSoft, 0.02);
  assert.equal(c.impactHard, 0.2);
});

test('radius under 15 % apart: radius weight 0, its ends stay at the defaults', () => {
  const soft = [tap(20, 0.02), tap(20, 0.03)];
  const hard = [tap(22.9, 0.3), tap(22.9, 0.2)]; // 14.5 % above
  const c = calibrateTouchVelocity(soft, hard);
  assert.equal(c.radiusWeight, 0);
  assert.equal(c.radiusSoft, D.radiusSoft);
  assert.equal(c.radiusHard, D.radiusHard);
  assert.equal(c.impactWeight, D.impactWeight);
  assert.equal(c.impactSoft, 0.025);
  assert.equal(c.impactHard, 0.25);
});

test('exactly 15 % apart separates', () => {
  const c = calibrateTouchVelocity([tap(20, null)], [tap(23, null)]);
  assert.equal(c.radiusWeight, D.radiusWeight);
  assert.equal(c.radiusHard, 23);
});

test('impact not separating, or hard below soft: impact weight 0', () => {
  const c = calibrateTouchVelocity([tap(20, 0.2)], [tap(30, 0.1)]);
  assert.equal(c.impactWeight, 0);
  assert.equal(c.impactSoft, D.impactSoft);
  assert.equal(c.impactHard, D.impactHard);
  assert.equal(c.radiusWeight, D.radiusWeight);
});

test('null impacts are left out of the impact signal only', () => {
  const soft = [tap(20, null), tap(20, 0.02), tap(20, null)];
  const hard = [tap(30, 0.2), tap(30, null), tap(30, null)];
  const c = calibrateTouchVelocity(soft, hard);
  assert.equal(c.impactSoft, 0.02);
  assert.equal(c.impactHard, 0.2);
  assert.equal(c.radiusSoft, 20);
  assert.equal(c.radiusHard, 30);
});

test('every impact null: impact weight 0, radius alone', () => {
  const c = calibrateTouchVelocity([tap(20, null), tap(21, null)], [tap(30, null), tap(31, null)]);
  assert.equal(c.impactWeight, 0);
  assert.equal(c.radiusWeight, D.radiusWeight);
});

test('neither separates: null', () => {
  assert.equal(calibrateTouchVelocity([tap(25, 0.1)], [tap(25, 0.1)]), null);
  assert.equal(calibrateTouchVelocity([tap(25, null)], [tap(24, null)]), null);
});

test('empty or invalid sets: null', () => {
  assert.equal(calibrateTouchVelocity([], [tap(30, 0.3)]), null);
  assert.equal(calibrateTouchVelocity([tap(20, 0.02)], []), null);
  assert.equal(calibrateTouchVelocity(null, null), null);
  assert.equal(calibrateTouchVelocity([tap(NaN, NaN)], [tap(Infinity, -1)]), null);
});

test('non-finite and non-positive values are ignored', () => {
  const soft = [tap(20, 0.02), tap(NaN, 0), tap(-5, -0.1)];
  const hard = [tap(30, 0.3), tap(Infinity, NaN)];
  const c = calibrateTouchVelocity(soft, hard);
  assert.equal(c.radiusSoft, 20);
  assert.equal(c.radiusHard, 30);
  assert.equal(c.impactSoft, 0.02);
  assert.equal(c.impactHard, 0.3);
});

test('inputs are not mutated', () => {
  const soft = [tap(25, 0.05), tap(20, 0.02)];
  const hard = [tap(35, 0.4), tap(30, 0.3)];
  const before = JSON.stringify([soft, hard]);
  calibrateTouchVelocity(soft, hard);
  assert.equal(JSON.stringify([soft, hard]), before);
});

test('config for native: known keys, finite numbers, no nulls', () => {
  assert.deepEqual(touchVelocityConfigForNative({ enabled: true }), { enabled: true });
  assert.deepEqual(touchVelocityConfigForNative({ enabled: false, calibration: null }), { enabled: false });
  assert.deepEqual(
    touchVelocityConfigForNative({
      enabled: true,
      curve: 'hard',
      minVelocity: 30,
      maxVelocity: NaN,
      calibration: { ...D, radiusSoft: Infinity },
      extra: 1,
    }),
    {
      enabled: true,
      curve: 'hard',
      minVelocity: 30,
      calibration: {
        radiusHard: D.radiusHard,
        impactSoft: D.impactSoft,
        impactHard: D.impactHard,
        radiusWeight: D.radiusWeight,
        impactWeight: D.impactWeight,
      },
    }
  );
  assert.deepEqual(touchVelocityConfigForNative({ enabled: 'yes', curve: 'loud' }), { enabled: false });
});

test('defaults match the native estimator', () => {
  assert.deepEqual(TOUCH_VELOCITY_DEFAULTS, {
    curve: 'normal',
    minVelocity: 20,
    maxVelocity: 127,
    calibration: { radiusSoft: 20, radiusHard: 34, impactSoft: 0.02, impactHard: 0.25, radiusWeight: 0.4, impactWeight: 0.6 },
  });
});
