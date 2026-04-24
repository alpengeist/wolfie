# Wolfie Architecture

## Intent

The codebase is organized around one main constraint: the Win32 app shell should not own business logic, device I/O, persistence, and rendering details at the same time.

## Design Principles

- Keep platform UI code at the edge.
- Keep pure calculation code independent from Win32 and device APIs where possible.
- Isolate audio backends behind an interface so measurement orchestration does not depend on WinMM details.
- Isolate file formats and persistence from UI state management.
- Make reusable views, especially response graphs, independent from any one workflow.

## Module Overview

### `src/core`

Purpose: shared domain types and basic text conversion helpers.

Files:

- `models.h`
- `text_utils.h/.cpp`

Responsibilities:

- Define shared application data structures such as `WorkspaceState`, `AudioSettings`, `MeasurementSettings`, `MeasurementResult`, and `MeasurementStatus`.
- Provide UTF-8 / UTF-16 conversion and numeric formatting helpers used across UI and services.

Reasoning:

- These types are the contract between modules.
- Keeping them separate prevents UI, persistence, and measurement code from redefining or reshaping the same concepts differently.

### `src/measurement`

Purpose: measurement-domain logic.

Files:

- `sweep_generator.h/.cpp`
- `response_analyzer.h/.cpp`
- `measurement_controller.h/.cpp`
- `target_curve_designer.h/.cpp`
- `waterfall_builder.h/.cpp`

Responsibilities:

- `sweep_generator` creates playback sweep data and writes the generated sweep WAV file.
- `response_analyzer` computes amplitude and frequency response data from captured audio.
- `measurement_controller` orchestrates a single measurement run, tracks progress, and converts backend capture data into a final `MeasurementResult`.
- `target_curve_designer` computes target-curve view data from the basic curve and bell bands without depending on Win32 UI code.
- `waterfall_builder` derives waterfall-plot view data from measured impulse responses without depending on Win32 UI code.

Reasoning:

- Signal generation and response analysis are core domain logic, not UI logic.
- `measurement_controller` is intentionally above the pure math layer and below the UI layer. It knows the workflow of a measurement, but not how to draw controls or talk directly to Win32 widgets.

### `src/audio`

Purpose: audio I/O and driver-related platform services.

Files:

- `audio_backend.h`
- `asio_sdk.h/.cpp`
- `asio_audio_backend.h/.cpp`
- `winmm_audio_backend.h/.cpp`
- `asio_service.h/.cpp`

Responsibilities:

- `audio_backend.h` defines the abstraction for a measurement session and audio backend.
- `asio_sdk` owns the shared COM/driver interop definitions used by ASIO-facing modules.
- `asio_audio_backend` implements routed playback/capture using the selected ASIO driver and channels.
- `winmm_audio_backend` implements playback/capture using WinMM.
- `asio_service` handles ASIO driver enumeration and opening a driver control panel.

Reasoning:

- Audio device I/O is platform-specific and operationally fragile; it should be isolated.
- The measurement workflow should depend on an interface, not on `waveIn*` / `waveOut*` calls.
- ASIO settings and driver control-panel access are service concerns, not dialog concerns.

This split also leaves room for future backends without changing measurement orchestration.

### `src/persistence`

Purpose: file I/O and serialization of application/workspace state.

Files:

- `workspace_repository.h/.cpp`
- `app_state_repository.h/.cpp`

Responsibilities:

- Load and save workspace files such as `workspace.json`, `ui.json`, and `measurement/response.csv`.
- Load and save app-level state such as recent workspaces.

Reasoning:

- The UI should not parse JSON-like text or CSV directly.
- Persistence logic is easier to test and replace when it is grouped behind repository-style modules.
- Keeping this separate also reduces accidental coupling between widget state and on-disk format.

### `src/ui`

Purpose: reusable UI components and window-specific UI modules.

Files:

- `ui_theme.h`
- `response_graph.h/.cpp`
- `measurement_page.h/.cpp`
- `settings_dialog.h/.cpp`
- `target_curve_graph.h/.cpp`
- `target_curve_page.h/.cpp`
- `waterfall_graph.h/.cpp`

Responsibilities:

- `ui_theme` contains shared colors and visual constants.
- `response_graph` is a reusable graph widget for response displays.
- `measurement_page` owns the measurement tab controls, layout, and widget synchronization.
- `settings_dialog` owns the settings window behavior and delegates ASIO-specific actions to `AsioService`.
- `target_curve_graph` owns the interactive graph rendering and drag behavior for target-curve editing.
- `target_curve_page` owns the target-curve tab controls, EQ band list, and widget synchronization.
- `waterfall_graph` renders the dedicated waterfall-decay view used by the measurement page.

Reasoning:

- The response graph will be reused in multiple workflows, so it should not depend on `WolfieApp` or directly read application state.
- The measurement page is a UI module, not an app-wide controller.
- Dialog code becomes easier to change when it only handles dialog behavior and delegates persistence and services outward.

### `src/wolfie_app.*`

Purpose: application shell and top-level coordination.

Responsibilities:

- Create the main window, menus, tabs, and high-level layout.
- Coordinate workspace lifecycle actions.
- Connect the UI modules with repositories, services, and the measurement controller.
- Own top-level message handling.

Reasoning:

- `WolfieApp` should be the composition root.
- It wires modules together, but it should avoid embedding domain logic or low-level platform logic that belongs elsewhere.

## Dependency Direction

The intended dependency flow is:

`core` <- shared by everything

`measurement` depends on `core` and `audio` abstractions

`audio` depends on `core` and platform APIs

`persistence` depends on `core`

`ui` depends on `core` and service/controller-facing interfaces

`wolfie_app` depends on all modules and composes them

More practically:

- `response_graph` does not know about `WorkspaceState`
- `winmm_audio_backend` does not know about Win32 controls
- `workspace_repository` does not know about tabs, dialogs, or progress bars
- `measurement_controller` does not know how the UI is drawn

## Runtime Flow

### Startup

1. `WolfieApp` loads app state from `AppStateRepository`.
2. `WolfieApp` creates the main window and UI modules.
3. If possible, the last workspace is loaded through `WorkspaceRepository`.
4. `MeasurementPage` is populated from `WorkspaceState`.

### Measurement

1. `MeasurementPage` pushes edited settings into `WorkspaceState`.
2. `WolfieApp` calls `MeasurementController::start`.
3. `MeasurementController` builds sweep playback data using `sweep_generator`.
4. `MeasurementController` starts an `IAudioMeasurementSession` through the audio backend.
5. Timer ticks call `MeasurementController::tick`.
6. The controller polls the backend, updates progress/status, and when finished computes `MeasurementResult` through `response_analyzer`.
7. `WolfieApp` stores the result via `WorkspaceRepository`.
8. `MeasurementPage` passes graph data into `ResponseGraph`.

### Settings

1. `WolfieApp` opens `SettingsDialog`.
2. `SettingsDialog` uses `AsioService` to enumerate drivers and open the ASIO control panel.
3. On save, dialog changes are returned to `WolfieApp`.
4. `WolfieApp` persists them through `WorkspaceRepository`.

## Why This Shape Works

This architecture is intentionally conservative. It does not introduce a deep framework or elaborate abstraction hierarchy. It solves the immediate problem of a monolithic app file by introducing clear seams with practical value:

- pure logic is easier to reason about
- platform APIs are isolated
- reusable widgets can be reused
- file formats are centralized
- the top-level app shell is smaller and clearer

The goal is not maximum abstraction. The goal is local changeability.

## Future Extensions

This layout supports several likely future changes:

- additional response graphs can reuse `ResponseGraph`, while specialized time-frequency views can follow the `waterfall_builder` + `waterfall_graph` split
- a future ASIO or WASAPI measurement backend can implement `IAudioBackend`
- target-curve, filter, and export tabs can each become their own UI modules
- persistence can move to a real JSON library later without touching UI code
- measurement analysis can grow without re-entering `WolfieApp`

## Boundaries To Preserve

When extending the codebase, these boundaries should remain intact:

- Do not put file parsing/writing back into UI modules.
- Do not put device API calls into `WolfieApp` or dialog classes.
- Do not let graph widgets read application state directly from the app shell.
- Do not put sweep generation or response analysis into UI handlers.
- Keep `WolfieApp` as a coordinator, not as the implementation site for all behavior.

If a new feature does not fit a current module, prefer adding a focused new module over expanding `WolfieApp` again.
