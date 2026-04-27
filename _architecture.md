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
- `measurement_controller.h/.cpp`
- `target_curve_designer.h/.cpp`
- `filter_designer.h/.cpp`
- `waterfall_builder.h/.cpp`

Responsibilities:

- `sweep_generator` creates playback sweep data and exported sweep WAV content.
- `response_analyzer` turns captured audio into `MeasurementResult` value sets and analysis metadata.
- `measurement_controller` orchestrates a measurement run through an audio backend.
- `target_curve_designer` computes target-curve view data without UI dependencies.
- `filter_designer` computes correction curves, minimum-phase FIR filters, simulated responses, phase-derived diagnostics, and filter-design view data without UI dependencies.
- `waterfall_builder` derives waterfall data from measured impulse responses.

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

Responsibilities:

- Load and save workspace files and recent-workspace state.
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
- `target_curve_graph.h/.cpp`
- `target_curve_page.h/.cpp`
- `filters_page.h/.cpp`
- `settings_dialog.h/.cpp`
- `waterfall_graph.h/.cpp`

Responsibilities:

- `ui_theme` defines shared colors and visual constants.
- `response_graph` renders reusable response displays for measurement workflows.
- `plot_graph` renders reusable non-interactive plots for filter-design workflows.
- `measurement_page`, `target_curve_page`, and `filters_page` own their controls, layout, legend state, and graph synchronization.
- `target_curve_graph` and `waterfall_graph` implement specialized graph behavior for their workflows.
- `settings_dialog` owns settings-window behavior and delegates ASIO-specific work outward.

Reasoning:

- Widgets should render supplied data, not reach into `WolfieApp`.
- Page modules should own page behavior so the app shell stays focused on coordination.

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

## Dependency Direction

The intended dependency flow is:

- `core` is shared by everything.
- `measurement` depends on `core` and audio abstractions.
- `audio` depends on `core` and platform APIs.
- `persistence` depends on `core`.
- `ui` depends on `core` and prepared view data.
- `wolfie_app` depends on all modules and composes them.

Practical examples:

- `response_graph` and `plot_graph` do not know about `WorkspaceState`.
- `workspace_repository` does not know about tabs or graph widgets.
- `measurement_controller` does not know how pages are drawn.
- `filter_designer` does not call Win32 APIs.

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

The current implementation is minimum-phase only.

Consequences:

- `FilterDesignSettings::phaseMode` is normalized to `"minimum"`.
- Designed FIR filters are minimum-phase FIRs.
- Magnitude correction and predicted corrected response are simulated from those filters.
- Minimum-phase correction does not modify excess phase.

That last point is intentional and currently visible in the data model:

- `predictedExcessPhaseDegrees` is expected to match `inputExcessPhaseDegrees`
- the excess-phase chart exists as a baseline for future excess-phase correction work, not as proof that excess phase is currently being corrected

### Phase And Group-Delay Inputs

Filter-design phase diagnostics must be derived from the dense measured phase spectrum, not from an already-downsampled display response.

Current decisions:

- `filter_designer` prefers `measurement.raw_phase_spectrum` from `MeasurementResult`
- phase is unwrapped before it is resampled to the filter-design display axis
- the measured impulse pre-roll delay is removed before excess-phase diagnostics are derived
- a residual best-fit linear phase slope is also removed so leftover pure delay does not dominate the excess-phase view

Reasoning:

- using a sparse display-axis phase response for unwrap is unstable at high frequency
- pure delay appears as a large linear phase ramp and should be separated from excess phase
- group delay is the more trustworthy chart for timing trend; excess phase is a phase-shape diagnostic

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
- provide a before/after comparison even though the current minimum-phase filter leaves excess phase unchanged

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

This structure supports several likely next steps:

- a true excess-phase correction mode can extend `filter_designer` without redesigning the page module
- additional filter diagnostics can be added to `FilterDesignResult` and visualized through `PlotGraph`
- persistence can evolve independently of the page modules
- new audio backends can plug into the existing controller structure
