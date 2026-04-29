# Wolfie DSP Review

## Scope

This review is scoped to:

- full-range left speaker
- full-range right speaker
- single listening position as the primary correction point
- stereo image stability and timing consistency between left and right

This review explicitly excludes:

- subwoofer integration
- crossover alignment
- multiway loudspeaker crossover phase work
- binaural / BRIR / crosstalk cancellation workflows

The comparison baseline is the DSP approach described in `deep-research-report.md`, with emphasis on:

- ESS measurement quality
- transfer-function estimation
- separation of bulk delay, minimum phase, and excess phase
- correct use of data products at each processing step
- robustness limits for mixed-phase correction in rooms

## Executive Summary

Wolfie is already credible as a **minimum-phase magnitude-correction tool**. Its log-sweep measurement path, inverse sweep construction, minimum-phase FIR generation, and per-rate Roon export are all directionally correct.

Wolfie is **not yet state of the art for full-range stereo mixed-phase room correction**. The main issue is not a single broken formula. The deeper issue is that Wolfie currently mixes together data that should remain distinct:

- display smoothing data
- correction-design magnitude data
- raw measured phase
- bulk-delay estimates
- phase-only correction targets

That causes the current excess-phase path to be only partially trustworthy. In short:

- the minimum-phase path is materially stronger than the mixed-phase path
- the mixed-phase path is a promising prototype, not yet a defensible production algorithm
- the next major gains will come from improving the analysis contract, not from making the phase correction more aggressive

## High-Level Verdict

### What Wolfie already gets right

1. **ESS as the measurement primitive**
   - `src/measurement/sweep_generator.cpp`
   - `src/measurement/response_analyzer.cpp`
   - Wolfie uses a logarithmic sine sweep and an inverse sweep filter. That is the right measurement family for room correction work.

2. **Minimum-phase FIR construction**
   - `src/measurement/filter_designer.cpp`
   - The minimum-phase filter path uses a homomorphic / cepstral-style reconstruction from the designed magnitude response. That is a valid and practical minimum-phase design method.

3. **Phase unwrapping fix**
   - `src/measurement/dsp_utils.cpp`
   - Phase unwrapping compares adjacent wrapped samples rather than adjacent already-unwrapped samples. That is the correct behavior and avoids runaway wrap accumulation.

4. **Stereo safeguard in mixed mode**
   - `src/measurement/filter_designer.cpp`
   - Wolfie already constrains LF mixed-phase correction to a common left/right correction profile, which is better than solving each channel independently.

5. **Per-rate export**
   - `src/measurement/filter_wav_export.cpp`
   - Wolfie exports per-sample-rate Roon assets, which matches the practical deployment model.

### What is not yet good enough

1. No loopback or dual-channel transfer-function estimation
2. No coherence or H1/H2 diagnostics
3. No direct-path-first delay estimation stage
4. Excess-phase decomposition is built from mismatched magnitude and phase inputs
5. Mixed-phase correction is underconstrained relative to current best practice
6. Validation is too weak for a phase-sensitive workflow

## Detailed Findings

## 1. Measurement Pipeline

### 1.1 ESS generation and inverse sweep

Wolfie generates a logarithmic sweep in `generateSweepSamples()` and builds the inverse sweep in `buildInverseSweepFilter()`.

Relevant code:

- `src/measurement/sweep_generator.cpp`
- `src/measurement/response_analyzer.cpp`

Assessment:

- This is broadly correct.
- The inverse filter uses the usual time-reversal plus exponential amplitude correction strategy.
- The current loopback tests show the deconvolution is sharp enough to recover a clean impulse in the synthetic case.

This part is good enough to keep.

### 1.2 Sequential left/right measurement layout

Wolfie plays left and right sweeps in separate time segments inside one stereo playback file.

Relevant code:

- `src/measurement/sweep_generator.cpp`

Assessment:

- This is a reasonable full-range stereo measurement strategy.
- It avoids direct acoustic overlap between the left and right excitation segments.
- It keeps the UI and backend simpler than simultaneous dual-speaker excitation.

This is fine for now.

### 1.3 Missing loopback reference

The report strongly favors an ESS measurement with a loopback reference channel. Wolfie does not currently have that.

Relevant code:

- `src/audio/audio_backend.h`
- `src/audio/asio_audio_backend.cpp`
- `src/audio/winmm_audio_backend.cpp`

Current behavior:

- the audio session exposes only one captured stream
- ASIO captures one microphone input channel
- WinMM captures mono input only
- there is no reference capture path and no reference-aware transfer-function estimator

Why this matters:

- without a reference channel, Wolfie cannot remove interface timing and phase from the transfer estimate
- without a reference channel, Wolfie cannot compute coherence in the standard dual-channel sense
- without a reference channel, Wolfie cannot compute H1 and H2 estimators
- without a reference channel, phase correction decisions are made on a weaker measurement basis than the report recommends

Conclusion:

For full-range stereo mixed-phase work, this is the single largest measurement limitation in the current architecture.

### 1.4 Single analysis window serving multiple purposes

Wolfie crops the deconvolved IR around the detected peak and then applies one cosine-tapered analysis window before FFT.

Relevant code:

- `src/measurement/response_analyzer.cpp`

Assessment:

- This is acceptable for a first-pass magnitude display pipeline.
- It is not enough for a robust mixed-phase room-correction workflow.

Why:

- direct-sound timing work and excess-phase estimation benefit from a direct-path-focused view
- full-room low-frequency correction benefits from a longer room window
- a single window is forcing one transfer function to serve both tasks

Conclusion:

Wolfie needs distinct analysis products, not just one IR and one FFT.

## 2. Transfer-Function Estimation and Confidence

### 2.1 No coherence

Wolfie does not compute coherence.

Assessment:

- That is acceptable for simple magnitude smoothing.
- That is not acceptable as the final basis for phase correction decisions.

Why:

- the report explicitly treats coherence as the confidence metric for phase trustworthiness
- without it, Wolfie cannot distinguish stable phase structure from room interference, noise, or leakage

### 2.2 No H1 / H2 estimator support

Wolfie does not compute H1 or H2 transfer estimates.

Assessment:

- This is a gap, not a fatal flaw for the current minimum-phase path.
- It becomes important once Wolfie wants to claim reliable mixed-phase correction.

### 2.3 No repeatability metrics

Wolfie measures once and designs immediately.

Assessment:

- For full-range room phase work, that is too optimistic.
- The report is clear that stable, repeatable structure should be favored over single-shot phase traces.

Recommended direction:

- add repeated sweeps at one seat
- compute phase spread or excess-phase spread per frequency
- down-weight or mask unstable regions before correction

## 3. Magnitude Path vs Phase Path

This is the most important finding in the entire review.

### 3.1 Wolfie uses different source data for magnitude and phase decomposition

Current design:

- magnitude correction is based on `SmoothedResponse`
- phase diagnostics are based on `measurement.raw_phase_spectrum`
- minimum-phase reconstruction for excess-phase diagnostics is rebuilt from the smoothed magnitude, not from the same complex transfer function that produced the phase

Relevant code:

- `src/measurement/response_smoother.cpp`
- `src/measurement/filter_designer.cpp`

Why this matters:

The excess-phase definition only has clean mathematical meaning when phase and minimum-phase reference are derived from the same transfer function, or from deliberately matched versions of the same transfer function.

Wolfie is currently doing this:

1. take smoothed magnitude for correction design
2. take raw measured phase from a separate path
3. reconstruct minimum phase from the smoothed magnitude
4. call the difference "excess phase"

That is not the same as:

1. estimate one complex transfer function
2. choose a window and smoothing law for that transfer function
3. reconstruct minimum phase from that same magnitude
4. define excess phase relative to that matched pair

Consequence:

Wolfie's excess-phase estimate is partly a DSP quantity and partly a byproduct of using mismatched inputs. That makes the current mixed-phase correction less trustworthy than the UI suggests.

### 3.2 Display smoothing is leaking into correction semantics

`SmoothedResponse` is a useful object, but today it serves too many roles.

It is being used as:

- a UI display response
- a correction-design magnitude response
- the magnitude basis for minimum-phase reconstruction in the phase diagnostic path

That last use is the problematic one.

Recommendation:

Separate these concepts explicitly:

1. **display magnitude**
2. **correction-design magnitude**
3. **phase-decomposition magnitude**

Those may share machinery, but they should not be treated as the same data product.

## 4. Bulk Delay, Minimum Phase, and Excess Phase

### 4.1 Bulk delay handling is only partially correct

Wolfie removes delay in two steps:

1. it removes a time-origin delay derived from the IR start
2. it then fits and removes a residual linear phase slope from the phase-minus-minimum-phase residual

Relevant code:

- `analysisDelaySeconds()` in `src/measurement/filter_designer.cpp`
- `estimateLinearDelaySeconds()` in `src/measurement/filter_designer.cpp`
- `buildPhaseDiagnostics()` in `src/measurement/filter_designer.cpp`

Assessment:

- The intent is good.
- The ordering is not yet ideal.

Why:

- the report recommends bulk delay estimation from direct sound first
- Wolfie currently estimates residual delay from the phase residual after the minimum-phase comparison
- that can absorb real low-frequency structure into a linear-delay fit

For full-range stereo speakers, the better order is:

1. direct-path onset estimate
2. inter-channel direct-path delay estimate
3. remove bulk delay
4. then decompose the residual phase into minimum-phase and excess-phase parts

### 4.2 Minimum-phase reconstruction itself is acceptable

Wolfie's minimum-phase reconstruction is based on a cepstral/homomorphic method.

Assessment:

- This is fine.
- The mathematical method is not the core issue.
- The input semantics are the core issue.

### 4.3 Excess-phase estimate is usable as a diagnostic, not yet as a hard correction target

Because Wolfie's magnitude and phase inputs are not yet properly matched, the current excess-phase chart should be treated as:

- a planning / qualitative diagnostic
- not yet a precise engineering target

That matters because the mixed-mode correction path is built from this estimate.

## 5. Group Delay

Wolfie computes group delay by differentiating unwrapped phase on the log-spaced display axis.

Relevant code:

- `buildGroupDelayMs()` in `src/measurement/filter_designer.cpp`

Assessment:

- This is acceptable for a display diagnostic.
- It is not ideal as the only basis for optimization decisions.

Why:

- numerical differentiation on a resampled display axis is noisy
- the display axis is optimized for plotting, not estimation
- a serious solver should work from a denser and more internally consistent phase representation

Conclusion:

Wolfie's group-delay publication is useful, but it should remain a display product unless the underlying estimation path is strengthened.

## 6. Minimum-Phase Filter Design

### 6.1 Magnitude correction solver

Wolfie builds a desired magnitude correction curve, regularizes it with a second-difference penalty, and then turns that into a minimum-phase FIR.

Relevant code:

- `solveRegularizedCurve()`
- `buildCorrectionCurve()`
- `buildMinimumPhaseImpulse()`
- `src/measurement/filter_designer.cpp`

Assessment:

- This is a pragmatic design.
- It is not theoretically exotic, but it is sound enough for a production minimum-phase baseline.

### 6.2 Target anchoring regression

The existing test binary currently fails the target-level anchoring case.

Relevant test:

- `expectTargetCurveAnchorsToMeasuredLevel()` in `tests/filter_design_tests.cpp`

Observed result:

- the first failing message is `flat offset response still produced a non-trivial correction`

Implication:

- there is already a baseline mismatch between target semantics and filter design behavior
- this should be fixed before making the mixed-phase path more sophisticated

This is not a phase-theory issue, but it will contaminate objective validation if left unresolved.

## 7. Mixed-Phase / Excess-Phase Correction

### 7.1 What Wolfie currently does

Wolfie's mixed mode currently:

1. computes an excess-phase estimate
2. builds a limited low-frequency phase correction curve
3. applies that correction as a positive-frequency phase rotation
4. combines it with the minimum-phase magnitude design
5. IFFTs to a real impulse
6. selects a high-energy circular window
7. aligns left and right impulse peaks

Relevant code:

- `buildExcessPhaseCorrectionDegrees()`
- `buildPositivePhaseCorrectionRadians()`
- `applyPositivePhaseCorrection()`
- `buildMixedPhaseImpulse()`
- `refreshMixedPhaseChannel()`
- `src/measurement/filter_designer.cpp`

### 7.2 What is good about the current mixed-mode approach

- It is limited-band, not full-band.
- It exposes strength and max-correction controls.
- It uses one shared LF correction profile for left and right.
- It avoids turning mixed mode into unconstrained full-band phase flattening.

These are all good instincts.

### 7.3 What is still missing

The current mixed-phase design lacks several things that the report treats as essential:

1. **confidence weighting**
   - no coherence weighting
   - no repeatability weighting

2. **explicit time-domain control**
   - no early-energy penalty
   - no pre-ringing metric in the solver
   - no delay-slack optimization

3. **clear causal target formulation**
   - the circular-window extraction is pragmatic, but it is still a heuristic

4. **matched decomposition input**
   - the solver is acting on an excess-phase estimate that is not yet derived from a matched complex TF

Conclusion:

Wolfie's mixed mode is a useful exploratory tool. It is not yet a state-of-the-art mixed-phase correction engine.

## 8. Stereo-Specific Findings for Full-Range Left/Right

### 8.1 Common LF phase correction is the right default

For full-range stereo, Wolfie is correct to avoid independent LF mixed-phase correction per channel.

Why:

- independent low-frequency correction can damage phantom-center stability
- a common LF phase strategy is safer for stereo image preservation

### 8.2 Missing explicit direct-path inter-channel alignment stage

Even without crossovers, Wolfie still needs a more explicit stereo timing stage.

For full-range stereo, the key question is:

- what is the direct-arrival timing mismatch between left and right at the listening point?

Wolfie does detect per-channel impulse timing, but it does not yet expose a dedicated stereo direct-path alignment stage as a first-class design step.

Recommended behavior:

1. estimate left/right direct-arrival timing from gated direct sound
2. separate that from room excess-phase work
3. apply fractional or integer delay as a dedicated alignment result
4. only then decide whether any residual excess-phase correction is warranted

### 8.3 No joint stereo optimization beyond shared LF correction

The shared correction profile is good, but it is still limited.

Wolfie does not yet optimize a stereo objective such as:

- preserving relative phase difference
- limiting inter-channel phase divergence after correction
- minimizing stereo mismatch in the correction band

That would be the next serious improvement after the measurement/decomposition contract is fixed.

## 9. Export and Deployment

Wolfie exports per-sample-rate stereo FIRs correctly enough for the intended Roon workflow.

Relevant code:

- `src/measurement/filter_wav_export.cpp`

What is good:

- per-rate filter design
- stereo WAV output
- per-rate CFG output
- ZIP packaging

What is still missing:

- explicit latency manifest
- explicit headroom recommendation or gain accounting
- validation metadata tied to the exported bundle

These are not the most urgent DSP issues, but they matter once mixed-phase export becomes more serious.

## 10. Validation and Test Coverage

Wolfie's current tests are valuable, especially the synthetic loopback and mixed-phase guardrails.

Relevant files:

- `tests/measurement_loopback_tests.cpp`
- `tests/filter_design_tests.cpp`

What is good:

- synthetic ESS recovery checks
- bulk-delay-not-excess tests
- mixed-mode containment checks
- stereo relationship checks

What is missing:

- repeated-measurement stability tests
- coherence-related tests
- post-correction objective validation against measured re-runs
- transient / pre-ringing risk metrics

The test suite is better than "no DSP tests," but it still tests the current model more than it tests the final acoustic claims.

## Data Usage Matrix

| Process step | Correct data product | Wolfie today | Assessment |
|---|---|---|---|
| Impulse onset detection | Deconvolved IR near direct arrival | Raw deconvolved IR | Good |
| Inter-channel timing | Gated direct-path IR or TF ratio | Indirectly available, not a dedicated stage | Partial |
| Magnitude target fitting | Smoothed magnitude response | `SmoothedResponse` | Good |
| Excess-phase decomposition | Phase and magnitude from the same TF/window | Raw phase plus smoothed magnitude | Incorrect contract |
| Confidence weighting | Coherence / H1-H2 / repeatability | Not present | Missing |
| Mixed-phase correction target | Stable low-band excess phase after delay removal | Prototype estimate from mismatched inputs | Weak |
| Stereo phase preservation | Joint stereo constraint | Shared LF correction only | Partial |
| Export | Per-rate FIR artifacts | Implemented | Good |

## Prioritized Recommendations

## Priority 0: Stabilize the current baseline

Before any deeper phase work:

1. fix the target anchoring regression
2. verify minimum-phase magnitude correction still behaves as intended
3. keep the existing export path stable

## Priority 1: Fix the measurement contract

Add a reference-aware measurement path:

1. loopback / reference capture
2. dual-channel TF estimation
3. coherence
4. H1 and H2
5. repeated sweep support

This is the highest-value change for mixed-phase credibility.

## Priority 2: Fix the decomposition contract

Create explicit analysis products:

1. direct-window complex TF
2. room-window complex TF
3. display magnitude
4. correction-design magnitude
5. phase-decomposition TF

Then compute excess phase only from matched inputs.

## Priority 3: Separate stereo alignment from excess-phase correction

For full-range left/right speakers:

1. estimate direct-path left/right mismatch
2. design a dedicated delay correction
3. treat residual excess phase as a separate optional step

This will make the workflow much clearer and reduce overfitting.

## Priority 4: Replace the current mixed-mode heuristic with a more explicit solver

Target properties:

- limited band
- delay slack
- early-energy penalty
- coherence weighting
- stereo-relative constraint

The current mixed path should evolve toward that, rather than toward more aggressive unrestricted phase flattening.

## Development Phase Plan

## Phase 0: Baseline Repair

Goal:

- make the current minimum-phase baseline reliable before expanding scope

Tasks:

1. fix target anchoring behavior
2. confirm minimum-phase export remains stable across sample rates
3. keep current mixed mode behind clear "experimental" semantics

Exit criteria:

- current DSP tests pass
- flat-offset target cases behave correctly
- no regression in minimum-phase export

## Phase 1: Measurement Infrastructure

Goal:

- produce trustworthy transfer-function data for phase work

Tasks:

1. extend the audio/session model to support reference capture
2. store reference-aware TF artifacts in `MeasurementResult`
3. compute coherence, H1, and H2
4. support repeated sweeps at one listening seat

Exit criteria:

- Wolfie can compute and persist a reference-aware TF
- coherence is available as a first-class analysis product
- repeated measurements can be compared for stability

## Phase 2: Analysis Product Separation

Goal:

- stop mixing display data with correction-design data

Tasks:

1. define separate structures for:
   - display magnitude
   - correction-design magnitude
   - phase-decomposition TF
2. add direct-window and room-window analysis outputs
3. compute bulk delay from the direct path first
4. compute excess phase only from matched magnitude and phase inputs

Exit criteria:

- excess-phase charts are derived from a matched TF
- the decomposition can be explained cleanly as:
  - bulk delay
  - minimum phase
  - excess phase

## Phase 3: Stereo Alignment Stage

Goal:

- make direct-path stereo timing a separate design result

Tasks:

1. estimate inter-channel arrival mismatch from direct sound
2. design integer/fractional delay correction
3. expose the delay result separately from mixed-phase correction
4. validate phantom-center stability before and after delay alignment

Exit criteria:

- Wolfie can align left/right arrival time explicitly
- phase correction is no longer burdened with solving pure geometric delay

## Phase 4: Mixed-Phase Solver Upgrade

Goal:

- replace the current phase-rotation heuristic with a more controlled correction stage

Tasks:

1. keep correction limited to a user-defined LF band
2. weight correction by coherence and repeatability
3. add explicit delay slack and early-energy penalties
4. constrain stereo-relative phase behavior in the correction band
5. add objective metrics for pre-ringing and correction effort

Exit criteria:

- mixed-phase correction improves low-band excess phase where data is stable
- stereo-relative behavior is preserved
- transient behavior is not materially worsened

## Phase 5: Validation Workflow

Goal:

- turn correction into a measurable before/after process

Tasks:

1. add post-correction remeasurement
2. compare before/after:
   - excess phase
   - group delay
   - inter-channel mismatch
   - correction effort
   - pre-ringing index
3. persist validation results next to exported filters

Exit criteria:

- every mixed-phase export can be tied to objective validation data
- Wolfie can reject correction settings that improve one metric while harming stereo behavior or transient response

## Final Assessment

For full-range stereo loudspeakers, Wolfie is already respectable as a minimum-phase correction tool.

Its mixed-phase path is not yet wrong in spirit, but it is still too dependent on weak analysis inputs and heuristic reconstruction steps. The strongest conclusion from this review is:

**Wolfie should not try to become smarter by correcting more phase until it becomes stricter about what phase data is actually meaningful.**

The next development step should therefore be:

1. fix the baseline target anchoring regression
2. add reference-aware transfer-function analysis
3. separate direct-path delay alignment from excess-phase correction
4. rebuild the mixed-phase path on top of that stronger analysis contract

That path is incremental, fits Wolfie's current architecture, and matches the state-of-the-art direction described in `deep-research-report.md`.
