# DSP Implementation Assessment - May 1, 2026

This assessment summarizes where the current Wolfie DSP implementation stands relative to the direction laid out in `doc/_dsp_deep_research.md`.

This deliberately ignores coherence and H1/H2 issues, per request.

## Executive summary

The current implementation is materially ahead of `_architecture.md`.

The architecture document still describes the filter path as minimum-phase only, but the codebase already implements:

- log ESS generation and inverse-sweep deconvolution
- separate direct and room analysis products
- stored loopback reference measurement and regularized reference compensation
- bulk-delay removal before minimum-phase reconstruction
- excess-phase extraction from measured phase minus minimum-phase reconstruction
- a low-frequency mixed-phase FIR mode on top of minimum-phase magnitude correction
- stereo-preserving heuristics for mixed-phase correction
- per-sample-rate Roon export

This is best described as a serious mixed-phase prototype rather than a minimum-phase-only baseline.

## Current implementation status

### 1. Measurement path

The measurement path is already on a solid ESS-based foundation.

`src/measurement/sweep_generator.cpp` generates a logarithmic sweep and builds the stereo playback plan. The sweep is exponential in frequency and is normalized after fade-in and fade-out are applied.

Relevant code:

- `src/measurement/sweep_generator.cpp:18`
- `src/measurement/sweep_generator.cpp:106`

`src/measurement/response_analyzer.cpp` performs inverse-sweep deconvolution, finds the dominant impulse peak, estimates latency, trims to a working impulse region, and publishes:

- raw impulse response
- room-window impulse/response/spectrum
- direct-window impulse/response/spectrum
- optional reference-compensated room/direct products

Relevant code:

- `src/measurement/response_analyzer.cpp:696`
- `src/measurement/response_analyzer.cpp:722`
- `src/measurement/response_analyzer.cpp:929`
- `src/measurement/response_analyzer.cpp:1094`

The direct window is shorter than the room window and is intentionally used to separate speaker/direct-arrival behavior from the fuller room response.

Relevant code:

- `src/measurement/response_analyzer.cpp:472`

### 2. Reference measurement path

The project already supports a loopback reference workflow, but not in the strongest form recommended by the research note.

Current behavior:

- a dedicated reference run is started in `MeasurementRunMode::Reference`
- the measurement controller switches the mic input to the configured loopback input
- microphone calibration is disabled during the reference run
- room measurements may then be reference-compensated against that stored reference result

Relevant code:

- `src/measurement/measurement_controller.cpp:69`
- `src/measurement/measurement_controller.cpp:187`
- `src/measurement/response_analyzer.cpp:590`

This is useful and practical, but it is still weaker than capturing a simultaneous loopback channel on every acoustic sweep.

### 3. Phase preparation and decomposition

This part is already conceptually aligned with the research note.

`src/measurement/phase_preparation.cpp` selects the best matched magnitude/phase source pair in this order:

1. direct, reference-compensated
2. direct
3. room, reference-compensated
4. room
5. raw

It prefers matched `_response` products first, then `_spectrum` products.

Relevant code:

- `src/measurement/phase_preparation.cpp:281`
- `src/measurement/phase_preparation.cpp:295`

For each channel, it then:

1. loads matched magnitude and phase
2. removes bulk delay derived from the impulse time axis
3. smooths the magnitude
4. reconstructs minimum phase from the smoothed magnitude using a cepstral minimum-phase construction
5. defines excess phase as the unwrapped principal difference between delay-corrected measured phase and reconstructed minimum phase

Relevant code:

- `src/measurement/phase_preparation.cpp:252`
- `src/measurement/phase_preparation.cpp:363`
- `src/measurement/phase_preparation.cpp:386`
- `src/measurement/phase_preparation.cpp:393`
- `src/measurement/phase_preparation.cpp:397`

That is the right high-level decomposition:

- measured phase
- minus bulk delay
- minus minimum-phase component from magnitude
- equals excess phase

### 4. Minimum-phase magnitude correction

Minimum-phase FIR magnitude correction is established and reasonably mature.

`src/measurement/filter_designer.cpp`:

- builds a bounded correction curve in dB
- regularizes it with a smoothness penalty
- converts that correction to a positive-frequency magnitude response
- constructs a minimum-phase spectrum via cepstral reconstruction
- IFFTs that to an FIR impulse

Relevant code:

- `src/measurement/filter_designer.cpp:249`
- `src/measurement/filter_designer.cpp:322`
- `src/measurement/filter_designer.cpp:338`
- `src/measurement/filter_designer.cpp:373`

This path is no longer just a plotting baseline. It is a working correction engine.

### 5. Mixed-phase path

The codebase already contains a real mixed-phase mode.

This contradicts `_architecture.md`, which still says the filter model is minimum-phase only.

Stale architecture text:

- `_architecture.md:331`

Actual implementation:

- `FilterDesignSettings` carries `phaseMode`, `mixedPhaseMaxFrequencyHz`, `mixedPhaseStrength`, and `mixedPhaseMaxCorrectionDegrees`
- `filter_designer.cpp` normalizes `"mixed"` as a supported phase mode
- in mixed mode, the solver builds a low-frequency excess-phase correction in degrees
- that correction is tapered above the configured LF range
- the correction is regularized and capped
- it is converted to a positive-frequency phase rotation
- that rotation is applied to the minimum-phase spectrum before IFFT

Relevant code:

- `src/core/models.h:364`
- `src/measurement/filter_designer.cpp:23`
- `src/measurement/filter_designer.cpp:555`
- `src/measurement/filter_designer.cpp:591`
- `src/measurement/filter_designer.cpp:718`
- `src/measurement/filter_designer.cpp:959`

This is not state of the art, but it is definitely a real mixed-phase implementation.

### 6. Stereo handling

Stereo handling is better than two naive independent channel fits, but it is still heuristic rather than optimal.

Current behavior in mixed mode:

- if both channels show substantial LF excess phase
- and the inter-channel LF phase difference is large enough
- the code builds left and right LF correction targets
- averages them into one shared mixed-phase correction
- then aligns left/right FIR peak positions to a shared latency

Relevant code:

- `src/measurement/filter_designer.cpp:1172`
- `src/measurement/filter_designer.cpp:1189`
- `src/measurement/filter_designer.cpp:1202`
- `src/measurement/filter_designer.cpp:1241`

This is a sensible protection against damaging stereo relationships, but it is still a threshold-and-average heuristic, not a joint stereo-constrained optimizer.

### 7. Excess-phase preview mode

There is also an internal `excess-lf` mode that performs a phase-only preview path without changing magnitude correction.

Relevant code:

- `src/measurement/filter_designer.cpp:947`
- `tests/excess_phase_mode_tests.cpp:12`

However, the UI phase-mode combo only exposes:

- Minimum phase
- Mixed phase

Relevant code:

- `src/ui/filters_page.cpp:1524`
- `src/ui/filters_page.cpp:1558`

So the internal capability is ahead of the exposed product surface.

### 8. Stereo diagnostics and post-filter analysis

The analysis layer already has useful stereo diagnostics.

`src/measurement/stereo_diagnostics.cpp` computes:

- delay mismatch
- direct impulse correlation
- phase RMS in low and mid bands
- magnitude mismatch
- phase similarity
- IACC-style time-window metrics

Relevant code:

- `src/measurement/stereo_diagnostics.cpp:420`
- `src/measurement/stereo_diagnostics.cpp:438`
- `src/measurement/stereo_diagnostics.cpp:468`

`src/measurement/filter_analysis.cpp` applies the designed filters to measured spectra and impulses and builds before/after diagnostics for direct and room windows.

Relevant code:

- `src/measurement/filter_analysis.cpp:279`
- `src/measurement/filter_analysis.cpp:307`

This is useful for verification, but it is still predictive/post-hoc analysis rather than a closed-loop DSP-on remeasurement pipeline.

### 9. Roon export

Roon export is already practical and tested.

The export path:

- redesigns filters per sample rate
- writes stereo 64-bit float WAVs
- writes Roon `.cfg` files
- writes a ZIP bundle

Relevant code:

- `src/measurement/filter_wav_export.cpp:62`
- `src/measurement/filter_wav_export.cpp:245`
- `src/measurement/filter_wav_export.cpp:273`

This part is operationally useful already.

## What the implementation already gets right

Relative to the research note, the following decisions are already correct:

- ESS is the default measurement primitive
- direct and room windows are separated
- bulk delay is removed before excess-phase interpretation
- minimum-phase reconstruction is derived from magnitude
- excess phase is treated as residual phase after minimum-phase reconstruction
- phase correction is mainly constrained to the low-frequency region
- stereo relationships are explicitly protected at least heuristically
- per-rate export for Roon is implemented

## Where the implementation is still below state of the art

### 1. Reference capture architecture is still weaker than it should be

The code uses a separate stored reference measurement, not a simultaneous loopback reference channel captured during each room sweep.

That means phase and latency normalization depend on:

- the stability of the hardware chain between runs
- the quality of the stored reference measurement
- the user performing both steps correctly

State of the art would capture loopback and acoustic response together per run.

### 2. Delay estimation is still too simple

The current analysis path is driven by the deconvolved impulse peak and integer-sample latency estimation.

Relevant code:

- `src/measurement/response_analyzer.cpp:727`

That is usable, but not enough for a best-in-class phase workflow. A better system would add:

- robust coarse direct-arrival estimation
- sub-sample refinement
- explicit fractional-delay correction before long-FIR design

### 3. The mixed-phase solver is still heuristic

Current mixed-phase correction is built by:

- converting excess phase to degrees
- scaling it by a taper and user strength
- solving a smooth regularized target curve
- clamping by a correction cap
- turning that into a spectral phase rotation

Relevant code:

- `src/measurement/filter_designer.cpp:555`

That works as a bounded heuristic. It is not a state-of-the-art optimizer.

Missing pieces include:

- a complex weighted least-squares objective
- explicit latency penalties
- explicit pre-ringing penalties
- explicit spatial robustness terms

### 4. No dedicated all-pass or fractional-delay stage

Everything phase-related still ends up in a long FIR.

There is currently no:

- fractional-delay alignment filter
- Thiran-style stage
- IIR/SOS all-pass fitter
- separate low-latency phase-correction layer

That is a major gap versus modern practical implementations.

### 5. Stereo coupling is not yet a true joint solve

Current stereo preservation is based on rules and shared LF averaging.

That is better than independent fits, but it is not equivalent to optimizing:

- both channels together
- under an inter-channel phase-difference constraint
- with a shared latency budget

### 6. No multi-position robustness model

The current path is still effectively single-measurement driven.

There is no:

- multi-seat low-frequency aggregation
- repeatability scoring across repeated measurements
- seat-cluster optimization
- stability mask for phase correction regions

That makes the current mixed-phase mode more vulnerable to overfitting one measurement position.

### 7. No dedicated sub/main integration workflow

The research note gives strong weight to subwoofer alignment and crossover-region timing work.

The current code has no dedicated:

- subwoofer-only workflow
- main-plus-sub measurement orchestration
- crossover-specific timing optimization
- sub/main delay/polarity assistant

That is a practical gap.

### 8. Validation is not yet closed-loop

The code predicts and analyzes, but it does not fully enforce a remeasure-with-DSP-active workflow.

There is no end-to-end automated path that:

1. exports or activates the designed correction
2. remeasures with DSP active
3. compares before/after at the main seat and nearby positions
4. rejects solutions that improve one metric while harming latency, pre-ringing, or stereo image

### 9. Product surface lags the engine

The engine contains more phase functionality than the UI exposes.

The clearest example is `excess-lf`: it exists and is tested, but the UI does not expose it.

That is not a DSP limitation, but it matters because it hides useful diagnostic capability.

## Necessary steps to reach state-of-the-art methods

The next steps should be architectural, not cosmetic.

### 1. Split the correction stack into explicit layers

Wolfie should stop treating long FIR design as the place where every timing problem gets absorbed.

The correction pipeline should become:

1. direct-path timing estimation
2. explicit integer/fractional delay alignment
3. minimum-phase magnitude correction
4. optional low-frequency excess-phase correction
5. stereo-constrained validation

This is the most important structural change.

### 2. Add sub-sample delay estimation and correction

Implement:

- coarse direct-arrival estimate from the direct window
- sub-sample refinement from local interpolation or phase-slope fit
- explicit fractional-delay correction before mixed-phase fitting

This will reduce the need to spend FIR length on simple geometric arrival mismatch.

### 3. Replace the current LF phase heuristic with a real optimizer

The next mixed-phase step should be one of these:

- weighted complex FIR least-squares solve against a fixed complex target
- weighted excess-group-delay fit with explicit regularization
- stable all-pass section fitting for low-latency use cases

In all cases, add:

- latency penalty
- pre-ringing penalty
- correction-band limits
- frequency weighting

### 4. Introduce a dedicated all-pass / low-latency phase layer

A state-of-the-art practical system should not force all phase correction into long FIRs.

Wolfie should add:

- a fractional-delay stage for direct alignment
- optional low-order all-pass correction for crossover and LF timing cleanup
- a long FIR only where the problem genuinely requires it

### 5. Make stereo optimization explicitly joint

Replace the current preserve-stereo heuristic with a real coupled objective.

The solver should optimize both channels together while constraining:

- inter-channel excess-phase difference in the imaging-relevant band
- shared latency
- maximum per-channel phase intervention

The current shared-average heuristic is a good interim safeguard, not a final method.

### 6. Upgrade measurement architecture to simultaneous loopback capture

The preferred measurement method should become:

- acoustic capture channel
- loopback capture channel
- same sweep, same run, same clock domain

That will make transfer estimation more stable and reduce dependence on separate reference runs.

The stored reference workflow can remain as fallback, but it should not be the primary state-of-the-art path.

### 7. Add multi-position LF robustness

For low frequencies, correction targets should be derived from more than one point.

At minimum, add:

- primary seat
- small local seat cluster
- robust LF aggregation rule
- optional repeat measurements for stability screening

That is necessary if the phase correction is meant to survive normal head movement.

### 8. Add dedicated subwoofer integration tools

This should become a first-class workflow:

- measure mains alone
- measure sub alone
- measure combined
- estimate crossover-region delay/polarity mismatch
- optimize for summation and stereo integrity

This is one of the highest-value state-of-the-art additions.

### 9. Make validation closed-loop and measurement-based

The validation layer should expand from predictive diagnostics to measured validation.

Add:

- DSP-on remeasurement
- before/after residual excess-phase metrics
- residual group-delay metrics
- latency budget checks
- pre-ringing metrics
- seat-spread metrics

The acceptance rule should be based on measured improvement, not only predicted improvement.

### 10. Expose the phase workflow more clearly in the product

Once the solver layers are cleaned up, the UI should expose phase tools as separate, intentional capabilities:

- minimum-phase magnitude correction
- LF phase preview
- LF phase correction
- stereo-preserving mode
- sub/main alignment mode

Right now the engine is ahead of the product surface.

## Priority order

If the goal is practical state-of-the-art progress, the work should happen in this order:

1. explicit fractional-delay alignment stage
2. simultaneous loopback capture architecture
3. proper joint mixed-phase optimizer
4. all-pass / low-latency phase layer
5. multi-position LF robustness
6. sub/main integration workflow
7. closed-loop DSP-on validation

That sequence gives the best improvement in method quality without wasting effort on secondary presentation issues.

## Assessment summary

Wolfie is already beyond a minimum-phase-only correction tool.

Today it is:

- a solid ESS-based measurement system
- a competent minimum-phase correction engine
- a meaningful excess-phase analysis tool
- a heuristic low-frequency mixed-phase prototype
- a practical Roon export tool

It is not yet state of the art because it still lacks:

- simultaneous loopback transfer capture
- sub-sample delay correction as a first-class stage
- a real stereo-coupled phase optimizer
- a dedicated all-pass/fractional-delay architecture
- multi-position robustness
- sub/main alignment tooling
- closed-loop measured validation

That is the real gap. The next advances should be in solver architecture and measurement rigor, not in more display polish.

## Verification note

This assessment was based on direct inspection of:

- `src/measurement/*`
- `src/ui/filters_page.cpp`
- `src/core/models.h`
- `_architecture.md`
- `tests/*phase*`
- `tests/measurement_loopback_tests.cpp`
- `tests/filter_analysis_tests.cpp`
- `tests/filter_export_tests.cpp`

The existing native measurement/filter test suite was also built and run successfully with:

- `cmake --build . --config Debug --target wolfie_measurement_tests`
- `ctest -C Debug --output-on-failure`

All 7 tests passed in `build-codex`.
