# Drums: sound design and realtime notes

The drum part (0.8.0+) is `ios/DrumMachine.h`: pure C++, header-only, no JUCE, no samples. It is
owned by `PolySynthProcessor`, which drains drum hits from the note queue at the start of each
block and adds the kit to its mono output after the synth voices, so the effects chain processes
synth and drums together. The API is in the README ([Drums](../README.md#drums-080)).

## Signal flow

```
 drumHit / JucePadView (drums) ─► NoteEventQueue (kind Drum) ─► PolySynthProcessor::processBlock
                                                                   └─ DrumMachine::trigger(voice, vel)
 voices 0–8 ─► Σ (each × level × velocity / 127) ─► drive ─► × kit level (ramped per block) ─► + synth ─► effects
```

- Envelopes are one-pole exponentials. A decay time `T` is the time to −60 dB; `decay` scales
  every `T` of a voice by `2^((decay − 0.5) × 4)` (×0.25 … ×4). Pitch, click and burst envelopes
  are fixed, so `decay` changes the length without smearing the attack.
- `tune` multiplies every frequency of the voice (oscillators and filter centres) by
  `2^(tune / 12)`; filter cutoffs are clamped below 0.45 × the sample rate.
- A voice stops when every envelope it plays is below −80 dB; from then on it outputs exact
  zeros and costs nothing. With no voice sounding the kit does not touch the buffer at all, so
  a synth-only output is bit-identical to 0.7.
- Retrigger: a hit on a sounding voice restarts it from its initial state and adds the previous
  sound's last sample, fading linearly to 0 over 64 samples (no click, no overlap).
- Choke: a closed-hat hit switches a sounding open hat's envelope to a 12 ms decay.
- Drive: `x' = (1 − d) · x + d · tanh(k · x) / √k` with `k = 1 + 7d`. At 0 the kit is untouched;
  towards 1 quiet hits get louder and peaks round off. The safety limiter after the effects still
  catches the sum.
- Noise is a 32-bit xorshift (one generator for the kit); filters are trapezoidal state-variable
  filters (Simper), recomputed once per block per sounding voice.

## Voices

| # | Voice | Built from | `tone` |
|---|---|---|---|
| 0 | Kick | sine at `52 Hz × (1 + 3p)`, `p` falling from 1 with a 0.08 s T60 (≈ 4× → 1× in 40 ms); noise click (12 ms); amp 0.6 s | click level 0.1–0.6 |
| 1 | Snare | 185 + 330 Hz sines with a short +40 % bend (body, 0.2 s) + noise through a band-pass at 2.5–7 kHz (snares, 0.3 s) | body 0.8 → 0.4, noise 0.5 → 1.4, band moves up |
| 2 | Closed hat | six naive squares at 205.3, 304.4, 369.6, 522.7, 540, 800 Hz (the 808 set) + noise → band-pass 7–11 kHz → high-pass 5–8 kHz; 0.09 s | less metal, more noise, both filters up |
| 3 | Open hat | same source and filters, 0.7 s; choked by 2 | same |
| 4 | Clap | noise → band-pass 0.9–2.5 kHz (Q 1.4); bursts at 0, 10 and 20 ms (each a fresh 30 ms T60), then a tail from 30 ms (0.35 s) | band centre |
| 5 | Low tom | sine at `95 Hz × (1 + 0.5p)`, `p` 0.25 s T60; amp 0.5 s; noise attack (15 ms) | attack noise 0–0.35 |
| 6 | High tom | same at 150 Hz, amp 0.4 s | same |
| 7 | Rim | noise → band-pass 3.5 kHz (Q 2), 8 ms click + 1660 / 480 Hz sine ping, 0.07 s | click 1 → 3, ping 0.7 → 0.35 |
| 8 | Mid tom (0.8.0-rc.2+) | same at 119.4 Hz (√(95 × 150), centred between the toms on a log scale), amp 0.45 s | same |

Starting points for kits (all values as `setDrumVoice` takes them):

- 808-like: kick `decay` 0.8, `tone` 0.2; hats `tone` 0.3; snare `tone` 0.4.
- 909-like: kick `decay` 0.35, `tone` 0.7, `tune` +2; snare `tone` 0.8; hats `tone` 0.8.
- Dark / lo-fi: every `tone` 0.2, kit `drive` 0.5, then the `crush` effect.
- Electro: toms (5, 8, 6) `tune` +7 … +12, `decay` 0.3; clap `tone` 0.7.

## Realtime rules

- Everything is preallocated in the processor: 9 voice states, a 256-sample scratch buffer,
  the parameter store. `trigger` and `render` do not allocate, lock or call into the OS.
- Parameters are relaxed atomics (one per value) read once per block; the engine keeps one
  `DrumParamStore` for the life of the process, so settings work before `setup()` and survive
  rebuilds. Hits travel through the note queue's ring with the note-ons (a full ring drops a hit
  and counts it in `drumHitsDropped`).
- Cost: with all 9 voices sounding and drive on, about 18 µs per 128-sample block at 48 kHz on a
  desktop x86 core (harness case `drb`, 0.006 × realtime); a phone core is a few times slower,
  still well under 1 % of the 2.7 ms block.

## Verified where

The harness cases `dr`, `drs`, `drb`, `drm`, `drd` ([test/harness/README.md](../test/harness/README.md))
cover the DSP, the queue path and the defaults. The Objective-C++ / Swift glue and the pad's drums
mode are compiled first by the iOS build.
