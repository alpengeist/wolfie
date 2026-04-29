# Excess Phase Reduction Plan

## Goal

Add a low-frequency excess-phase reduction mode to `src/measurement/filter_designer.cpp` with deterministic validation feedback before and during implementation.

This should stay inside the measurement module. The UI can surface the mode later, but the math and the predicted diagnostics belong in filter design.

## Current State

As of 2026-04-28 after the latest implementation slice:

- `FilterDesignSettings::phaseMode` now normalizes to either:
  - `"minimum"`
  - `"excess-lf"`
  - `"mixed"`
- `filter_designer` already derives:
  - input excess phase
  - predicted excess phase
  - filter group delay
  - predicted total group delay
- phase diagnostics already consume `measurement.raw_phase_spectrum` from `MeasurementResult` when available.
- bulk delay removal currently depends on `measurement.raw_impulse_response`.
- `"minimum"` behavior is preserved.
- `"excess-lf"` is currently an isolated preview mode:
  - it derives a tapered LF excess-phase correction from measured phase
  - it updates predicted excess phase and predicted group delay
  - it does not mix that correction with the existing minimum-phase magnitude FIR yet
  - corrected magnitude remains unchanged in that mode
- `"mixed"` is now the first combined realization mode:
  - it uses the existing magnitude correction target
  - it applies the LF excess-phase correction inside a single realized FIR synthesis path
  - it analyzes the realized taps and uses those realized taps for predicted magnitude and phase diagnostics
  - it is now wired into the Filters page as the user-facing mixed-phase mode
  - its LF excess-phase correction must stay common to left and right in stereo use; solving that correction independently per channel made bass widen and become locatable off-center
  - its realized FIRs must share one common stereo latency; letting each channel keep an independent mixed-phase peak position pulled the phantom center apart
- the Filters page now exposes only:
  - `"Minimum phase"`
  - `"Mixed phase"`
- `"excess-lf"` remains internal and is not exposed in the UI
- the user-editable mixed-mode controls are now:
  - `mixedPhaseMaxFrequencyHz` with a default of `220 Hz`
  - `mixedPhaseStrength` with a default of `1.0`

## Test Harness Added In This Session

File: `tests/filter_design_tests.cpp`

Added reusable synthetic helpers:

- `buildLinearAxis(...)`
- `buildImpulseValueSet(...)`
- `buildWrappedPhaseSpectrum(...)`
- `buildPhaseMeasurement(...)`
- `bandMeanAbs(...)`

Added baseline guardrail tests:

- `expectMinimumPhaseInputNeedsNoExcessCorrection()`
- `expectBulkDelayIsNotTreatedAsExcessPhase()`

Added isolated excess-phase preview tests:

- `expectExcessLfModeLeavesMinimumPhaseInputAlone()`
- `expectExcessLfModeIgnoresBulkDelay()`
- `expectExcessLfModeReducesLowFrequencyExcessPhase()`
- `expectExcessLfModeContainsCorrectionToLowFrequencies()`

Added combined mixed-mode tests:

- `expectMixedModeLeavesMinimumPhaseInputAlone()`
- `expectMixedModeIgnoresBulkDelay()`
- `expectMixedModeReducesLowFrequencyExcessPhase()`
- `expectMixedModeContainsCorrectionToLowFrequencies()`
- `expectMixedModePreservesMagnitudeVsMinimum()`
- `expectMixedModeStrengthZeroMatchesMinimum()`
- `expectMixedModePhaseLimitControlsCorrectionExtent()`
- `expectMixedModeStereoImpulsePeaksStayAlignedWithoutLargeBulkDelay()`
- `expectMixedModePreservesStereoLowFrequencyPhaseRelationship()`

These are intentionally pre-implementation tests. They verify that:

1. already-minimum-phase input does not get misclassified as excess phase
2. pure bulk delay does not get misclassified as excess phase

The synthetic `raw_phase_spectrum` fixture uses a dense linear frequency axis. That matters because large linear phase slopes are not unwrap-safe on sparse log-spaced samples.

## Why This Harness Makes Sense

Real measurements are too noisy and too timing-sensitive for the first implementation pass.

The useful validation loop here is:

1. build synthetic magnitude + phase inputs with known behavior
2. run the production `designFilters(...)` API
3. assert band-limited outcomes on the returned `FilterDesignResult`

That keeps the tests:

- deterministic
- fast
- local to `src/measurement`
- aligned with the actual data contracts consumed by the UI

## Metrics To Use

Keep the validation band-based. Point-wise phase checks are too brittle.

Primary metrics:

- `bandMeanAbs(frequencyAxisHz, excessPhaseDegrees, minHz, maxHz)`
- later: `bandMeanAbsDelta(frequencyAxisHz, a, b, minHz, maxHz)`
- later: `bandMeanAbsMagnitudeDeltaDb(frequencyAxisHz, aDb, bDb, minHz, maxHz)`
- later: impulse-domain energy ratio before vs after a main window

Initial working bands:

- LF correction band: `20..200 Hz`
- wider planning band: `20..300 Hz`
- containment band: `500..5000 Hz`

## Baseline Thresholds Already In Use

These are current no-false-positive thresholds:

- minimum-phase input:
  - mean abs excess phase in `20..300 Hz` < `2 deg`
- pure bulk delay:
  - mean abs excess phase in `20..300 Hz` < `3 deg`

These should stay green throughout the implementation.

## Tests To Add With The Next Implementation Slice

The isolated `"excess-lf"` preview mode is now in place. The next tests should land together with the first true combination step.

### 1. Combined Minimum-Phase + LF Excess Phase Reduction

Synthetic input:

- flat magnitude
- left channel has a low-frequency excess-phase hump
- right channel either matches or stays clean depending on the case

Expected:

- left `inputExcessPhaseDegrees` in `20..200 Hz` is materially nonzero
- left `predictedExcessPhaseDegrees` in `20..200 Hz` drops by at least `40%`

Current isolated preview assertion:

- `predicted / input <= 0.6` on mean absolute excess phase in `20..200 Hz`

Next combined assertion:

- `predicted / input <= 0.6` on mean absolute excess phase in `20..200 Hz`

### 2. Out-Of-Band Containment

For the same LF case:

- mean abs excess-phase change in `500..5000 Hz` stays small

Good first assertion:

- `bandMeanAbsDelta(...) <= 5 deg`

### 3. Magnitude Preservation

The excess-phase reduction path should not quietly become a magnitude redesign path.

Expected:

- corrected magnitude stays effectively unchanged versus the minimum-phase-only baseline

Good first assertion:

- predicted corrected response delta <= `0.25 dB` mean abs over `20..20000 Hz`

### 4. Channel Independence

Synthetic input:

- left channel has LF excess phase
- right channel does not

Expected:

- left improves materially
- right stays nearly unchanged

Good first assertions:

- right excess-phase delta in `20..200 Hz` <= `2 deg`
- right corrected magnitude delta <= `0.1 dB`

### 5. Impulse-Domain Improvement

After the phase correction exists, add an impulse-domain simulation check.

Expected:

- corrected impulse has less LF late-energy spread than the uncorrected synthetic transfer function

This test is important because phase plots can improve numerically while time-domain behavior does not.

## Recommended Implementation Sequence

### Step 1: Preserve Current Minimum-Phase Behavior

- keep `"minimum"` working exactly as it does now
- avoid changing existing magnitude-correction behavior while adding the new path

### Step 2: Add A New Internal Phase Mode

Add support in `filter_designer` for a second mode, likely one of:

- `"mixed"`
- `"minimum_plus_excess_lf"`

Status:

- done for isolated `"excess-lf"` preview mode
- UI text is still intentionally unchanged

### Step 3: Build The LF Excess-Phase Correction From Measured Phase

The correction path should:

1. start from measured raw phase
2. remove bulk delay
3. remove the minimum-phase contribution implied by the magnitude response
4. isolate low-frequency excess phase
5. derive a stable low-frequency phase correction with tapering

The correction should taper out smoothly above the configured LF region.

Status:

- done for the isolated preview mode
- the current implementation derives LF correction from measured excess phase with smoothing and tapering
- it is not yet combined with the minimum-phase FIR path

### Step 4: Combine With Existing FIR Path

Keep the existing magnitude correction and minimum-phase FIR path intact.

Then decide explicitly how the LF excess-phase correction is represented:

- all-pass section(s), or
- phase-shaped FIR component

Whichever representation is used, predicted diagnostics must reflect the combined result.

Status:

- done with a single realized FIR path for `"mixed"`
- the implementation now synthesizes one combined FIR from magnitude correction plus LF excess-phase correction
- predictions are based on realized taps, not the ideal target spectrum
- the mode is still measurement-side only and not yet exposed in the UI

## Implementation Strategy

### Recommended Representation

Use single-shot FIR synthesis in the frequency domain for the combined mode.

Do not introduce an IIR all-pass path for this feature first.

Reasoning:

- the app already uses FIR end to end
- export is already FIR/WAV based
- persistence and UI semantics already assume FIR output
- one realized FIR is easier to validate than a mixed FIR + IIR representation

Recommended mode split:

- `"minimum"` keeps current behavior exactly
- `"excess-lf"` remains an isolated preview/diagnostic mode
- `"mixed"` becomes the first true combined exportable mode

### Combined FIR Strategy

For `"mixed"`:

1. build the magnitude correction target exactly as the current minimum-phase path does
2. derive the minimum-phase angle implied by that magnitude
3. derive the LF excess-phase correction angle from the current `"excess-lf"` preview path
4. form one complex target spectrum
5. synthesize one real FIR from that spectrum
6. analyze the realized taps and use the realized result for all predicted outputs

Conceptually:

`H(f) = |H(f)| * exp(j * (phi_min(f) + phi_excess_lf_corr(f) + phi_delay(f)))`

Where:

- `|H(f)|` is the existing magnitude correction
- `phi_min(f)` is the minimum-phase angle for that magnitude
- `phi_excess_lf_corr(f)` is the new LF excess-phase correction
- `phi_delay(f)` is the intentional linear delay needed to make the FIR realizable

### Realization Path

The combined path should:

1. build the positive-frequency complex target
2. enforce Hermitian symmetry
3. IFFT to time domain
4. window/truncate to `tapCount`
5. FFT the realized taps back to frequency domain
6. populate predictions from the realized taps, not the ideal target spectrum

That last point is mandatory. If the realized taps differ from the ideal target, the predicted curves must report the realized behavior.

Status:

- implemented
- the current path builds the combined spectrum, realizes a time-domain impulse, extracts the strongest circular window, and analyzes the realized taps by FFT

### Delay Strategy

This is the hard part of Step 4.

Pure excess-phase correction wants look-ahead. A realizable FIR needs explicit delay budgeting.

So the combined mode needs an intentional policy:

- add enough linear delay to keep the FIR causal and stable
- keep that delay bounded so the impulse does not become pathological
- remove that intentional bulk delay when reporting diagnostic excess phase
- keep it included in the actual realized taps

Practical rule:

- `predictedExcessPhaseDegrees` should ignore intentional bulk delay
- `predictedGroupDelayMs` should include the actual realized filter delay
- stereo exports must keep a shared realized latency across left and right
- that shared latency should be the minimum delay needed to keep the pair aligned, not an automatic re-centering to half the FIR length

### Why Not IIR All-Pass First

Do not start with a cascaded all-pass IIR path.

That would force new architecture and validation work for:

- runtime representation
- export format
- persistence
- stability guarantees
- UI semantics for FIR versus IIR behavior

That is a larger architectural change than this feature currently needs.

### Suggested Code Structure

Inside `src/measurement/filter_designer.cpp`, add focused helpers along these lines:

- `buildCombinedCorrectionSpectrum(...)`
- `buildRealFirFromComplexSpectrum(...)`
- `analyzeRealizedFilter(...)`

The current `buildExcessPhaseCorrectionDegrees(...)` helper should remain the source of the LF phase correction shape, but not the final exported artifact.

### Acceptance Criteria For Step 4

Before trusting `"mixed"`, add tests that verify:

- realized LF excess-phase reduction in `20..200 Hz`
- out-of-band containment in `500..5000 Hz`
- magnitude preservation versus the current `"minimum"` path
- clean-channel independence
- sane impulse placement / bounded pre-ringing
- realized-taps FFT matches the predicted combined response

The key rule for Step 4:

- implement one combined FIR synthesis problem
- do not bolt excess-phase math onto the side of the current minimum-phase FIR without validating the realized taps

Current status:

- the current tests cover LF reduction, containment, magnitude preservation versus `"minimum"`, and no-false-positive behavior
- the current mixed-mode path now also preserves stereo bass coherence by:
  - averaging the derived LF mixed-phase correction across left and right before FIR realization
  - delaying the earlier channel after realization so both channels share one common peak latency
- the remaining useful follow-up for this step is an explicit impulse-domain quality check for pre-ringing / late-energy concentration
- current UI inspection of the aligned Group Delay view shows that the mixed excess-phase compensation is materially effective above roughly `50..70 Hz`, but weak below that region
- this weak sub-70 Hz behavior is likely not caused primarily by `mixedPhaseMaxFrequencyHz`; the correction weighting is already full below that limit
- the more likely causes are the current realization path:
  - strong regularization in `buildExcessPhaseCorrectionDegrees(...)`
  - the former `+-120 deg` phase-correction clamp, now replaced with a configurable mixed-mode `Phase Cap` control
  - the max-energy circular-window FIR realization plus tail fade in `buildMixedPhaseImpulse(...)`
- the next implementation slice should treat this as a realization-fidelity problem, not just a UI-parameter-tuning problem
- recommended follow-up:
  - add a failing synthetic regression for `20..60 Hz` excess-phase reduction
  - revisit the mixed FIR realization strategy with an explicit delay-budgeted complex fit
  - evaluate whether raising the now-configurable LF phase-correction cap materially helps below `70 Hz`
  - decouple phase regularization from the existing magnitude `smoothness` control

### Note: PRC-Like Direction

Acourate's published PRC material is a useful design hint, but not a drop-in algorithm specification.

The credible takeaway is:

- pre-ringing is treated as a consequence of excess-phase correction that is too strong or too localized
- the diagnostic domain is group delay / excess-phase behavior, not only the impulse chart
- the useful control is likely not a single global scalar on the whole FIR, but a targeted suppression or softening of specific low-frequency delay / excess-phase peaks

For this codebase that suggests a better future direction than a generic "pre-ringing amount" knob:

- detect dominant low-frequency excess-phase / group-delay peaks
- locally reduce or broaden the corresponding phase correction
- trade some phase linearization for less acausal energy before the main peak
- keep this peak-aware damping separate from the existing global controls:
  - `mixedPhaseMaxFrequencyHz`
  - `mixedPhaseStrength`
  - `mixedPhaseMaxCorrectionDegrees`

Practical implication:

- if a PRC-like feature is added later, model it as a peak-aware excess-phase compensation stage inside `src/measurement/filter_designer.cpp`
- do not frame it as a UI-only post-process on the already realized taps
- do not assume a vendor-style scalar range such as `-1..5` has portable meaning here; define our own semantics around measurable group-delay / impulse-domain effects

Observed tuning note:

- manual tuning showed that lowering `mixedPhaseStrength` reduces the visible pre-ringing
- that supports the working hypothesis that ringing is being driven by a small number of strongly corrected local excess-phase / group-delay features, not by an unrelated plotting artifact
- treat this as evidence in favor of a future peak-aware local optimization stage rather than only adding more global scalar controls

### Step 5: Predict Combined Diagnostics And Expose Mixed Mode

Update:

- `predictedExcessPhaseDegrees`
- `predictedGroupDelayMs`
- impulse diagnostics if needed

The predicted curves must reflect the combined minimum-phase + LF excess-phase correction, not just the existing minimum-phase FIR.

Status:

- done for the measurement-side predicted diagnostics
- done for the Filters page wiring
- the UI now exposes a `Phase Mode` selector with:
  - `Minimum phase`
  - `Mixed phase`
- the UI also exposes the two mixed-mode control parameters:
  - `Phase Limit` in Hz
  - `Phase Strength` in the range `0..1`
- the Group Delay graph now exposes three distinct trace families:
  - measured/input group delay
  - filter group delay
  - predicted corrected group delay
- the filter trace labels were renamed to make the comparison explicit and avoid confusing filter delay with measured delay
- the Excess Phase graph now has an `Unwrap phase` display toggle
- important correction: the unwrap view must use continuous excess-phase series produced by `filter_designer`, not a UI-side unwrap of already wrapped `[-180, 180]` differences
- the current code now publishes continuous input/predicted excess-phase series for that purpose, and the UI unwrap view uses those directly
- current practical status:
  - the interesting low-frequency region used for excess-phase correction is working well enough for tuning and validation
  - real listening confirmed two stereo constraints that are easy to miss in synthetic per-channel checks:
    - if mixed LF phase correction differs between channels, centered bass becomes airy and laterally locatable
    - if mixed FIR latency differs between channels, the phantom center moves outside the middle
  - the unwrapped Excess Phase view looks somewhat better than before, but it can still drift into implausible thousands of degrees at higher frequencies
  - that means the continuous excess-phase diagnostic is still not fully trustworthy outside the LF region of interest
  - leave this as a known limitation for now; do not use the high-frequency unwrapped excess-phase trace as a decision-grade diagnostic
- `excess-lf` stays internal for testing and algorithm iteration only

## Alternative Excess Phase Compensation Akin To Acourate

This section defines a complete implementation strategy for an Acourate-like excess-phase window
inside Wolfie.

The key distinction is:

- the current Wolfie mixed-phase path mostly decides *where and how strongly to apply correction*
- an Acourate-like excess-phase window should decide *which measured phase information is trusted
  and extracted as excess phase in the first place*

That means the frequency-dependent window belongs in the excess-phase analysis path before
regularization and FIR realization. It must not be implemented as a UI-only scalar or as a
post-hoc taper applied to an already-derived correction curve.

### Design Intent

The goal is not to clone Acourate's internal implementation. The goal is to add the same useful
control concept to Wolfie in a way that is deterministic, testable, and compatible with the
existing FIR export architecture.

The intended behavior:

- smaller excess-phase windows produce a more conservative phase estimate
- larger excess-phase windows expose more low-frequency phase detail for correction
- aggressive windows can improve group-delay / excess-phase plots but can also increase
  pre-peak energy and audible pre-ringing
- the realized filter response, not the ideal target response, remains the source of truth for
  predicted diagnostics

Acourate's published material also points in this direction:

- FDW-style controls are expressed in cycles, not only in Hz
- larger excess-phase correction values permit more phase correction
- aggressive excess-phase windows can produce pre-ringing and require test convolution
- PRC-style mitigation is group-delay / peak aware, not simply a global phase scalar

### Exact Signal Definition

Use the measured raw impulse response when it is available:

- source value set: `measurement.raw_impulse_response`
- x-axis unit: seconds
- left/right values: measured impulse amplitudes

For channel `c`, define:

- `h_c[n]`: measured raw impulse samples
- `t[n]`: measured sample time in seconds from the value-set x-axis
- `impulseSampleRate`: sample rate inferred from the raw impulse time axis
- `designSampleRate`: current filter-design / export sample rate
- `f_k`: dense positive-frequency analysis bin
- `n_ref_c`: channel reference sample

Use `impulseSampleRate` for time-window lengths and reference-search distances. Use
`designSampleRate` for the realized FIR and positive-bin target spectrum. The windowed diagnostic
may be evaluated only up to the lower of the measured-impulse Nyquist and design Nyquist; above
that, the phase correction target should taper to zero.

Reference sample rule:

1. prefer the sample whose `t[n]` is nearest `0.0` when the raw impulse x-axis is valid
2. if the strongest absolute impulse peak is within a small neighborhood of that zero-time sample,
   use the strongest local peak instead
3. if the x-axis is missing or invalid, fall back to the dominant absolute peak
4. for stereo export, derive diagnostics per channel but keep one shared correction target and one
   shared realized latency after FIR realization

The neighborhood should be conservative at first:

- `referenceSearchMs = 5.0`
- `referenceSearchSamples = round(referenceSearchMs * impulseSampleRate / 1000)`

This preserves compatibility with Wolfie's existing raw impulse convention, where the measurement
analyzer keeps a pre-roll and places the direct impulse near zero time.

### Cycle Window Definition

Add two cycle settings:

- `mixedPhaseWindowLowCycles`
- `mixedPhaseWindowHighCycles`

Initial defaults:

- `mixedPhaseWindowLowCycles = 1.5`
- `mixedPhaseWindowHighCycles = 3.0`

Initial clamps:

- low cycles: `0.5..12.0`
- high cycles: `0.5..12.0`

Do not require `lowCycles <= highCycles`. Some rooms may benefit from a longer LF aperture and a
shorter upper-bass aperture, while other cases may need the opposite. Normalize only to finite,
bounded values.

Cycle interpolation:

```text
lowAnchorHz = max(20.0, measurement.startFrequencyHz)
highAnchorHz = clamp(settings.mixedPhaseMaxFrequencyHz,
                     lowAnchorHz + 10.0,
                     nyquist * 0.45)

cycles(f) = interp_log_clamped(f,
                               lowAnchorHz, mixedPhaseWindowLowCycles,
                               highAnchorHz, mixedPhaseWindowHighCycles)
windowSeconds(f) = cycles(f) / max(f, 1.0)
windowSamples(f) = round(windowSeconds(f) * impulseSampleRate)
```

Use `mixedPhaseMaxFrequencyHz` as the first high anchor because it already defines Wolfie's mixed
phase region. If this later proves too limiting, add explicit anchor-frequency settings; do not
overload the cycle values to mean both aperture and frequency range.

### Window Shape

Start with a symmetric Hann aperture around the reference sample:

```text
halfWindowSamples(f) = max(1, round(windowSamples(f) * 0.5))
distance = abs(n - n_ref_c)

if distance > halfWindowSamples(f):
    w_f[n] = 0
else:
    x = distance / halfWindowSamples(f)
    w_f[n] = 0.5 + 0.5 * cos(pi * x)
```

This is intentionally simple and deterministic. It is also easy to test.

Do not start with separate pre/post windows. Acourate-style FDW can expose left/right half-window
concepts, but Wolfie should first land a stable symmetric implementation. The helper should still
be written so asymmetric windows can be added later without changing the public data model.

### Windowed Complex Response

For each analysis frequency `f_k`, compute the local complex response directly from the impulse:

```text
H_windowed_c(f_k) =
    sum_n h_c[n] * w_fk[n] * exp(-j * 2*pi * f_k * (t[n] - t[n_ref_c]))
```

Important details:

- evaluate on a dense linear positive-frequency axis, not on the log display axis
- use the same dense axis shape as the normal filter-design FFT path where practical
- skip DC for phase work; keep DC magnitude finite for minimum-phase reconstruction
- do not divide by the window weight sum; a centered unit impulse must still produce flat 0 dB
  magnitude, and sum-normalization would create a false frequency-dependent magnitude slope
- floor magnitude before taking logs: `magnitudeFloor = 1.0e-9`
- unwrap phase on the dense linear axis before resampling to the display axis

Recommended dense axis:

```text
fftSize = nextPowerOfTwo(settings.tapCount * 4)
positiveBinCount = fftSize / 2 + 1
f_k = designSampleRate * k / fftSize
maxWindowedAnalysisHz = min(0.5 * impulseSampleRate, 0.5 * designSampleRate)
```

Only trust windowed phase bins where `f_k <= maxWindowedAnalysisHz`. For bins above that limit,
phase correction should be zero or smoothly tapered out before FIR realization.

This is more expensive than one FFT because the aperture changes with frequency. That is acceptable
for the first implementation because filter design is not a real-time path. If performance becomes
an issue, optimize later with band grouping or cached window families.

### Windowed Minimum-Phase Reference

For each channel, derive the minimum-phase contribution from the same windowed magnitude used to
derive the windowed phase:

1. compute `abs(H_windowed_c(f_k))`
2. floor and optionally lightly smooth the dense magnitude
3. build a minimum-phase spectrum from that dense magnitude with the existing cepstral helper style
4. unwrap the resulting minimum-phase angle on the same dense frequency axis

This is mandatory. Subtracting a minimum-phase curve from the unwindowed magnitude would create a
false residual because the frequency-dependent window changes the magnitude model.

Important limitation:

- a response assembled from frequency-dependent apertures is not guaranteed to be a perfectly
  self-consistent transfer function
- the cepstral minimum-phase reference is therefore a useful model, not mathematical proof

Validation must include no-false-positive tests where known minimum-phase synthetic inputs remain
near zero excess phase after windowed analysis.

### Windowed Excess-Phase Diagnostic

For each channel:

```text
sourcePhase_c(f) = unwrap(arg(H_windowed_c(f)))
minimumPhase_c(f) = unwrap(arg(H_min_from_abs_windowed_c(f)))
residual_c(f) = sourcePhase_c(f) - minimumPhase_c(f)
residualDelaySeconds = robustLinearDelayFit(residual_c(f), f)
excess_c(f) = residual_c(f) + 2*pi*f*residualDelaySeconds
```

Then resample these dense results to `displayFrequencyAxisHz` for graph output and correction
target generation.

Residual delay fitting should be more robust than the current plain least-squares slope:

- ignore frequencies below `80 Hz` for the first implementation, matching current behavior
- ignore bins above the mixed-phase containment region when computing the residual delay
- use magnitude or coherence weighting when available
- at minimum, clamp out non-finite values and very low-magnitude bins

Recommended first fit band:

- `fitMinHz = 80.0`
- `fitMaxHz = min(2000.0, nyquist * 0.4)`

Do not let residual bulk delay dominate the excess-phase estimate.

### Correction Target

Feed the windowed excess phase into the existing correction regularization path:

```text
desiredCorrectionDegrees(f) =
    -windowedExcessPhaseDegrees(f)
    * mixedPhaseStrength
    * excessPhaseCorrectionWeightAt(f)
```

Keep the existing safety controls:

- `mixedPhaseStrength`: global amount of diagnosed excess phase to correct
- `mixedPhaseMaxCorrectionDegrees`: phase correction clamp
- `mixedPhaseMaxFrequencyHz`: outer containment / high anchor for the phase region

But change the semantics:

- the cycle window controls what phase structure is considered valid and correctable
- strength and cap control how aggressively Wolfie acts on that extracted structure
- the Hz limit remains a containment boundary, not the main extraction mechanism

The existing regularization in `buildExcessPhaseCorrectionDegrees(...)` can be reused at first, but
do not leave it permanently tied only to `settings.smoothness`. The current magnitude smoothness
control is not the right long-term owner for phase regularization.

Recommended later split:

- `mixedPhaseWindowLowCycles`
- `mixedPhaseWindowHighCycles`
- `mixedPhaseStrength`
- `mixedPhaseMaxCorrectionDegrees`
- later only if needed: `mixedPhasePhaseSmoothing`

### Stereo Export Policy

Keep the stereo policy stricter than Acourate's UI implies.

Diagnostics:

- analyze left and right independently
- publish left/right windowed input excess phase if that diagnostic path is active

Export correction:

- derive one shared low-frequency correction target for stereo export
- keep one shared realized latency across left and right filters
- continue delaying the earlier realized FIR peak so both channels share a common latency

Do not blindly average opposite channel corrections when they strongly disagree.

First shared-target rule:

```text
if sign(leftCorrection(f)) == sign(rightCorrection(f)):
    sharedCorrection(f) = 0.5 * (leftCorrection(f) + rightCorrection(f))
else:
    sharedCorrection(f) = correction with smaller absolute magnitude
```

Then regularize the shared correction curve.

This is conservative:

- it preserves common-mode LF correction
- it avoids creating a stereo phase correction that did not exist coherently in both channels
- it avoids cancellation from naive averaging when channels have opposite-sign local residuals

Later improvement:

- compute a coherence-gated mid-channel correction from `(left + right) / 2`
- damp bins where left/right excess-phase signs or group-delay peaks disagree

Do not add independent left/right window controls in the first user-facing version. Per-channel
expert controls can be added later for diagnostics, but they should not be the default export path.

### FIR Realization

The first implementation should keep the current mixed FIR realization unchanged:

- build the magnitude correction target exactly as the current mixed mode does
- apply the shared windowed LF phase correction to the minimum-phase correction spectrum
- realize one FIR per channel with `buildMixedPhaseImpulse(...)`
- align stereo peak latency after realization
- refresh all predicted diagnostics from the realized taps

This isolates the analysis change from the realization problem.

Known risk:

- the current strongest-circular-window extraction and tail fade can limit very-low-frequency
  correction fidelity
- if the windowed analysis improves the target but not the realized sub-70 Hz behavior, treat that
  as a delay-budgeted FIR realization problem, not as proof that the windowed analysis failed

The later realization improvement should be an explicit delay-budgeted complex fit:

- choose a shared allowed latency budget
- apply the corresponding linear phase before IFFT
- window/truncate around the planned causal region
- analyze realized taps and compare target-vs-realized phase error in the LF band

Do not expose a broad UI feature until realized-tap diagnostics show the intended effect.

### Fallback Behavior

If `measurement.raw_impulse_response` is missing or invalid:

- do not attempt true excess-phase-window analysis
- fall back to the existing phase-spectrum-based mixed mode if `measurement.raw_phase_spectrum`
  is valid
- otherwise fall back to minimum-phase behavior

Result semantics:

- `result.phaseMode` should still normalize to `"mixed"` if the user requested mixed mode
- diagnostics should reflect the actual fallback path
- later, add a lightweight diagnostic note if `FilterDesignResult` gains warning/status fields

Do not synthesize a fake frequency-dependent window from already-unwrapped phase. That would only be
a frequency taper with a misleading name.

### Code Structure

Keep the helpers private to `src/measurement/filter_designer.cpp` unless reuse becomes necessary.

Recommended helper split:

- `findImpulseReferenceSample(...)`
- `interpolateExcessPhaseWindowCycles(...)`
- `windowWeightAtSample(...)`
- `buildWindowedComplexResponse(...)`
- `buildWindowedMinimumPhaseRadians(...)`
- `buildWindowedPhaseDiagnostics(...)`
- `buildSharedStereoPhaseCorrectionDegrees(...)`
- `buildExcessPhaseCorrectionFromWindowedDiagnostics(...)`

Suggested data structures:

```cpp
struct WindowedPhaseDiagnostics {
    std::vector<double> denseFrequencyAxisHz;
    std::vector<double> sourcePhaseRadians;
    std::vector<double> minimumPhaseRadians;
    std::vector<double> excessPhaseRadians;
    std::vector<double> displayExcessPhaseDegrees;
    std::vector<double> displayExcessPhaseContinuousDegrees;
    std::vector<double> displayGroupDelayMs;
};
```

Implementation detail:

- keep the existing `PhaseDiagnostics` path unchanged for minimum mode and for fallback
- add a window-aware branch only when mixed mode has valid raw impulse input
- avoid moving this logic into `FiltersPage` or `WolfieApp`

### Data Model And Persistence

Add to `FilterDesignSettings`:

```cpp
double mixedPhaseWindowLowCycles = 1.5;
double mixedPhaseWindowHighCycles = 3.0;
```

Update all settings paths:

- `normalizeFilterDesignSettings(...)`
- workspace load in `WorkspaceRepository`
- workspace save in `WorkspaceRepository`
- `FiltersPage::areSettingsEqual(...)`
- `FiltersPage::currentSettings(...)`
- UI load/apply code if controls are exposed
- export code paths that copy or normalize `FilterDesignSettings`

Initial UI exposure can wait.

Recommended internal rollout:

1. add settings and persistence
2. keep controls hidden or non-user-facing
3. test via synthetic harness and direct settings construction
4. expose UI only after impulse-domain validation is stable

### Validation Strategy

Keep deterministic synthetic tests as the first gate.

Required guardrail tests:

- minimum-phase synthetic impulse remains near-zero excess phase after windowed analysis
- pure bulk delay remains near-zero excess phase after delay removal
- missing raw impulse falls back to the existing phase-spectrum mixed path
- missing raw impulse and missing phase spectrum falls back to minimum-phase behavior

Required window-semantics tests:

- on a controlled LF excess-phase fixture, larger cycle windows expose more correctable LF excess
  phase than smaller windows
- this monotonic assertion is only required for the controlled fixture, not for arbitrary room data
- high-frequency containment remains bounded in `500..5000 Hz`
- predicted corrected magnitude does not drift versus current mixed/minimum baseline

Required stereo tests:

- left/right realized FIR peaks remain aligned within one sample
- shared latency remains bounded and does not jump to half the FIR length
- opposite-sign channel residuals do not produce a larger shared correction than either channel
- stereo LF phase relationship remains within the existing tolerance used by current tests

Required impulse-domain tests:

- convolve the synthetic measured impulse with the realized mixed filter
- measure pre-peak energy in a fixed window before the main corrected peak
- measure post-peak LF energy concentration after the peak
- verify larger windows can increase pre-peak energy but stay below an explicit threshold
- verify the selected default window does not create worse pre-ringing than the current mixed mode
  beyond a small tolerance

Recommended first metrics:

```text
lfResidualRatio =
    bandMeanAbs(predictedExcessPhase, 20, 200)
    /
    bandMeanAbs(inputExcessPhase, 20, 200)

containmentDelta =
    bandMeanAbsDelta(predictedExcessPhaseCurrent,
                     predictedExcessPhaseWindowed,
                     500,
                     5000)

prePeakEnergyRatioDb =
    10 * log10(energyBeforePeak / max(energyAfterPeak, epsilon))
```

Initial thresholds should be conservative and tuned on synthetic fixtures first:

- LF residual ratio improves materially versus minimum mode
- containment delta <= `5 deg` mean absolute in `500..5000 Hz`
- magnitude delta <= `0.25 dB` mean absolute over `20..20000 Hz`
- pre-peak energy does not exceed the current mixed mode by more than an agreed small margin for
  the default cycle settings

### Implementation Order

1. Add `mixedPhaseWindowLowCycles` and `mixedPhaseWindowHighCycles` to `FilterDesignSettings`.
2. Normalize, persist, and compare the new settings, but do not expose them in the UI yet.
3. Add synthetic helpers that build impulse responses with known minimum-phase, pure-delay, and
   low-frequency excess-phase behavior.
4. Implement `buildWindowedComplexResponse(...)` on a dense linear positive-frequency axis.
5. Implement `buildWindowedPhaseDiagnostics(...)` and prove the guardrail tests pass.
6. Feed windowed excess phase into the existing correction regularization.
7. Keep `buildMixedPhaseImpulse(...)` unchanged and compare current mixed mode versus windowed
   mixed mode.
8. Add stereo shared-target logic and stereo latency checks.
9. Add convolved-response pre-ringing / pre-peak energy tests.
10. Only after the above is stable, expose the cycle controls in the Filters page.

### Acceptance Criteria

Do not consider this strategy ready for UI exposure until all of these are true:

- known minimum-phase inputs do not get misclassified as excess phase
- pure bulk delay does not get misclassified as excess phase
- the default cycle settings improve LF excess phase on controlled fixtures
- high-frequency containment remains bounded
- corrected magnitude remains effectively unchanged versus the current path
- stereo realized peak latency stays shared
- convolved-response pre-peak energy is measured and bounded
- predicted diagnostics are based on realized taps after FIR synthesis

Bottom line:

- for Wolfie, an Acourate-like excess-phase window should be a frequency-dependent
  phase-extraction aperture over the measured impulse response
- it should feed the existing mixed-phase FIR synthesis path
- it should fall back cleanly when raw impulse data is unavailable
- it should be validated by realized-tap and convolved-impulse diagnostics before becoming a
  broadly exposed UI feature

## Important Constraints

- Keep all math in `src/measurement`.
- Do not put phase-reduction logic into `FiltersPage` or `WolfieApp`.
- Keep `FilterDesignResult` as the seam for diagnostics consumed by the UI.
- Keep the tests deterministic and synthetic until the algorithm stabilizes.

## Validation Command

Current build/test command used for this harness:

```powershell
cmake -S . -B build-agent -G Ninja `
  -DCMAKE_MAKE_PROGRAM=E:/Programs/CLion/bin/ninja/win/x64/ninja.exe `
  -DCMAKE_CXX_COMPILER=E:/Programs/CLion/bin/mingw/bin/g++.exe `
  -DCMAKE_RC_COMPILER=E:/Programs/CLion/bin/mingw/bin/windres.exe

& 'E:/Programs/CLion/bin/ninja/win/x64/ninja.exe' -C build-agent wolfie_measurement_tests
ctest --test-dir build-agent --output-on-failure
```

## Resume Checklist

When resuming this work:

1. read `tests/filter_design_tests.cpp`
2. keep the two baseline guardrail tests passing
3. add the new phase mode inside `src/measurement/filter_designer.cpp`
4. add the LF reduction tests in the same branch as the implementation
5. verify magnitude preservation and out-of-band containment before touching UI
