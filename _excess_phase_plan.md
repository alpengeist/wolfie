# Excess Phase Reduction Plan

## Goal

Add a low-frequency excess-phase reduction mode to `src/measurement/filter_designer.cpp` with deterministic validation feedback before and during implementation.

This should stay inside the measurement module. The UI can surface the mode later, but the math and the predicted diagnostics belong in filter design.

## Current State

As of 2026-04-28 after the latest implementation slice:

- `FilterDesignSettings::phaseMode` now normalizes to either:
  - `"minimum"`
  - `"excess-lf"`
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

### Step 5: Predict Combined Diagnostics

Update:

- `predictedExcessPhaseDegrees`
- `predictedGroupDelayMs`
- impulse diagnostics if needed

The predicted curves must reflect the combined minimum-phase + LF excess-phase correction, not just the existing minimum-phase FIR.

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
