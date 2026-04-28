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
- the remaining useful follow-up for this step is an explicit impulse-domain quality check for pre-ringing / late-energy concentration
- current UI inspection of the aligned Group Delay view shows that the mixed excess-phase compensation is materially effective above roughly `50..70 Hz`, but weak below that region
- this weak sub-70 Hz behavior is likely not caused primarily by `mixedPhaseMaxFrequencyHz`; the correction weighting is already full below that limit
- the more likely causes are the current realization path:
  - strong regularization in `buildExcessPhaseCorrectionDegrees(...)`
  - the current `+-120 deg` phase-correction clamp
  - the max-energy circular-window FIR realization plus tail fade in `buildMixedPhaseImpulse(...)`
- the next implementation slice should treat this as a realization-fidelity problem, not just a UI-parameter-tuning problem
- recommended follow-up:
  - add a failing synthetic regression for `20..60 Hz` excess-phase reduction
  - revisit the mixed FIR realization strategy with an explicit delay-budgeted complex fit
  - make the LF phase-correction cap configurable or raise it
  - decouple phase regularization from the existing magnitude `smoothness` control

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
- `excess-lf` stays internal for testing and algorithm iteration only

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
