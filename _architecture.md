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
- `measurement_controller.h/.cpp`
- `target_curve_designer.h/.cpp`
- `filter_designer.h/.cpp`
- `stereo_diagnostics.h/.cpp`
- `waterfall_builder.h/.cpp`

Responsibilities:

- `sweep_generator` creates playback sweep data and exported sweep WAV content.
- `response_analyzer` turns captured audio into `MeasurementResult` value sets and analysis metadata, including direct/room transfer products and optional reference-compensated variants.
- `room_simulator` generates synthetic room-response `MeasurementResult` data for UI and filter-testing workflows without audio I/O.
- `measurement_controller` orchestrates room and reference measurement runs through an audio backend.
- `target_curve_designer` computes target-curve view data without UI dependencies.
- `filter_designer` computes correction curves, FIR filters, mixed-phase phase shaping, simulated responses, phase-derived diagnostics, and filter-design view data without UI dependencies.
- `stereo_diagnostics` computes left/right comparison diagnostics and analysis-plot data from measured transfer functions without UI dependencies.
- `waterfall_builder` derives waterfall data from measured impulse responses and predicted post-filter impulse responses.

Reasoning:

- Signal analysis, target evaluation, and filter design are domain logic.
- UI code should not own FFTs, smoothing, phase handling, or filter prediction rules.

### `src/audio`

Purpose: audio I/O and driver-related platform services.

Files:

- `audio_backend.h`
- `asio_sdk.h/.cpp`
- `asio_audio_backend.h/.cpp`
- `winmm_audio_backend.h/.cpp`
- `asio_service.h/.cpp`

Responsibilities:

- Define audio-session abstractions.
- Implement playback and capture through WinMM or ASIO.
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
- `measurement_loopback_tests` covers sweep playback planning, synthetic capture analysis, direct/room result publication, reference compensation, latency retention, and waterfall generation.
- `filter_design_basics_tests` covers baseline filter-design behavior such as target evaluation, correction-shape behavior, and general result sanity.
- `phase_preparation_tests` covers bulk-delay removal, source selection, fallback rules, and phase-preparation metadata/process logging.
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

Rules:

- do not call `WorkspaceRepository::save()` from interactive UI paths such as sliders, graph zoom handlers, hover updates, or live recalculation feedback
- automatic full-workspace writes belong at app exit and workspace switch boundaries
- explicit user-driven save remains a separate action
- narrower persistence helpers such as settings-only or UI-only saves should be used for interactive edits that need persistence without rewriting measurement payloads

Reasoning:

- full workspace writes include much more than lightweight UI state
- calling the full save path from smoothing or filter controls causes avoidable stalls and regression-prone behavior
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
3. `stereo_diagnostics` derives summary metrics and plot curves from direct or room spectra, optionally using reference-compensated data when available.

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

In data-model terms:

- minimum mode usually leaves predicted excess phase close to the measured input excess phase
- mixed mode may reduce low-frequency excess phase while preserving the same general magnitude-correction workflow
- the excess-phase and group-delay plots are now part of the active filter-design workflow, not just placeholders

Mixed-phase regression policy:

- evaluate mixed-mode changes against both synthetic fixtures and at least one persisted real workspace when the reported failure involves audible artifacts, waterfall decay, or narrow low-frequency magnitude loss
- treat the realized FIR as the load-bearing artifact, not just the predicted plots: regressions may hide if only the correction target or averaged band metrics are inspected
- when balancing bass preservation against late artifacts, the acceptance check is the combined result on the real workspace: low-frequency corrected-response drift versus minimum phase must stay bounded, and the realized mixed FIR plus expected waterfall must improve in the expected direction when tap count is increased for that regression

### Phase And Group-Delay Inputs

The measurement layer publishes two transfer-function product families for each source window such as `raw`, `room`, `direct`, and `reference_compensated_direct`:

- `_spectrum` pairs such as `measurement.direct_magnitude_spectrum` and `measurement.direct_phase_spectrum`
- `_response` pairs such as `measurement.direct_magnitude_response` and `measurement.direct_phase_response`

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
- direct-window products are preferred over room-window products, and room-window products are preferred over raw products
- bulk delay is removed before minimum-phase reconstruction and excess-phase derivation
- group-delay publication is derived from the prepared phase data, then resampled for display
- minimum-phase magnitude correction still operates from `SmoothedResponse`

Reasoning:

- matched magnitude and phase inputs matter more than reusing unrelated display products
- direct-window transfer products are the better default for excess-phase meaning and bulk-delay estimation
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
