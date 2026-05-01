# Excess Phase Refactor Plan

This is based on the research in deep-research-report.md.
Consult this document for in-depth details. It also contains valuable scientific references towards the end.

## Purpose

This note is a handoff plan for refactoring Wolfie's excess-phase preparation path.

The immediate goal is not to make mixed-phase correction more aggressive. The goal is to make the phase-analysis contract internally consistent so that:

- excess phase is derived from matched magnitude and phase inputs
- bulk delay is removed before excess-phase decomposition
- smoothing remains simple and shared between display and minimum-phase preparation
- current minimum-phase magnitude correction behavior stays intact

The user explicitly accepted one simplification:

- display smoothing may remain identical to the data-preparation smoothing used for minimum-phase reconstruction

That means this refactor should separate excess-phase preparation from `SmoothedResponse`, but it does not need a second independent smoothing UI or an additional persisted smoothing profile.

## Progress Update

Status as of the current implementation pass:

- done: Phase 1 reusable smoothing extraction
- done: Phase 2 matched phase-preparation helper
- done: most of Phase 3 filter-designer rewiring
- in progress: Phase 4 diagnostics, naming, and observability cleanup

What is now implemented in code:

- `response_smoother` exposes reusable magnitude smoothing primitives
- `phase_preparation` selects a matched TF source, removes bulk delay, smooths matched magnitude, rebuilds minimum phase, and derives excess phase
- `filter_designer` consumes prepared phase data for excess-phase diagnostics, predicted excess phase, and mixed-phase correction input
- `filter_designer` now publishes phase-preparation metadata and a process log with the major design steps
- the old ad hoc phase-diagnostics path in `filter_designer.cpp` has been removed so there is one internal phase-preparation path

What still remains after this pass:

- decide whether the current `_response`-first source preference remains the right long-term tradeoff versus native `_spectrum`-first preparation
- expose phase-preparation source metadata in any additional UI or exported diagnostics if that becomes useful
- continue tuning mixed-mode behavior only behind the expanded test coverage

## Reference Basis

### Primary PDF

`Digital Room Correction for Excess Phase and Stereo Phase Alignment.pdf`

Relevant sections:

- Chapter 3, "Measurement and estimation methods" (outline page 3, content around PDF pp. 4-5)
  - windowing defines what transfer function is being analyzed
  - direct-sound timing should be estimated from the direct path
  - coherence is the confidence metric for whether phase is trustworthy
- Chapter 4, "Excess-phase estimation and correction algorithms" (outline page 5, content around PDF pp. 7-10)
  - excess phase should come from a measured, windowed transfer function
  - smooth the log-magnitude before minimum-phase reconstruction
  - remove bulk delay before defining excess phase
  - fractional delay belongs in the architecture as a separate primitive
  - limited-band correction is the practical default
  - stereo work should separate arrival-time alignment, relative phase alignment, and room correction

Useful anchors from the extracted text:

- PDF p. 5: channel time alignment should be estimated from the direct sound
- PDF p. 5: coherence determines whether a phase estimate is worth trusting
- PDF p. 7: baseline excess-phase estimator is minimum-phase reconstruction plus subtraction after bulk-delay removal
- PDF p. 8: limited-band correction is usually the practical engineering compromise
- PDF p. 8: fractional delay should be its own primitive
- PDF p. 10: stereo work should explicitly separate arrival-time alignment, relative phase alignment, and room correction

### Local review notes

`_dsp_review.md`

Critical sections:

- 3.1 at line 196: current magnitude and phase inputs are mismatched
- 3.2 at line 231: display smoothing is leaking into correction semantics
- 4.1 at line 255: bulk-delay handling order is not ideal
- 4.2 at line 286: minimum-phase reconstruction method is acceptable
- 4.3 at line 296: current excess phase is useful as a diagnostic, not yet a hard target
- 5 at line 305: group delay is acceptable as a display product, not the solver basis

### Supporting local report

`deep-research-report.md`

Useful implementation references:

- line 305: pseudocode for excess-phase estimation
- line 360: the sensitive parts are magnitude floor, smoothing law, and bulk-delay estimator

## Current Code State

### Current smoothing path

- `src/measurement/response_smoother.cpp:222`
  - `buildSmoothedResponse(...)` builds the current `SmoothedResponse`
- `src/measurement/response_smoother.cpp:198`
  - `normalizeResponseSmoothingSettings(...)`

This path is currently fine for display and acceptable as the magnitude-preparation path for minimum-phase reconstruction.

### Current excess-phase path

- `src/measurement/filter_designer.cpp:595`
  - `buildPhaseInput(...)`
- `src/measurement/filter_designer.cpp:639`
  - `buildPhaseDiagnostics(...)`
- `src/measurement/filter_designer.cpp:697`
  - `buildExcessPhaseCorrectionDegrees(...)`
- `src/measurement/filter_designer.cpp:919`
  - `buildPredictedExcessPhaseSeries(...)`
- `src/measurement/filter_designer.cpp:1219`
  - `designFiltersForSampleRate(...)`

Today the path effectively does this:

1. take display-smoothed magnitude from `SmoothedResponse`
2. take phase from `measurement.raw_phase_spectrum`
3. remove one delay term from the IR start
4. rebuild minimum phase from the smoothed display magnitude
5. estimate another residual linear delay from the phase residual
6. call the difference excess phase

That is the core problem identified by `_dsp_review.md`.

### Available measurement products

The analyzer already stores the raw data needed to build a better prep path:

- `src/measurement/response_analyzer.cpp:944`
  - `measurement.room_impulse_response`
- `src/measurement/response_analyzer.cpp:949`
  - `measurement.direct_impulse_response`
- `src/measurement/response_analyzer.cpp:977`
  - `measurement.raw_phase_spectrum`
- `src/measurement/response_analyzer.cpp:1009`
  - `measurement.direct_phase_spectrum`

It also stores corresponding magnitude spectra and optional reference-compensated variants.

This means the refactor should stay inside `src/measurement`. It does not need a UI redesign first.

## Refactor Goal

Move the excess-phase preparation path from:

- unmatched display-smoothed magnitude plus separately sourced phase

to:

- one matched transfer-function preparation path that produces:
  - a chosen TF window/domain
  - bulk-delay-corrected phase
  - smoothed matched magnitude
  - minimum-phase estimate derived from that same magnitude
  - excess phase derived only after those steps

## Target Semantics

### 1. Keep smoothing simple

Keep one smoothing law and one smoothing settings object for both:

- smoothing-page display
- minimum-phase preparation magnitude

Do not add separate persisted "display smoothing" and "phase smoothing" settings in this pass.

Implementation implication:

- `SmoothedResponse` can remain as the display/correction magnitude product
- but excess-phase preparation must not reuse `SmoothedResponse` as a stand-in for "phase-decomposition magnitude"

Instead, the same smoothing kernel should be reused directly on the chosen transfer-function magnitude series.

### 2. Define one matched phase-decomposition input

Add a dedicated internal preparation object in `src/measurement`, for example:

```cpp
struct PreparedPhaseChannel {
    std::vector<double> nativeFrequencyAxisHz;
    std::vector<double> measuredMagnitudeDb;
    std::vector<double> smoothedMagnitudeDb;
    std::vector<double> measuredPhaseRadians;
    std::vector<double> delayCorrectedPhaseRadians;
    std::vector<double> minimumPhaseRadians;
    std::vector<double> excessPhaseRadians;
    double bulkDelaySeconds = 0.0;
    std::string sourceKey;
};
```

and a stereo wrapper:

```cpp
struct PreparedPhaseData {
    bool valid = false;
    PreparedPhaseChannel left;
    PreparedPhaseChannel right;
    std::string sourceWindow;
};
```

This should be an internal measurement-layer product, not a persisted workspace field yet.

### 3. Choose a transfer function explicitly

The decomposition source should be selected by a simple fallback rule:

1. `measurement.reference_compensated_direct_*`
2. `measurement.direct_*`
3. `measurement.reference_compensated_room_*`
4. `measurement.room_*`
5. `measurement.raw_*`

Rationale:

- Chapter 3 of the PDF treats windowing as part of what the phase target means.
- `_dsp_review.md` says the current path is too loose about mixing semantics.
- Direct-window data is the better default for delay and excess-phase preparation.
- Room-window data is still useful as a fallback when direct-window data is missing.

This selection should be explicit in code and in any future diagnostics.

### 4. Remove bulk delay before excess-phase decomposition

The new order should be:

1. choose the TF source
2. unwrap the measured phase on the source's native dense frequency axis
3. estimate bulk delay from direct-path timing first
4. remove that bulk delay from measured phase
5. smooth the matched magnitude from the same TF
6. reconstruct minimum phase from that matched smoothed magnitude
7. define excess phase as:
   - `delay_corrected_phase - minimum_phase`
8. only then resample for display or correction curves

This aligns with:

- PDF chapter 4 baseline estimator
- `_dsp_review.md` section 4.1
- `deep-research-report.md` line 305 pseudocode

### 5. Treat residual delay conservatively

The current path does:

- one delay removal from the IR start
- then another best-fit linear slope removal from the phase residual

The new path should keep any second-pass slope removal narrow and explicit, or omit it entirely in the first refactor pass.

Recommended approach for the first implementation:

- first remove only the direct-path bulk delay estimate
- do not automatically fit away a second large residual slope from `phase - phi_min`
- if a cleanup slope is still needed, make it:
  - small
  - separately named
  - diagnostically visible
  - bounded to avoid absorbing real low-frequency structure

This is important because `_dsp_review.md` identifies the current residual fit as capable of eating real structure.

### 6. Publish group delay from prepared phase data, not from the display path

Group delay should still be shown in the UI, but it should be derived from the prepared phase representation and then resampled to the display axis.

Do not treat the display-axis differentiated group delay as the internal truth source anymore.

This is a semantic cleanup, not a visible feature change.

## Proposed Module Shape

### New measurement helper

Add a focused helper under `src/measurement`, for example:

- `phase_preparation.h`
- `phase_preparation.cpp`

Suggested responsibilities:

- locate the best available TF window/spectrum pair
- unwrap native phase
- estimate and remove bulk delay
- smooth the matched magnitude using the existing smoothing kernel
- build minimum phase from that smoothed matched magnitude
- derive excess phase on the native axis
- resample prepared products to a requested display axis

This respects the existing architecture:

- measurement-domain math stays in `src/measurement`
- UI stays unchanged
- `WolfieApp` stays a coordinator

### Smoothing reuse

Refactor `response_smoother.cpp` so the actual smoothing kernel is reusable without going through `SmoothedResponse`.

Suggested extraction:

- `smoothMagnitudeSeries(...)`
- `flattenHighFrequencyTail(...)`
- any shared normalization helpers

Then:

- `buildSmoothedResponse(...)` calls the shared smoothing kernel for UI/display data
- `phase_preparation.cpp` calls the same kernel for matched phase-decomposition magnitude

That keeps the user-approved simplification while removing the current semantic leak.

### Filter designer integration

Change `filter_designer.cpp` to consume prepared phase data instead of rebuilding phase semantics ad hoc.

Specifically:

- replace `buildPhaseInput(...)` with a wrapper around the new preparation helper
- replace the current `buildPhaseDiagnostics(...)` internals so they consume prepared:
  - delay-corrected phase
  - minimum phase
  - excess phase
- replace `buildPredictedExcessPhaseSeries(...)` so predicted excess phase is computed with the same matched preparation rules

The mixed-phase correction builder:

- `buildExcessPhaseCorrectionDegrees(...)`

should continue to operate on a low-frequency excess-phase target, but that target should now come from the prepared matched TF.

## Detailed Implementation Sequence

### Phase 1: Extract reusable smoothing primitives

Files:

- `src/measurement/response_smoother.h`
- `src/measurement/response_smoother.cpp`

Tasks:

1. extract the smoothing kernel so it can smooth an arbitrary magnitude series on an arbitrary axis
2. keep the current settings normalization
3. keep current UI behavior unchanged
4. keep `SmoothedResponse` output unchanged

Acceptance:

- no visible smoothing-page change
- existing filter magnitude design remains unchanged for minimum-phase mode

### Phase 2: Add matched phase-preparation helper

Files:

- add `src/measurement/phase_preparation.h`
- add `src/measurement/phase_preparation.cpp`
- update `CMakeLists.txt`

Tasks:

1. load the best available phase/magnitude pair using explicit fallback order
2. operate on the source's native dense axis
3. unwrap measured phase
4. estimate bulk delay from the selected source
5. remove bulk delay
6. smooth the matched magnitude using the shared smoothing kernel
7. build minimum phase from that smoothed matched magnitude
8. derive excess phase
9. expose resampling helpers for display-axis publication

Acceptance:

- prepared data is internally matched
- no dependency on `SmoothedResponse` for excess-phase decomposition

### Phase 3: Rewire filter-designer diagnostics

Files:

- `src/measurement/filter_designer.cpp`

Tasks:

1. replace the current `buildPhaseInput(...)` source path
2. replace `buildPhaseDiagnostics(...)` internals with prepared products
3. replace `buildPredictedExcessPhaseSeries(...)` internals with the same semantics
4. leave the current minimum-phase magnitude correction path unchanged
5. leave mixed-mode correction scope unchanged except for better phase preparation

Acceptance:

- current minimum-mode behavior remains stable
- excess-phase input and predicted curves now move consistently with chosen TF source

### Phase 4: Tighten diagnostics and naming

Tasks:

1. record which TF source was used for phase preparation
2. expose that source in logs or result metadata if practical
3. make any residual cleanup slope, if still used, explicit and bounded

This phase is optional for the first pass, but it will make later debugging much easier.

## Validation Plan

Progress note:

- the matched-source invariance, direct-source preference, raw fallback, input-group-delay publication, continuous excess-phase continuity, and several mixed-mode behavior tests are now implemented in `tests/filter_design_tests.cpp`
- the test runner now includes the mixed-mode and mixed-export cases that were previously present but not executed

There is already a good synthetic harness in `tests/filter_design_tests.cpp`.

Relevant existing tests:

- line 457: `expectMinimumPhaseInputNeedsNoExcessCorrection()`
- line 521: `expectBulkDelayIsNotTreatedAsExcessPhase()`
- line 910: `expectMixedModeReducesLowFrequencyExcessPhase()`
- line 1426: `expectContinuousExcessPhaseSeriesStaySmoothAcrossWraps()`

### Keep these green

These are the current guardrails that should not regress:

1. minimum-phase input is not misclassified as excess phase
2. pure bulk delay is not misclassified as excess phase
3. continuous excess-phase publication does not jump across wraps

### Add new tests for this refactor

#### 1. Matched-source invariance

Build a synthetic measurement where:

- the chosen phase-decomposition TF is fixed
- `SmoothedResponse` is changed independently

Expected:

- excess-phase diagnostics do not change materially

This is the test that proves display smoothing no longer leaks into phase semantics.

#### 2. Source-selection fallback

Create synthetic cases where only one of these is present:

- direct
- room
- raw

Expected:

- the preparation path uses the highest-priority available source

#### 3. Direct-vs-room semantic difference

Create a synthetic case where:

- direct-window TF has one phase character
- room-window TF has a different late-reflection-driven character

Expected:

- phase preparation differs in a deliberate, explainable way
- the selected source determines the result

This validates the chapter 3 point that windowing changes the meaning of the target.

#### 4. Bulk-delay-first behavior

Create a case with:

- known direct-path delay
- known low-frequency excess-phase structure

Expected:

- the known delay is removed first
- the LF excess phase remains visible instead of being flattened by a later slope fit

#### 5. Predicted excess-phase symmetry

For minimum-phase mode:

- predicted excess phase should remain equal or nearly equal to input excess phase after the new preparation rules

This verifies that the refactor does not invent a phase change in minimum mode.

## Non-Goals For This Pass

Do not expand scope into these areas yet:

- new UI controls for separate smoothing profiles
- persisted extra analysis fields in `WorkspaceState`
- H1/H2 estimator support
- coherence calculation and masking
- a new all-pass or fractional-delay correction stage
- a new stereo alignment workflow

The PDF and the review documents both support those as future steps, but they should not be bundled into this refactor.

## Expected Outcome

After this refactor:

- minimum-phase filter design should behave the same in the magnitude domain
- excess-phase diagnostics should be derived from a matched TF instead of mismatched inputs
- bulk delay should be separated earlier and more cleanly
- group-delay publication should be better grounded
- mixed-mode correction should still be limited-band and conservative, but built on a stronger analysis basis

That is the correct stopping point for this session handoff. It improves correctness first, without turning the refactor into a larger architecture project.
