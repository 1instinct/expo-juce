# Touch velocity (iOS, 0.10.0-rc.3+)

`JucePadView` with `touchVelocity` plays each note or hit at a velocity estimated from the touch-down
itself. This page describes what the phone can measure, how the estimate is made, how to calibrate
it and what the diagnostics report.

## Signals

| Signal | Source | Notes |
|---|---|---|
| Contact size | `UITouch.majorRadius`, `majorRadiusTolerance` (pt) | Quantised in steps of a few points. A firmer strike flattens the finger more, but the first frame often comes before full flattening, and finger size and angle dominate: weak alone, needs calibration. |
| Device impact | CoreMotion accelerometer at 100 Hz (the practical iPhone ceiling) | A firm tap jolts the phone. Strong on a table, weaker hand-held. |
| Force | `UITouch.force / maximumPossibleForce` | Only where `maximumPossibleForce > 0` (3D Touch devices, Apple Pencil). Used alone when above 5 % of the maximum on the touch-down frame. |

`CMAccelerometerData.timestamp`, `UITouch.timestamp` and `CACurrentMediaTime()` share the boot clock.

## Estimator (`ios/TouchVelocity.h`)

Pure C++, tested on the host by the offline harness (cases `tv`, `tvd`).

- **Accelerometer ring:** 256 samples (2.56 s at 100 Hz), single producer (the motion queue,
  serial), read on the main thread at touch-down and by diagnostics. Every slot carries a sequence
  number; a reader drops a slot that the writer overwrote while it was being copied. No locks, no
  allocation.
- **Impact** for a touch at `t`: baseline = per-axis median of the samples in
  `[t − 150 ms, t − 30 ms]` (removes gravity and slow hand motion; the median keeps the previous
  tap's spike in a fast passage from shifting it), at least 3 samples; impact = the largest
  `|a − baseline|` (g) over `[t − 20 ms, now]`, where `now` is the touch handler's entry. Invalid
  when the newest sample is more than 100 ms old (accelerometer off or stalled), the baseline has
  fewer than 3 samples (e.g. right after the accelerometer started) or the impact window is empty.
- **Strength 0–1:** force when valid, alone: `force / maxForce`. Force counts only above 5 % of
  `maxForce`: force hardware often reports 0 or a tiny value on the touch-down frame, and such a
  touch uses the blend instead. Otherwise the weighted mean of
  `norm(radius; radiusSoft, radiusHard)` (linear) and `norm(impact; impactSoft, impactHard)` (log
  scale), weights `radiusWeight` / `impactWeight`. An invalid impact (or radius) drops its term;
  with no usable term the strength is 0.5. Of several touches landing in one call the strongest
  plays.
- **Velocity:** `round(min + (max − min) · strength^γ)` with γ 0.6 / 1.0 / 1.6 for
  `soft` / `normal` / `hard`, clamped to 1–127.

| Default | Value |
|---|---|
| `curve` | `normal` |
| `minVelocity` / `maxVelocity` | 20 / 127 (a soft tap stays audible) |
| `radiusSoft` → `radiusHard` | 20 → 34 pt |
| `impactSoft` → `impactHard` | 0.02 → 0.25 g |
| `radiusWeight` / `impactWeight` | 0.4 / 0.6 |
| accelerometer | 100 Hz requested |

The uncalibrated ends are first guesses; a calibration on the device replaces them.

**Latency: none added.** The note fires in `touchesBegan` as without touch velocity, with what is
known then: the radius of the first frame and the accelerometer samples up to the handler's entry
(touches usually arrive ~12 ms after their timestamp, so the ring already holds the sample at or
just after contact). The cost is one scan of a few dozen ring samples. Waiting for more samples is
not offered: at 100 Hz each one costs 10 ms of note delay. To show what waiting would have bought,
the diagnostics report `impactLate`, the peak in the 50 ms after the touch, worked out after the
note (shortly after each touch on the main thread, and when diagnostics are read).

**Accelerometer lifecycle:** runs only while `setTouchVelocity({ enabled: true })` is set and at
least one pad with `touchVelocity` is in a window; stops otherwise and while the app is in the
background, and restarts in the foreground. The raw accelerometer needs no Info.plist entry
(`NSMotionUsageDescription` covers motion activity and the pedometer). Without an accelerometer
(simulator) the impact is invalid and the radius plays alone.

## API

```ts
type TouchVelocityCurve = 'soft' | 'normal' | 'hard';
type TouchVelocityCalibration = { radiusSoft: number; radiusHard: number; impactSoft: number;
  impactHard: number; radiusWeight: number; impactWeight: number };
type TouchVelocityConfig = { enabled: boolean; curve?: TouchVelocityCurve; minVelocity?: number;
  maxVelocity?: number; calibration?: TouchVelocityCalibration | null };
type TouchSignals = { radius: number; radiusTolerance: number; force: number; maxForce: number;
  impact: number | null; accelAgeMs: number | null; strength: number };

hasTouchVelocity(): boolean
setTouchVelocity(config: TouchVelocityConfig): boolean
calibrateTouchVelocity(soft: TouchSignals[], hard: TouchSignals[]): TouchVelocityCalibration | null
TOUCH_VELOCITY_DEFAULTS
// <JucePadView touchVelocity /> (default false: the fixed `velocity` prop)
// onNoteOn: { note, velocity, touch?: TouchSignals } (touch only when the velocity was estimated)
```

`setTouchVelocity` replaces the whole configuration. Natively: velocities are rounded and clamped
to 1–127 (min and max swapped when reversed), radii to 1–200 pt, impacts to 0.001–16 g, weights to
0–1, non-finite fields take their defaults, an unknown curve is `normal`, and a signal whose hard
end is not above its soft end gets weight 0. `calibration: null` (or omitted) = the defaults.

## Calibration

Ask for a few soft taps and a few hard taps on a pad with `touchVelocity` (and touch velocity
enabled) and keep each `onNoteOn` `touch`. `calibrateTouchVelocity(soft, hard)` takes the medians
of each set: the soft and hard medians become each signal's ends, and a signal whose hard median is
less than 15 % above its soft one gets weight 0 (its ends stay at the defaults). Taps without an
impact (null) are left out of the impact signal. It returns null when neither signal separates (or
a set is empty); keep the previous calibration then.

## Diagnostics

`getDiagnostics().touchVelocity`:

| Key | Meaning |
|---|---|
| `supported` | The binary has the estimator. |
| `enabled`, `curve`, `min`, `max` | The configuration in effect. |
| `calibration` | The calibration as set (sanitised), or null for the defaults. |
| `accel.running` / `available` | Accelerometer updates on / the device has one (known once started). |
| `accel.requestedHz` / `measuredHz` | 100 / samples per second over the last second. |
| `accel.samples` | Samples received since launch. |
| `forceCapable` | A pad's trait collection reports 3D Touch, or a touch carried force. |
| `pads` | Pads with `touchVelocity` in a window. |
| `clockSec` | `CACurrentMediaTime()` at the read. |
| `last` | The last estimate, or null. |
| `recent` | The last 16 estimates, oldest first. |

`accelAgeMs` is signed: the touch handler's entry minus the newest accelerometer sample's
timestamp. It is normally a few ms; a negative value means a sample was stamped after the handler
started, i.e. the accelerometer and touch clocks disagree (the impact windows would then be off).

Each estimate: the `TouchSignals` fields plus `velocity`, `forceUsed`, `impactLate` (g, null until
known or when the samples are gone) and `atSec` (the touch's timestamp, on the `clockSec` clock).
