# Smooth Excess-Phase Fadeout

This note captures the current state of investigation into mixed-phase pre-ringing, the facts confirmed during implementation, and the intended next steps.

The goal is to let a new session continue without reconstructing the reasoning from chat history.

## Problem Statement

Mixed-phase correction can improve low-frequency timing, but in some workspaces it introduces visible and audible pre-ringing in the filter impulse while minimum-phase remains benign.

The important question is not just "where is the pre-ringing in the impulse?" but "what in the mixed-phase target creation is causing the filter to ask for anti-causal energy?"

## What Was Implemented

### 1. Manual Pre-Ringing Compensation Probe

A first experimental control was added:

- a `Pre-Ring Spots` field with space- or comma-separated integer frequencies
- a `Pre-Ring Strength` slider

This is wired through:

- `src/core/models.h`
- `src/persistence/workspace_repository.cpp`
- `src/persistence/filter_store_repository.cpp`
- `src/ui/filters_page.cpp/.h`
- `src/measurement/filter_designer.cpp`

Behavior:

- the listed frequencies are normalized into the mixed-phase correction range
- mixed-phase correction is locally backed off around those frequencies
- this visibly damps the predicted excess-phase trace near those spots

Conclusion:

- useful as a manual probe
- not the right long-term fix
- it patches a symptom downstream instead of addressing the target-construction cause upstream

### 2. Requested Mixed Group Delay Chart

A new standalone chart was added to the Filters page:

- `Requested Mixed Group Delay`

This chart is separate from the existing `Group Delay` chart.

It shows the group delay implied by the requested mixed-phase correction before FIR realization.

That data is now published as:

- `FilterDesignChannelResult::requestedMixedGroupDelayMs`

and persisted in stored filters.

This was implemented so we can distinguish:

1. prepared input phase structure
2. requested mixed-phase correction structure
3. realized filter behavior

without conflating them.

## Key Facts Learned

### A. The original "ring frequency from the impulse" method was incomplete

Measuring the oscillation wavelength before the main pulse gives the carrier frequency of the ringing packet, but not:

- the responsible correction bandwidth
- the shoulder shape of the mixed-phase target
- whether the ringing comes from the mixed-phase target or its realization

Conclusion:

- the measured ring frequency is only a rough center-frequency hint
- it is not sufficient as the primary design/debug signal

### B. Mixed-phase is the source, not minimum-phase

This is now a working assumption supported by observation:

- minimum-phase filter impulse is benign
- mixed-phase filter impulse shows the problematic pre-ringing

Conclusion:

- debugging effort should focus on mixed-phase target construction and realization
- not on the general magnitude-correction path

### C. The new requested-group-delay chart exposed a home-made structure

The requested mixed group-delay chart showed a hump that moved with the configured `Phase Limit` (`mixedPhaseMaxFrequencyHz`).

This was the most important finding of the session.

Conclusion:

- that hump is not primarily a property of the measured input phase
- it is being created by the mixed-phase limiter/taper itself

### D. Broadening the upper mixed-phase taper helped

The old mixed-phase upper taper was relatively short.

It was changed to a broader fadeout:

- previously the upper taper was much narrower
- now it extends farther above `mixedPhaseMaxFrequencyHz`

Observed result:

- the requested mixed group-delay hump became broader and flatter
- the pre-ringing impulse size decreased

Conclusion:

- the limiter transition was indeed contributing to the pre-ringing
- smoother fadeout is directionally correct

### E. The requested mixed group-delay chart still shows sharp edges after the phase limit

After broadening the taper, the requested mixed group-delay still has visible edge-like features near and just above the phase limit.

Conclusion:

- the current approach still creates boundary-shaped structure
- broadening the taper reduced the severity but did not remove the underlying mechanism

## Why The Sharp Edges Happen

This is the current DSP interpretation and should be treated as the working model.

### The mixed-phase target is still being shaped in phase-space

Current path:

1. prepare excess phase from the measurement
2. build a requested excess-phase correction in `buildExcessPhaseCorrectionDegrees(...)`
3. apply a frequency weight tied to `mixedPhaseMaxFrequencyHz`
4. solve a regularized curve
5. shape/limit the result again with taper and cap
6. convert to phase radians
7. realize as FIR

The important detail is that the upper mixed-phase boundary is encoded as a phase-space fadeout.

### Group delay is the derivative of phase

The new chart is showing requested group delay, which is proportional to the derivative of the requested phase target with respect to frequency.

That means:

- a boundary in the phase target
- a shoulder in the taper
- a bend introduced by a cap

becomes a localized structure in the requested group-delay plot.

Conclusion:

- the sharp edge in requested group delay is not surprising
- it is the derivative signature of the current boundary-shaped phase fadeout

### The current implementation effectively double-shapes the mixed-phase target

In the current logic:

- the taper influences the desired target before the solve
- then the result is multiplied/limited again after the solve

This makes the requested target follow the limiter envelope more directly than ideal.

Conclusion:

- the current limit behavior is more than just a permissive bound
- it is an active source of structure in the request

## Important Conclusion

The main remaining issue is not "find the right pre-ringing frequency."

The main issue is:

- the current mixed-phase limit is implemented as a phase-space fadeout
- that fadeout creates a synthetic group-delay hump/edge
- the FIR then realizes that structure as pre-ringing risk

So the root cause is upstream in target construction.

## Current Code Areas To Revisit

The most relevant files/functions are:

- `src/measurement/filter_designer.cpp`
  - `excessPhaseCorrectionWeightAt(...)`
  - `buildExcessPhaseCorrectionDegrees(...)`
  - `buildPositivePhaseCorrectionRadians(...)`
  - `buildMixedPhaseImpulse(...)`
  - requested mixed group-delay publication

- `src/measurement/phase_preparation.cpp`
  - phase-window behavior still matters because it affects what mixed mode is asked to correct

- `src/ui/filters_page.cpp/.h`
  - requested mixed group-delay chart is now available and should be kept

## What The Existing Charts Mean

### Existing `Group Delay` chart

This is downstream.

It mainly shows:

- input group delay
- realized filter group delay
- predicted total group delay

It is useful, but it does not isolate the mixed-phase request itself.

### New `Requested Mixed Group Delay` chart

This is upstream.

It shows:

- the requested incremental timing correction before FIR realization

This is the right chart for debugging:

- home-made limiter structures
- sharp shoulders
- narrow timing demands

## Diagnostics Added This Session

Process log additions:

- mixed-phase limit span:
  - "full strength through X Hz and tapers out by Y Hz"
- requested mixed group-delay peak:
  - currently global per channel

Important caveat:

- the current requested-group-delay peak log is too crude
- it may lock onto an unrelated hump far away from the limiter transition
- it is not yet the right metric for the phase-limit artifact specifically

## Diagnostics Still Needed

### 1. Transition-band diagnostic, not global peak

Replace the current "largest absolute requested group-delay peak anywhere" with a boundary-focused diagnostic.

Suggested metrics inside the upper mixed-phase transition band:

- peak requested group delay in `0.7 * phaseLimit .. taperEnd`
- RMS requested group delay in that band
- signed area in that band
- center frequency of the strongest hump in that band

This should answer:

"How much structure is the phase-limit transition itself creating?"

### 2. Separate pre-solve and post-solve target diagnostics

It would be useful to visualize or log:

- pre-solve requested phase/group-delay target
- post-solve requested phase/group-delay target

This would tell how much of the hump comes from:

- the limiter envelope itself
- the regularized solve
- the cap application

### 3. Better phase-window sensitivity check

The next session should compare the requested mixed group-delay chart across different `Phase Window` values.

Purpose:

- if the request changes strongly with `Phase Window`, the source phase is low-trust
- if the request stays stable but ringing remains, the problem is more in the limiter/realization path

## Preferred Design Direction

The preferred long-term fix is:

### Build the mixed-phase target in incremental group-delay space, then integrate back to phase

This is preferred over only broadening the current phase-space fadeout.

Reason:

- the artifact is visible directly in requested group delay
- the current phase-space boundary creates derivative artifacts
- group-delay-space targeting works on the quantity that is actually causing the trouble

Expected benefits:

- smoother requested timing correction
- better control over localized humps
- less accidental boundary-created structure
- more direct regularization of "timing demand"

### Why not stop at a broader phase-space taper?

A broader taper is a valid probe and already helped.

But it is still indirect symptom management because:

- the design object remains phase
- the problem reveals itself in group delay

Broader taper is useful as an intermediate step, not the final architecture.

## Implementation Targets For The Next Session

### Immediate diagnostic targets

1. Replace the current global requested-group-delay peak log with a transition-band diagnostic.
2. Log the transition-band metrics for both channels.
3. Optionally show a vertical marker or shaded span for:
   - `mixedPhaseMaxFrequencyHz`
   - transition end frequency

### DSP cleanup targets inside the current phase-space model

1. Remove or reduce the double-shaping effect around the upper mixed-phase fadeout.
2. Revisit whether the taper should:
   - only weight the solve
   - only clamp the result
   - or use separate pre/post weighting rules
3. Keep the broader fadeout for now because it improved the impulse behavior.

### Preferred refactor target

Move from:

- requested excess-phase correction directly in phase-space

to:

- requested incremental group-delay correction
- regularized and tapered in group-delay space
- integrated back to phase before FIR realization

### Suggested staged rollout

1. Keep current broader taper.
2. Improve limiter-transition diagnostics.
3. Reduce double-shaping.
4. Prototype group-delay-space target construction behind the same mixed-mode UI.
5. Compare:
   - requested mixed group-delay chart
   - filter impulse pre-ringing
   - predicted excess phase
   - predicted group delay

## Acceptance Criteria For Future Work

The next implementation should be judged by realized behavior, not only by prettier curves.

Good signs:

- requested mixed group-delay transition becomes smoother
- the limiter no longer creates a sharp hump that tracks `Phase Limit`
- pre-ringing impulse magnitude decreases further
- mixed-mode still provides useful LF timing improvement
- minimum vs mixed magnitude behavior remains sane

Warning signs:

- requested mixed group-delay still shows narrow edge-like structure at the phase limit
- broader taper only moves the problem instead of reducing it
- realized impulse still shows strong pre-peak energy despite a smoother request
- low-frequency timing improvement collapses completely

## Summary

The most important result of the session is this:

- the new requested mixed group-delay chart exposed a structure that tracks the configured phase limit
- therefore the current mixed-phase limiter is itself creating timing structure
- broadening the fadeout reduced both the hump severity and the pre-ringing impulse size
- the remaining sharp edges indicate that the root problem is still the phase-space boundary construction

The preferred next step is:

- improve diagnostics around the limiter transition
- then move mixed-phase target construction toward smooth incremental group-delay control

Do not remove the new chart. It is the load-bearing diagnostic for this line of work.
