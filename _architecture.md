# Wolfie Architecture

## Intent

`wolfie` is organized around a simple rule: the Win32 app shell should coordinate the workflow, not implement the workflow.

The codebase stays maintainable when these concerns remain separate:

- domain calculations
- audio and driver access
- persistence and file formats
- reusable plotting and page UI
- top-level application composition

The goal is local changeability, not abstraction for its own sake.

## Design Principles

- Keep Win32 UI code at the edge.
- Keep measurement and filter math independent from platform APIs.
- Centralize persistence so pages and dialogs do not parse files directly.
- Keep reusable graphs generic; they should consume prepared view data, not application state.
- Prefer small, explicit data contracts in `src/core` over ad hoc coupling between modules.
- Let `WolfieApp` compose modules and move data between them, but not grow back into a monolith.

## Module Overview

### `src/core`

Purpose: shared data contracts and basic text helpers.

Files:

- `models.h`
- `text_utils.h/.cpp`

Responsibilities:

- Define shared types such as `WorkspaceState`, `MeasurementResult`, `SmoothedResponse`, `FilterDesignSettings`, and `FilterDesignResult`.
- Carry the data exchanged between measurement, persistence, UI, and app-shell code.
- Provide UTF-8 / UTF-16 conversion and numeric formatting helpers.

Reasoning:

- These types are the seam between modules.
- Keeping them centralized prevents each subsystem from inventing slightly different versions of the same state.

### `src/measurement`

Purpose: measurement-domain logic.

Files:

- `sweep_generator.h/.cpp`
- `response_analyzer.h/.cpp`
- `room_simulator.h/.cpp`
- `sweet_spot_alignment.h/.cpp`
- `measurement_controller.h/.cpp`
- `target_curve_designer.h/.cpp`
- `filter_designer.h/.cpp`
- `stereo_diagnostics.h/.cpp`
- `waterfall_builder.h/.cpp`

Responsibilities:

- `sweep_generator` creates playback sweep data and exported sweep WAV content.
- `response_analyzer` turns captured audio into `MeasurementResult` value sets and analysis metadata, including room transfer products, optional reference-compensated room variants, and room/raw impulse responses.
- `room_simulator` generates synthetic room-response `MeasurementResult` data for UI and filter-testing workflows without audio I/O.
- `sweet_spot_alignment` derives mic-centering guidance and pulse-overlay view data from measured timing metadata and the room impulse response, with its own local peak focus.
- `measurement_controller` orchestrates room and reference measurement runs through an audio backend.
- `target_curve_designer` computes target-curve view data without UI dependencies.
- `filter_designer` computes correction curves, FIR filters, mixed-phase phase shaping, simulated responses, phase-derived diagnostics, and filter-design view data without UI dependencies.
- `stereo_diagnostics` computes left/right comparison diagnostics and analysis-plot data from measured transfer functions without UI dependencies.
- `waterfall_builder` derives waterfall data from measured impulse responses and predicted post-filter impulse responses.

Reasoning:

- Signal analysis, target evaluation, and filter design are domain logic.
- UI code should not own FFTs, smoothing, phase handling, or filter prediction rules.

Filter-design shaping rules:

- When inversion reduces measured peaks, do not force the predicted response to sit exactly on or below the target at every point.
- Prefer a smooth, tapered correction shape over literal target tracking when those goals conflict.
- It is acceptable for the predicted response to remain slightly above the target through a reduced peak if that avoids a sharper underswing after the cut.
- Avoid hard clamp behavior in the magnitude correction path; abrupt cut enforcement can trade a cleaner magnitude trace for worse phase and group-delay behavior.
- Correction limits and solver targets should soften into the boundary so realized filters stay well-behaved in both magnitude and time-domain diagnostics.

### `src/audio`

Purpose: audio I/O and driver-related platform services.

Files:

- `audio_backend.h`
- `asio_sdk.h/.cpp`
- `asio_audio_backend.h/.cpp`
- `wasapi_audio_backend.h/.cpp`
- `wasapi_service.h/.cpp`
- `asio_service.h/.cpp`

Responsibilities:

- Define audio-session abstractions.
- Implement playback and capture through WASAPI or ASIO.
- Enumerate Windows audio endpoints for the settings UI.
- Handle driver enumeration and ASIO control-panel access.

Reasoning:

- Audio device access is platform-specific and operationally fragile.
- The measurement workflow should depend on an interface, not on direct device API calls.

### `src/persistence`

Purpose: workspace and app-state serialization.

Files:

- `workspace_repository.h/.cpp`
- `app_state_repository.h/.cpp`
- `room_simulation_repository.h/.cpp`

Responsibilities:

- Load and save workspace files and recent-workspace state.
- Load and save named room-simulation parameter files under the workspace.
- Persist measurement value sets, workspace settings, and app-level state.

Reasoning:

- Pages and dialogs should not parse structured files.
- File-format rules belong in one place so compatibility changes stay contained.

### `src/ui`

Purpose: reusable UI widgets and page modules.

Files:

- `ui_theme.h`
- `response_graph.h/.cpp`
- `plot_graph.h/.cpp`
- `measurement_page.h/.cpp`
- `alignment_page.h/.cpp`
- `analysis_page.h/.cpp`
- `room_simulation_dialog.h/.cpp`
- `target_curve_graph.h/.cpp`
- `target_curve_page.h/.cpp`
- `filters_page.h/.cpp`
- `settings_dialog.h/.cpp`
- `waterfall_graph.h/.cpp`

Responsibilities:

- `ui_theme` defines shared colors and visual constants.
- `response_graph` renders reusable response displays for measurement workflows.
- `plot_graph` renders reusable non-interactive plots for filter-design workflows.
- `measurement_page`, `analysis_page`, `target_curve_page`, and `filters_page` own their controls, layout, legend state, and graph synchronization.
- `alignment_page` owns the dedicated mic-alignment workflow, including the alignment action, timing summary, and pulse overlay.
- `analysis_page` presents left/right comparison diagnostics such as delay mismatch, impulse correlation, phase delta, and magnitude delta using prepared measurement data.
- `room_simulation_dialog` owns the non-modal synthetic-room editor and delegates generation/persistence outward.
- `target_curve_graph` and `waterfall_graph` implement specialized graph behavior for their workflows.
- `settings_dialog` owns settings-window behavior and delegates ASIO-specific work outward.

Reasoning:

- Widgets should render supplied data, not reach into `WolfieApp`.
- Page modules should own page behavior so the app shell stays focused on coordination.

### UI Theming

The shared UI theme lives in `src/ui/ui_theme.h`.

Current design rules:

- Use the system button-face color for page and dialog backgrounds so the app keeps a native Win32 surface.
- Use the standard window/input surface for editable fields such as `EDIT` and editable parameter controls so active inputs do not read as disabled.
- Treat charts as distinct work surfaces with a white background, even when the surrounding page is gray.
- Reserve tinted or gray surfaces for page chrome, frames, non-editable status areas, and passive labels rather than live input fields.
- Use the shared in-dialog help bubble pattern for parameter help instead of hover-only tooltip popups when the UI needs explanatory text for calculation settings.
- New calculation input parameters should get a help tooltip/bubble by default unless the field is genuinely self-evident and does not benefit from extra explanation.
- Keep shared UI colors such as border, accent, muted text, and graph overlay colors centralized in `ui_theme` rather than duplicated inside pages or widgets.
- Reusable graph widgets should obtain their background and overlay colors through `ui_theme` helpers such as `graphBackgroundColor()` and `graphBackgroundBrush()`.
- Page modules may decide layout and visibility, but they should not invent page-local color schemes when an existing shared theme value fits.

Why this split exists:

- The gray application chrome keeps the rest of the UI aligned with native controls.
- White input surfaces preserve the native affordance that editable controls are active and focusable.
- White chart surfaces improve trace contrast, grid readability, and visual separation between control areas and analysis views.
- Press-and-hold help bubbles are more reliable in this Win32 surface than transient hover balloons and make parameter semantics discoverable without leaving the workflow.
- Centralizing theme values keeps cosmetic changes local and avoids drift between `response_graph`, `plot_graph`, `target_curve_graph`, and `waterfall_graph`.

### Win32 Scrolling And Paint Hygiene

The page modules use native Win32 child controls on scrolling surfaces. This is fragile because `ScrollWindowEx`, `WS_CLIPCHILDREN`, `WS_CLIPSIBLINGS`, native group boxes, owner-drawn controls, and static labels all participate in repaint ordering. Visual smearing during scroll is usually a paint invalidation problem, not a layout problem.

Good patterns:

- Controls that draw framed areas, colored swatches, graph legends, custom buttons, or panel-like regions must explicitly fill their whole client area before drawing content.
- Owner-drawn frame controls are preferred when a native control does not reliably erase the area it visually owns during page scrolling.
- When using `ScrollWindowEx`, include the invalidation/erase flags needed for the moved surface and repaint affected child controls whose native painting is known to be incomplete.
- Keep scroll handling in the owning page module. The page should own `contentHeight_`, scroll offset, scrollbar updates, child positioning, and repaint invalidation for its own controls.
- Use `WS_CLIPSIBLINGS` on overlapping child controls and `WS_CLIPCHILDREN` on parent pages deliberately, then verify that every exposed background area is still painted by either the parent or the child that owns it.
- If a control changes enabled state, visibility, z-order, or text color, invalidate the control or its containing area so stale pixels are not left behind.
- Prefer shared theme brushes and colors for erase/fill paths so repainted areas match the rest of the native page surface exactly.

Bad patterns:

- Do not rely on a native `BS_GROUPBOX` to erase the interior of a visual group on a scrolling page; it primarily draws a frame/title and can leave child-control trails behind.
- Do not fix smearing by adding broad layout churn, forced full-page relayouts, or unrelated control moves when the issue is stale pixels.
- Do not draw only borders, text, or glyphs in an owner-drawn control without first painting the background that control visually owns.
- Do not assume `WS_CLIPCHILDREN` solves repaint artifacts by itself. It can prevent the parent from repainting exactly the area that a child control fails to erase.
- Do not hide scroll artifacts with oversized invalidation unless a smaller target is not practical; repaint the affected control or region first.
- Do not add custom paint code into `WolfieApp` for page-local artifacts. The page that owns the controls should own the paint fix.

### `src/wolfie_app.*`

Purpose: application shell and composition root.

Responsibilities:

- Create the main window, tabs, menus, and top-level layout.
- Coordinate workspace loading and saving.
- Connect repositories, services, controllers, and page modules.
- Trigger derived-state refresh such as smoothing and filter design when needed.

Reasoning:

- `WolfieApp` is where modules are wired together.
- It should move data between modules, not reimplement module internals.

### `tests`

Purpose: focused native test executables for measurement-domain behavior.

Files:

- `test_harness.h`
- `filter_test_support.h/.cpp`
- `measurement_loopback_tests.cpp`
- `filter_design_basics_tests.cpp`
- `phase_preparation_tests.cpp`
- `excess_phase_mode_tests.cpp`
- `mixed_phase_tests.cpp`
- `filter_analysis_tests.cpp`
- `filter_export_tests.cpp`

Responsibilities:

- `test_harness` provides the small shared runner used by the custom native test executables.
- `filter_test_support` owns reusable synthetic fixtures and numeric helpers shared by filter-related tests.
- `measurement_loopback_tests` covers sweep playback planning, synthetic capture analysis, room-transfer publication, direct-impulse publication, reference compensation, latency retention, and waterfall generation.
- `filter_design_basics_tests` covers baseline filter-design behavior such as target evaluation, correction-shape behavior, and general result sanity.
- `phase_preparation_tests` covers bulk-delay removal, room-based phase preparation, phase-window behavior, and phase-preparation metadata/process logging.
- `excess_phase_mode_tests` covers the isolated `excess-lf` phase-correction mode.
- `mixed_phase_tests` covers the heavier mixed-phase design path, including correction limits, stereo alignment, and published phase diagnostics.
- `filter_analysis_tests` covers before/after stereo diagnostics derived from a designed filter result.
- `filter_export_tests` covers Roon WAV/config export behavior.

Reasoning:

- The test suite is intentionally split by behavior, not kept as one large executable.
- This keeps `ctest` output attributable when runtime regresses, especially for the heavy mixed-phase and export paths.
- Shared synthetic fixtures stay in `tests`, not in `src/measurement`, so production modules do not absorb test-only helpers.

Real-workspace regression rule:

- synthetic fixtures remain the first line of coverage for small, attributable DSP behavior
- when a regression is reported from an actual saved workspace and the synthetic tests do not reproduce it, verification must also include a persisted workspace from `workspaces/`
- the real-workspace check should load the workspace through `WorkspaceRepository`, rebuild `SmoothedResponse`, and rerun `measurement::designFilters(...)` with the saved `MeasurementResult`, smoothing settings, target curve, and filter settings
- for mixed-phase regressions, compare minimum and mixed mode on that real workspace using realized outputs such as `filterResponseDb`, `correctedResponseDb`, filter peak location, post-peak FIR level, and expected waterfall data rather than relying only on synthetic phase fixtures
- when the complaint is artifact level rather than basic correctness, also verify the same workspace across at least two tap counts so the acceptance check captures whether a larger FIR budget actually improves the realized result
- when the complaint is peak-cut overshoot or underswing around the target, acceptance should check more than target error alone: verify that the corrected response reduces the peak, does not swing materially below target, and does not require a hard-clamped correction shape to get there
- temporary probe executables are acceptable for diagnosis, but the lasting protection should end up in focused native tests and documented acceptance criteria rather than leaving ad hoc tooling in the tree

## Dependency Direction

The intended dependency flow is:

- `core` is shared by everything.
- `measurement` depends on `core` and audio abstractions.
- `audio` depends on `core` and platform APIs.
- `persistence` depends on `core`.
- `ui` depends on `core` and prepared view data.
- `wolfie_app` depends on all modules and composes them.
- `tests` depends on `core` and the measurement modules it exercises.

Practical examples:

- `response_graph` and `plot_graph` do not know about `WorkspaceState`.
- `workspace_repository` does not know about tabs or graph widgets.
- `measurement_controller` does not know how pages are drawn.
- `filter_designer` does not call Win32 APIs.
- `filter_test_support` may build synthetic `MeasurementResult` fixtures, but it does not become a production dependency.

## Persistence Timing

`WorkspaceRepository::save()` is the heavy full-workspace persistence path.

Strong warning:

- treat `WorkspaceRepository::save()` as forbidden from routine interactive UI event handlers
- a click on a checkbox, toggle, tab-local option, legend control, zoom preset, export sample-rate selector, or similar `workspace.ui` field must not trigger the full workspace save path
- if the edit only changes `workspace.ui`, the correct persistence path is `saveUiSettings()`, not `save()` and not broader settings persistence by habit
- if this boundary is violated, expect visible lag, unnecessary CPU and disk activity, and repeated regressions from code that "looks harmless" at the call site

Rules:

- do not call `WorkspaceRepository::save()` from interactive UI paths such as sliders, graph zoom handlers, hover updates, or live recalculation feedback
- do not use `WorkspaceRepository::saveSettings()` for UI-only state when `saveUiSettings()` is sufficient
- automatic full-workspace writes belong at app exit and workspace switch boundaries
- explicit user-driven save remains a separate action
- narrower persistence helpers such as settings-only or UI-only saves should be used for interactive edits that need persistence without rewriting measurement payloads
- in practice: `saveUiSettings()` is for `workspace.ui` edits; `saveSettings()` is for lighter non-measurement settings changes that are broader than UI state; `save()` is reserved for true full-workspace persistence

Reasoning:

- full workspace writes include much more than lightweight UI state
- even the narrower settings save path is too broad for pure UI toggles because it still rewrites more than the interaction changed
- calling the full save path from smoothing or filter controls causes avoidable stalls and regression-prone behavior
- export sample-rate selection is a canonical example of UI-only state and must stay on the UI-only persistence path
- this regression has happened repeatedly, so the timing boundary needs to stay explicit in the architecture document

## Runtime Flow

### Startup

1. `WolfieApp` loads app state through `AppStateRepository`.
2. `WolfieApp` creates the main window and page modules.
3. If possible, `WorkspaceRepository` loads the last workspace.
4. Pages populate from `WorkspaceState`.

### Measurement

1. `MeasurementPage` pushes edited settings into `WorkspaceState`.
2. `WolfieApp` starts `MeasurementController`.
3. `MeasurementController` drives sweep playback and capture through an audio backend.
4. `response_analyzer` produces `MeasurementResult`.
5. `WorkspaceRepository` persists the result.
6. `WolfieApp` can derive `SmoothedResponse` from the result when later workflows need it.

### Analysis

1. `WolfieApp` populates `AnalysisPage` from the current `MeasurementResult`.
2. `AnalysisPage` requests left/right comparison data from `measurement::stereo_diagnostics`.
3. `stereo_diagnostics` derives summary metrics and plot curves from room transfer data plus a locally focused room-impulse view when those inputs are available.

### Filter Design

1. `WolfieApp` ensures smoothed response data is available.
2. `WolfieApp` calls `measurement::designFilters(...)`.
3. `filter_designer` computes a `FilterDesignResult`.
4. `FiltersPage` builds chart data from `FilterDesignResult` and local legend-toggle state.

## Filter Design

This chapter captures the current architectural decisions for the filter-design workflow and its plots.

### Inputs And Outputs

The filter-design workflow consumes:

- `SmoothedResponse` for magnitude-domain source data
- `MeasurementSettings` and `TargetCurveSettings`
- `FilterDesignSettings`
- optionally `MeasurementResult` when phase-derived diagnostics are needed

The workflow produces `FilterDesignResult` in `src/core/models.h`. That result contains:

- display frequency axis
- aligned target curve
- correction curve per channel
- simulated filter response per channel
- predicted corrected response per channel
- filter group delay per channel
- excess-phase diagnostics per channel
- predicted group delay per channel
- filter taps and impulse metadata

Architectural rule:

- `filter_designer` owns the calculations that fill `FilterDesignResult`
- `filters_page` owns only chart assembly, legend toggles, and layout

### Current Filter Model

The current filter workflow is anchored by minimum-phase magnitude correction, with additional low-frequency phase handling layered on top.

Current phase modes:

- `minimum` builds a minimum-phase FIR from the magnitude correction curve
- `mixed` keeps the same magnitude-correction path and layers in bounded low-frequency excess-phase correction
- `excess-lf` exists as an internal phase-preview path for diagnostics and tests

Architectural consequences:

- magnitude correction still starts from `SmoothedResponse`
- prepared phase data comes from `MeasurementResult`
- minimum-phase reconstruction and bulk-delay removal happen before excess-phase interpretation
- mixed-mode phase work is intentionally constrained and stereo-aware rather than treated as full-band free-form inversion
- minimum-phase magnitude correction is cut-first: the inversion curve is capped at `0 dB` by default so the design reduces peaks without chasing troughs with boost, especially in the bass where boost quickly turns into excess phase rotation and group-delay ripple
- the default no-boost ceiling stays at exactly `0 dB` with no implicit headroom; if smoother shoulders are needed, they should come from the solver regularization, FIR tap budget, and any explicit user-selected `maxBoostDb` rather than from hidden overshoot above the stated boost limit

Final correction-shape principles:

- the stated boost limit is literal: `0 dB` means no boost, not "almost no boost" and not hidden positive headroom
- there is no implicit overshoot allowance above the configured boost ceiling
- if the inversion shape needs to stay smoother near the ceiling, that should be achieved by the regularized solve, the available FIR tap count, response smoothing, and any explicit user-selected `maxBoostDb`
- broad flat tops caused by a hidden limiter are not a desired outcome; if a smoother result needs some positive correction, that boost budget should be made explicit in settings
- in practice, raising tap count and allowing a small explicit boost budget can produce a better-shaped inversion than trying to fake smoothness with concealed headroom

In data-model terms:

- minimum mode usually leaves predicted excess phase close to the measured input excess phase
- mixed mode may reduce low-frequency excess phase while preserving the same general magnitude-correction workflow
- the excess-phase and group-delay plots are now part of the active filter-design workflow, not just placeholders

Mixed-phase regression policy:

- evaluate mixed-mode changes against both synthetic fixtures and at least one persisted real workspace when the reported failure involves audible artifacts, waterfall decay, or narrow low-frequency magnitude loss
- treat the realized FIR as the load-bearing artifact, not just the predicted plots: regressions may hide if only the correction target or averaged band metrics are inspected
- when balancing bass preservation against late artifacts, the acceptance check is the combined result on the real workspace: low-frequency corrected-response drift versus minimum phase must stay bounded, and the realized mixed FIR plus expected waterfall must improve in the expected direction when tap count is increased for that regression

### Mixed-Phase Filter Generation

The mixed-phase path is intentionally layered on top of the minimum-phase magnitude path instead of replacing it with full-band direct inversion.

Current generation pipeline:

1. `normalizeFilterDesignSettings()` snaps the FIR length to the supported tap counts, normalizes `phaseMode`, and clamps the mixed-phase controls before any solve begins.
2. `mixedPhaseMaxFrequencyHz` defines the full-strength correction band. The current implementation applies full weight through that limit and then tapers the mixed-phase request to zero by `2x` that frequency, subject to Nyquist-based clamps.
3. `preparePhaseData()` selects one matched magnitude/phase source pair for both channels. Selection order is: `reference_compensated_room` response, `reference_compensated_room` spectrum, `room` response, then `room` spectrum. Raw-only phase products are intentionally excluded from mixed-phase preparation.
4. The preparation step unwraps the measured phase, derives bulk delay from `measurement.room_impulse_response`, removes that linear delay, and optionally applies the user `Phase Window` by reconstructing a transfer impulse, extracting a local window around the dominant impulse, cosine-fading the edges, and transforming back to a transfer function.
5. The preparation step then smooths only the magnitude, rebuilds the corresponding minimum-phase curve from that smoothed magnitude, and defines excess phase as `delay-corrected measured phase - minimum phase`. This gives the mixed solver a phase-only residual instead of asking it to chase the full measured phase.
6. The prepared channel is resampled onto the filter display axis and publishes wrapped excess phase, continuous excess phase, and measured input group delay for plotting and diagnostics.

Mixed-phase correction design:

- `buildExcessPhaseCorrectionDesign()` converts the prepared excess phase into an excess group-delay request. The solver works in group-delay space because it is easier to constrain locally and easier to reason about in the low-frequency band than raw phase rotations.
- The request is weighted by the existing low-frequency correction envelope and by the mixed-phase limit band. Below the configured limit, mixed correction can run at full strength. In the transition band, the weight is tapered smoothly to zero. Above the taper end, mixed correction is disabled.
- `mixedPhaseStrength` scales the requested correction before the regularized solve. A value of `0` should collapse back to minimum-phase behavior.
- Pre-ringing compensation is applied as a local backoff multiplier around the user-listed center frequencies. It is intentionally narrowband and should reduce requested mixed correction mainly near those bands rather than reshaping the entire LF region.
- The requested group-delay curve is smoothed with the regularized solver instead of being applied literally. This is where the implementation trades exact target cancellation for a realizable, lower-ripple mixed-phase request.
- After the solve, bins outside the active band are forced back to zero. The solved group delay is then integrated back into phase.
- `mixedPhaseMaxCorrectionDegrees` is enforced after the solve by scaling the entire requested mixed correction when the largest weighted phase rotation exceeds the configured cap. The cap limits the realized LF phase swing without changing the general correction shape.

Stereo handling rules:

- Mixed mode normally designs per-channel correction from each channel's own prepared excess phase.
- When both channels show meaningful low-frequency excess phase and a meaningful left/right excess-phase delta, the implementation switches to a shared correction design computed as the average of the left and right requests. This preserves the stereo phase relationship instead of independently "fixing" each side toward a different LF timing target.
- After both mixed filters are realized, their impulse peaks are aligned to a shared latency by delaying the earlier peak to the later one and then recomputing the realized response diagnostics from the shifted FIRs. This keeps stereo timing consistent in the published result and in the exported taps.

FIR realization details:

- The magnitude side still comes from the normal correction curve and minimum-phase positive-magnitude reconstruction.
- Mixed mode then rotates that minimum-phase spectrum by the solved positive-frequency phase-correction curve and performs an inverse FFT to obtain a full mixed-phase impulse.
- The impulse is not taken from a fixed offset. The implementation searches for the circular window with the highest energy for the requested tap count, extracts that segment, and applies a tail fade before publishing/exporting it.
- If the solved mixed-phase correction is effectively zero, the mixed path falls back to the minimum-phase impulse builder instead of manufacturing a different FIR for no audible benefit.

Published diagnostics and contracts:

- `FilterDesignResult` publishes the requested mixed transition band as `requestedMixedTransitionStartHz` and `requestedMixedTransitionEndHz`.
- Each channel publishes both the pre-solve and post-solve requested mixed group-delay curves, the realized filter group delay, the measured input excess phase, the predicted excess phase after the realized FIR, and the predicted total group delay.
- The process log is part of the architectural contract for this path. It records the selected phase-preparation source, bulk-delay removal, configured phase window, whether correction was shared or per-channel, transition-band diagnostics, stereo peak alignment, and completion of each channel design.

What mixed mode is not allowed to do:

- It must not reinterpret pure bulk delay as excess phase.
- It must not require raw-only phase products when room-based matched sources are unavailable.
- It must not significantly disturb out-of-band phase once the mixed correction taper has reached zero.
- It must not materially change the magnitude result relative to minimum phase when only the phase mode changes.
- It must not sacrifice stereo low-frequency phase relationship for a cleaner-looking single-channel plot.

### Phase And Group-Delay Inputs

The measurement layer publishes two transfer-function product families for transfer windows such as `raw`, `room`, and `reference_compensated_room`:

- `_spectrum` pairs such as `measurement.room_magnitude_spectrum` and `measurement.room_phase_spectrum`
- `_response` pairs such as `measurement.room_magnitude_response` and `measurement.room_phase_response`

In addition, the analyzer publishes time-domain impulse products such as `measurement.raw_impulse_response` and `measurement.room_impulse_response`.

These represent the same transfer function at different resolutions and for different purposes.

`_spectrum` pairs:

- are native positive-FFT-bin products on a linear frequency axis
- preserve the highest available resolution from the analyzer
- are the right source when an algorithm genuinely needs dense bin-level phase behavior

`_response` pairs:

- are derived by interpolating the complex spectrum onto the analyzer's smaller log-frequency display axis
- are the right source for UI plots and for downstream paths that do not need native FFT-bin density
- provide a much cheaper working set for smoothing and repeated recalculation

Current filter-design and phase-preparation decisions:

- excess-phase preparation must use matched magnitude and phase products from the same source window
- phase-preparation source selection prefers matched `_response` pairs first, then matched `_spectrum` pairs as fallback
- phase preparation uses `reference_compensated_room` when available and otherwise `room`
- the user-facing `Phase Window` control is mandatory and clamps to a positive window length; there is no "off" state that bypasses phase-preparation windowing
- bulk delay is removed before minimum-phase reconstruction and excess-phase derivation
- group-delay publication is derived from the prepared phase data, then resampled for display
- minimum-phase magnitude correction still operates from `SmoothedResponse`

Reasoning:

- matched magnitude and phase inputs matter more than reusing unrelated display products
- the room transfer remains the load-bearing acoustic input, while phase preparation derives its own locally windowed phase-preparation view by windowing the reconstructed prepared-transfer impulse
- the `Phase Window` is the one explicit control over how much late phase structure is admitted into mixed-phase preparation
- pure delay appears as a large linear phase ramp and should be separated from excess phase
- group delay is the more trustworthy chart for timing trend; excess phase is a phase-shape diagnostic
- `_spectrum` remains the higher-fidelity representation, but `_response` is currently the preferred preparation input because it keeps interactive recalculation costs acceptable

Implementation note:

- `unwrapPhaseRadians` must compare adjacent raw wrapped samples, not adjacent already-unwrapped samples
- that rule is important because the earlier incorrect behavior caused runaway wrap accumulation and absurd phase cliffs

### Plot Responsibilities

`FiltersPage` currently manages four plot families plus the impulse view:

- Inversion
- Predicted Corrected Response
- Excess Phase
- Filter + Predicted Group Delay
- Filter Impulse

The page owns:

- checkbox state
- section layout
- legend entries
- shared hover synchronization
- conversion from `FilterDesignResult` into `PlotGraphData`

The page does not own:

- correction math
- target alignment
- FIR design
- minimum-phase reconstruction
- phase unwrapping or delay removal

### Plot Semantics

#### Inversion

Purpose:

- compare measured smoothed input magnitude against the computed correction curves

Series/color convention:

- right input: red
- left input: green
- right inversion: magenta
- left inversion: gray

#### Predicted Corrected Response

Purpose:

- compare target, input, and predicted corrected magnitude response

Series/color convention:

- target: accent
- left input: green
- right input: red
- left predicted: gray
- right predicted: magenta

#### Excess Phase

Purpose:

- show residual phase after removing bulk delay and minimum-phase contribution
- provide a before/after comparison for the active phase mode, including low-frequency mixed-phase behavior

Current display decisions:

- the chart shows wrapped excess phase, not accumulated unwrapped rotations
- the y-axis is fixed to `-180..180` degrees
- wrap crossings are rendered as discontinuities rather than connected with vertical spikes
- the low-frequency portion is the useful planning region; the high-frequency portion is naturally more sensitive to noise, timing error, and reflections

Series/color convention:

- right before: red
- left before: green
- right after: magenta
- left after: gray

#### Filter + Predicted Group Delay

Purpose:

- show the filter's own group delay and the predicted total group delay after applying the filter

Current display decisions:

- predicted group delay belongs in this chart, not in the excess-phase chart
- predicted curves are rendered as dashed lines to distinguish them from the filter-only curves

Series/color convention:

- left filter group delay: green
- right filter group delay: red
- left predicted group delay: gray dashed
- right predicted group delay: magenta dashed

### Generic Plotting Rules

`plot_graph` is a reusable plotting widget for filter-design charts.

Current responsibilities and behaviors:

- log-frequency and linear x-axis support
- automatic or fixed y-range support
- brush-based zoom on the x-axis
- reset support
- shared hover marker support across charts
- dashed-line rendering support
- non-finite values break a series instead of being drawn through

Architectural rule:

- generic graph widgets should continue to work from `PlotGraphData`
- workflow-specific meaning belongs in the page module that prepares that data

## Why This Shape Works

This architecture stays intentionally conservative:

- calculations live in measurement modules
- graph widgets stay reusable
- page modules stay UI-focused
- persistence remains centralized
- the app shell remains a coordinator

That balance is what makes ongoing filter-design iteration practical. We can keep refining the design math and the chart semantics without having to move logic back into the Win32 shell.

## Boundaries To Preserve

When extending the codebase, keep these rules intact:

- Do not move file parsing or writing into UI modules.
- Do not let graph widgets read application state directly.
- Do not move measurement, target-curve, or filter-design math into page message handlers.
- Do not move device API work into dialogs or `WolfieApp`.
- Keep `WolfieApp` as a coordinator rather than an implementation dump.
- Keep filter-design calculations in `src/measurement` even when the output is only used by a chart.
- Keep plot semantics in `filters_page` and generic drawing behavior in `plot_graph`.

## Future Extensions

This structure still supports likely next steps without forcing the app shell or UI modules to absorb DSP logic:

- additional filter diagnostics can be added to `FilterDesignResult` and visualized through `PlotGraph`
- persistence can evolve independently of the page modules
- new audio backends can plug into the existing controller structure
