# Pre-Ringing Compensation

This note captures a pragmatic way to treat predicted group-delay hot spots as low-trust regions for mixed-phase correction.

The goal is not to "fix" every visible delay peak. The goal is to keep mixed-phase filters useful in the low end without creating obvious pre-ringing or long front-tail energy in the realized FIR.

## Core Idea

Predicted group-delay peaks are often the places where an inverse mixed-phase filter becomes most aggressive.

If a narrow or strong excess-delay region is inverted too literally, the filter tends to pull energy in front of the main impulse. That shows up as pre-ringing.

So a group-delay peak should usually be treated as:

- a warning sign
- a low-trust region
- a place to reduce phase correction, not increase it

This is especially important when the delay peak is narrow, high-Q, or clearly tied to a room resonance or cancellation pattern.

## Interpretation Of Excess Group Delay

Absolute milliseconds are less useful than excess delay expressed as a fraction of the local period.

Use:

`excess_cycles = excess_gd_ms * frequency_hz / 1000`

Practical interpretation:

- below `0.25` cycle: usually benign
- `0.25` to `0.5` cycle: meaningful structure, often still safe
- `0.5` to `1.0` cycle: caution zone
- above `1.0` cycle: hot spot, usually not worth fully inverting
- above `2.0` cycles: strongly suspect resonance/null-driven behavior; aggressive correction is risky

Examples:

- at `50 Hz`, `1` cycle is `20 ms`
- at `100 Hz`, `1` cycle is `10 ms`
- at `200 Hz`, `1` cycle is `5 ms`
- at `1 kHz`, `1` cycle is `1 ms`

This is a better control signal than a fixed "good ms" threshold because audibility and inversion cost are frequency dependent.

## Manual Peak Marking

A useful workflow is to let the user manually identify predicted group-delay peaks that look unsafe for literal inversion.

Those marked regions should drive a pre-ringing compensation step that locally reduces mixed-phase correction.

The important design rule is:

- the mark does not mean "correct harder"
- the mark means "trust the inverse less here"

## Recommended Compensation Behavior

Model each user mark as a local attenuation on the phase-correction target.

Each marked region should define:

- center frequency
- width
- strength

Inside that band:

- reduce allowed excess-phase inversion
- taper smoothly in and out
- keep magnitude correction independent unless the same region is also unstable in magnitude

This is better than trying to patch the FIR afterward. It keeps the behavior predictable because the restraint is applied at the phase-target stage.

## Practical Heuristic

A good starting policy is:

- apply full phase correction up to about `0.5` cycle of excess group delay
- taper phase correction between `0.5` and `1.0` cycle
- apply minimal or no phase correction above `1.0` cycle

For manually marked regions, the local weighting can be more conservative than the global rule.

This combines well with existing low-frequency limits:

- allow more correction in the bass
- become progressively more conservative as frequency rises
- strongly limit narrow high-Q peaks even in the low end

## What To Avoid

- hard disabling correction with abrupt band edges
- keying compensation from raw group delay without removing the smooth trend
- treating high-frequency and low-frequency pre-ringing risk as equally audible
- using group-delay peaks as a reason to chase narrow room nulls harder

In practice, excess group delay over a smooth baseline is more useful than raw group delay.

## Short Version

Predicted group-delay peaks are good candidates for manual pre-ringing protection.

If the user marks those peaks, Wolfie should reduce mixed-phase correction locally and smoothly in those bands. A practical threshold is:

- full correction below `0.5` cycle
- tapered correction from `0.5` to `1.0` cycle
- little or no correction above `1.0` cycle

That keeps mixed-phase correction focused on broad, low-frequency timing improvement instead of forcing unstable local inversions.
