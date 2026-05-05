# Parameter Heuristics

This note captures pragmatic heuristics for tuning Wolfie's filter-design parameters without pretending that a single-seat room measurement is globally trustworthy.

The intended use is conservative:

- fix broad bass problems
- avoid chasing narrow cancellations
- keep mixed-phase correction limited and smooth
- judge settings from realized behavior, not from a prettier phase trace alone

## Recommended Plots

The most useful plots for parameter tuning are:

1. Inversion
2. Predicted Corrected Response
3. Excess Phase
4. Filter + Predicted Group Delay
5. Filter Impulse
6. Waterfall
7. Stereo Diagnostics

Use them in that order. Tune magnitude first, then phase, then validate time behavior and stereo behavior.

## Global Heuristics

- Prefer broad, smooth correction over exact target matching.
- Treat low-frequency room effects as worth shaping, not exactly inverting.
- Do not attack narrow nulls with boost.
- Treat high-frequency phase detail as low-trust unless repeated measurements prove otherwise.
- Mixed phase should earn its cost by improving low-frequency timing while keeping the FIR impulse compact.
- In stereo, "slightly less corrected but matched" is better than "more corrected but skewed."

## Practical Starting Defaults

These are starting points, not laws:

- `maxBoostDb = 0.0`
- `maxCutDb = 12.0`
- `mixedPhaseMaxFrequencyHz = 200.0` to `250.0`
- `mixedPhaseStrength = moderate`, not maximum
- `mixedPhaseMaxCorrectionDegrees = conservative`
- smoothing high enough that the inversion stays broad and non-toothy

If the room is unusually well behaved and the listening position is well validated, mixed phase can sometimes extend toward `300 Hz`. That should be a deliberate choice, not the default.

## Plot-Specific Heuristics

### Inversion

Purpose:

- judge whether the requested correction is sane

Good signs:

- broad shapes
- mostly cuts, not aggressive boost
- no comb-like detail

Warning signs:

- narrow spikes
- attempts to fill deep nulls
- rapid alternation between boost and cut

Adjustments:

- increase smoothing if the inversion is too detailed
- reduce boost if the filter is trying to fill nulls
- narrow the correction band if the curve gets busy at the edges

### Predicted Corrected Response

Purpose:

- judge whether the filter improves the broad response trend

Good signs:

- broad peaks are reduced
- the response moves toward target without looking overfit
- mixed mode stays close to minimum mode in magnitude

Warning signs:

- the curve hugs the target only by creating detailed structure
- mixed mode damages low-frequency magnitude compared with minimum mode
- target matching improves on paper while the other diagnostics get worse

Adjustments:

- accept broad improvement instead of perfect target matching
- ignore narrow dips the room is unlikely to support correcting
- compare `minimum` and `mixed`; if mixed costs obvious bass magnitude for minor phase gain, back off mixed-phase settings

### Excess Phase

Purpose:

- judge whether mixed phase is reducing broad low-frequency excess phase in the intended band

Good signs:

- broad low-frequency reduction
- little change above the mixed-phase limit
- no violent new swings

Warning signs:

- large changes above the intended low-frequency region
- the trace gets more complicated rather than simpler
- apparent improvement only at frequencies where the room response is obviously unstable

Adjustments:

- care mostly below about `200 Hz` to `250 Hz`
- treat `300 Hz` as an upper pragmatic limit, not a target
- if raising `mixedPhaseMaxFrequencyHz` changes a lot outside bass, reduce it
- if increasing `mixedPhaseStrength` flattens the phase trace but hurts impulse behavior, reduce it

### Filter + Predicted Group Delay

Purpose:

- judge whether the filter is reducing broad low-frequency timing error in a controlled way

Good signs:

- smoother, lower predicted low-frequency group delay
- improvement concentrated in the chosen low-frequency band

Warning signs:

- oscillatory ripple
- sharp new delay spikes
- visible complexity added for little net improvement

Adjustments:

- prefer broad low-frequency reduction over perfect flattening
- if stronger mixed-phase settings mainly add ripple, back off

### Filter Impulse

Purpose:

- validate that the realized FIR stays physically well behaved

Good signs:

- one clear main peak
- compact energy around the peak
- limited pre-ringing and limited late tail

Warning signs:

- long front tail
- obvious pre-ringing before the peak
- too much energy far after the peak
- large latency growth for small audible benefit

Adjustments:

- reduce `mixedPhaseStrength`
- lower `mixedPhaseMaxFrequencyHz`
- reduce `mixedPhaseMaxCorrectionDegrees`
- prefer the cleaner impulse even if the excess-phase plot looks less flat

### Waterfall

Purpose:

- validate that the correction improves low-frequency decay, not only steady-state magnitude

Good signs:

- shorter lingering low-frequency ridges
- less persistent bass energy after correction

Warning signs:

- corrected magnitude looks flatter but decay barely improves
- new trailing structure appears

Adjustments:

- mixed phase should be justified by improved low-frequency decay behavior
- if the waterfall does not improve, stronger phase correction may not be earning its complexity

### Stereo Diagnostics

Purpose:

- verify that one channel is not being improved at the expense of the stereo image

Good signs:

- left/right delay relationship stays controlled
- low-frequency stereo phase relationship stays close to the minimum-phase baseline
- no new channel latency split

Warning signs:

- one channel improves while inter-channel mismatch grows
- impulse peak timing diverges
- low-frequency phase relationship becomes less stable

Adjustments:

- prefer matched left/right behavior over slightly stronger single-channel correction
- if mixed phase harms stereo consistency, reduce the phase settings even if one channel's plots look better

## Parameter-by-Parameter Heuristics

### `smoothness`

Increase it when:

- inversion becomes spiky
- corrected response looks overfit
- mixed-phase behavior starts reacting to narrow room structure

Decrease it when:

- the correction is too blunt to reduce obvious broad bass peaks

Rule:

- err on the smooth side unless repeated measurements show that a feature is stable

### `tapCount`

Increase it when:

- the current settings are conservative and you still want smoother low-frequency realization
- the impulse stays compact and the waterfall suggests more low-frequency control is useful

Do not increase it just because:

- a prettier phase trace is possible
- you want to chase narrow detail

Rule:

- more taps should buy cleaner low-frequency realization, not more ambition

### `maxBoostDb`

Default:

- keep it at `0 dB` unless there is a specific reason not to

Raise it only when:

- the feature is broad
- the target shift is intentional
- the response deficit is not a narrow cancellation

Rule:

- never use boost to fill deep narrow nulls

### `mixedPhaseMaxFrequencyHz`

Recommended default:

- `200 Hz` to `250 Hz`

Raise it only when:

- the room and speakers are well behaved
- the impulse stays compact
- group-delay improvement remains broad and controlled

Rule:

- `300 Hz` is a pragmatic upper bound for most single-seat usage, not a default target

### `mixedPhaseStrength`

Use it as:

- a restraint control, not a courage control

Raise it when:

- low-frequency excess phase remains broad and obviously correctable
- impulse and waterfall still look controlled

Reduce it when:

- impulse pre-ringing grows
- late-tail energy grows
- bass magnitude drifts away from the minimum-phase result

Rule:

- stop increasing it once the remaining improvement is mostly cosmetic

### `mixedPhaseMaxCorrectionDegrees`

Purpose:

- prevent excessive phase inversion even when the measured trace is large

Raise it only when:

- the low-frequency excess phase is broad and persistent
- the realized impulse remains compact
- minimum and mixed magnitude stay close

Reduce it when:

- mixed mode starts pushing too much energy into the impulse tail
- low-frequency magnitude departs too much from the minimum-phase result

Rule:

- use the cap to keep mixed phase bounded, especially in the presence of room-driven swings

## Simple Tuning Workflow

1. Start with minimum phase and conservative magnitude settings.
2. Make the inversion broad and sane.
3. Enable mixed phase with a low-frequency limit near `200 Hz` to `250 Hz`.
4. Watch excess phase and predicted group delay only in the low end.
5. Validate with filter impulse and waterfall.
6. Check stereo diagnostics before accepting the result.

## Short Version

Trust broad low-frequency improvements that:

- keep the inversion smooth
- preserve magnitude behavior versus minimum phase
- reduce low-frequency timing error
- keep the impulse compact
- do not worsen stereo consistency

Ignore:

- narrow null chasing
- exact target matching
- pretty-looking high-frequency phase traces
- improvements that exist only in one plot
