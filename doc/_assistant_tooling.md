# Assistant Tooling

This note captures a pragmatic direction for Wolfie: add assisting tools that help the user make better mixed-phase decisions, without pretending that a fully automatic optimizer can always outperform a careful manual workflow.

The goal is decision support:

- make the tradeoffs visible
- make risky settings obvious
- shorten the iteration cycle
- preserve user control

## Design Stance

For Wolfie as a personal tool, the highest-value additions are not a black-box optimizer but a set of diagnostics and helper workflows around the existing minimum and mixed-phase paths.

These helpers should answer questions like:

- did mixed mode improve anything meaningful beyond the excess-phase plot?
- did it damage bass magnitude versus the minimum-phase baseline?
- did it push too much energy into pre-ringing or late tail?
- did it weaken stereo consistency to help one channel?
- what settings are worth listening to next?

## Suggested Tools

### 1. Minimum vs Mixed Delta View

Purpose:

- show only what changed when moving from `minimum` to `mixed`

Useful deltas:

- corrected-response delta
- predicted group-delay delta
- excess-phase delta
- filter impulse delta
- late-tail or pre-ringing delta

How to use:

1. Design a good minimum-phase baseline.
2. Switch to mixed phase.
3. Inspect only the delta view.
4. Accept mixed mode only if the low-frequency timing improvement is clear and the magnitude drift is small.

Why it helps:

- it removes visual clutter
- it makes "improvement versus baseline" the primary question

### 2. Mixed-Phase Guardrail Panel

Purpose:

- summarize whether a mixed-phase setting looks controlled or risky

Possible checks:

- low-frequency magnitude drift versus minimum
- low-frequency predicted group-delay improvement
- excess-phase reduction below the mixed-phase limit
- pre-peak energy ratio
- late-tail growth ratio
- stereo impulse-peak mismatch
- low-frequency inter-channel phase mismatch

Output style:

- pass / warn / fail
- short reason text for each warning

How to use:

- treat it as a rejection filter
- if several warnings appear, reduce `mixedPhaseStrength`, `mixedPhaseMaxFrequencyHz`, or `mixedPhaseMaxCorrectionDegrees`

Why it helps:

- it turns several specialist plots into a quick go/no-go summary

### 3. Parameter Sweep Table

Purpose:

- explore a small parameter grid automatically and rank the outcomes

Good sweep dimensions:

- `mixedPhaseMaxFrequencyHz`: `160 / 200 / 250 / 300`
- `mixedPhaseStrength`: `0.25 / 0.5 / 0.75 / 1.0`
- `mixedPhaseMaxCorrectionDegrees`: `90 / 180 / 360`

Metrics per candidate:

- low-frequency excess-phase reduction
- low-frequency predicted group-delay reduction
- low-frequency magnitude drift versus minimum
- pre-ringing metric
- late-tail metric
- stereo preservation metric

How to use:

1. Keep the minimum-phase baseline fixed.
2. Run a small sweep over mixed-phase settings.
3. Sort by balanced improvement, not by maximum phase reduction.
4. Listen only to the top few candidates.

Why it helps:

- it gives semi-automation without giving up judgment
- it avoids repeatedly hand-entering similar settings

### 4. Low-Frequency Trust / Risk Overlay

Purpose:

- highlight regions that look risky to correct aggressively

Possible risk cues:

- narrow notch shape
- steep local correction effort
- unstable-looking local phase structure
- large required phase rotation
- large difference between minimum and mixed predicted magnitude

How to use:

- if mixed phase is acting mostly in high-risk zones, back off
- use it to justify keeping the mixed-phase limit lower

Why it helps:

- it gives visual context for why some measured structure should not be chased

### 5. Impulse Quality Meter

Purpose:

- turn the impulse plot into explicit metrics

Candidate metrics:

- pre-peak energy ratio
- late-tail energy ratio
- peak compactness
- peak latency shift versus minimum
- center-of-energy shift

How to use:

- if excess phase improves but impulse metrics get worse, reject or reduce the setting

Why it helps:

- it makes time-domain damage easier to spot
- it reduces dependence on visually interpreting every impulse trace by eye

### 6. Waterfall Delta View

Purpose:

- show where decay actually improved or worsened after filtering

Possible outputs:

- before/after difference waterfall
- decay-improvement trace by frequency
- summary of best and worst bands

How to use:

- mixed phase should earn its cost mainly in the bass
- if the waterfall delta is near neutral, the excess-phase improvement may be mostly visual

Why it helps:

- it keeps the focus on decay improvement rather than plot cosmetics

### 7. Stereo Preservation Meter

Purpose:

- detect cases where one channel improves but stereo coherence gets worse

Candidate checks:

- left/right impulse-peak delta
- low-frequency inter-channel phase delta before and after
- predicted group-delay mismatch before and after
- left/right correction asymmetry

How to use:

- reject settings that make stereo mismatch worse for small single-channel gains

Why it helps:

- it protects image stability and crossover behavior

### 8. Workspace Snapshots and A/B Memory

Purpose:

- make comparison fast and reproducible

Useful capabilities:

- save named filter-design candidates
- restore previous settings instantly
- compare two saved candidates side by side

Example names:

- `min-baseline`
- `mixed-200Hz-0.5`
- `mixed-250Hz-180deg`

How to use:

- keep one trusted baseline
- compare experimental settings against it
- listen only to the best few saved candidates

Why it helps:

- it makes exploration safer
- it prevents losing a good hand-tuned state

### 9. Plain-Language Explanations

Purpose:

- explain why Wolfie is warning or recommending something

Example messages:

- "Mixed mode reduced excess phase, but total low-frequency group delay changed only slightly."
- "Late-tail energy increased strongly versus minimum phase."
- "Low-frequency corrected response drifted too far from the minimum-phase baseline."
- "Stereo timing mismatch increased after mixed-phase alignment."

How to use:

- use the text to understand the consequence of a setting, not only its numeric result

Why it helps:

- it makes the tool teachable instead of opaque

### 10. Conservative Starting Presets

Purpose:

- provide sensible entry points before fine tuning

Candidate presets:

- `Bass Safe`
- `Balanced`
- `Experimental`

Expected behavior:

- `Bass Safe`: lower mixed-phase limit, moderate smoothing, conservative phase cap
- `Balanced`: moderate mixed-phase limit and strength
- `Experimental`: wider low-frequency band and stronger phase settings

How to use:

- start safe
- move upward only when the diagnostics justify it

Why it helps:

- it shortens setup time
- it keeps first attempts away from unstable settings

## Recommended Implementation Order

If only a few tools are added first, build them in this order:

1. Minimum vs Mixed Delta View
2. Mixed-Phase Guardrail Panel
3. Parameter Sweep Table

Why this order:

- the delta view clarifies whether mixed mode changed anything useful
- the guardrail panel turns expert judgment into explicit checks
- the parameter sweep table speeds up exploration without requiring a full optimizer

## Suggested User Workflow

1. Build a trusted minimum-phase baseline.
2. Enable mixed phase conservatively.
3. Compare `minimum` and `mixed` in the delta view.
4. Read the guardrail panel.
5. If the result looks promising, try a small sweep.
6. Listen only to the top few candidates.
7. Save the accepted candidate as a snapshot.

## Principles To Preserve

- keep the user in control
- prefer conservative recommendations over aggressive optimization
- evaluate mixed phase against the minimum-phase baseline, not in isolation
- treat impulse behavior, waterfall behavior, and stereo consistency as load-bearing checks
- avoid presenting cosmetic phase improvement as a success on its own

## Short Version

The best next step for Wolfie is not a fully automatic mixed-phase optimizer.

It is a set of assisting tools that:

- compare mixed against minimum clearly
- warn when mixed mode causes side effects
- help search a small parameter space efficiently
- preserve manual judgment as the final decision maker
