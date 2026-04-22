# AGENTS.md

## Project Summary

`wolfie` is a native Windows C++20 application for acoustic measurement workflows.

The codebase is organized into focused modules:

- `src/core`: shared domain models and text helpers
- `src/measurement`: sweep generation, response analysis, measurement orchestration
- `src/audio`: audio backend abstractions and Windows audio services
- `src/persistence`: workspace/app state loading and saving
- `src/ui`: reusable UI widgets and page/dialog modules
- `src/wolfie_app.*`: top-level application shell and composition root

Read `_architecture.md` before making structural changes.

## Build And Verify

Preferred local configure/build flow on this machine:

```powershell
cmake -S . -B build-refactor -G Ninja `
  -DCMAKE_MAKE_PROGRAM=E:/Programs/CLion/bin/ninja/win/x64/ninja.exe `
  -DCMAKE_CXX_COMPILER=E:/Programs/CLion/bin/mingw/bin/g++.exe `
  -DCMAKE_RC_COMPILER=E:/Programs/CLion/bin/mingw/bin/windres.exe

& 'E:/Programs/CLion/bin/ninja/win/x64/ninja.exe' -C build-refactor
```

Notes:

- `cmake --build` may fail in some shells if the Ninja path is not resolved correctly.
- Existing build directories may contain a locked `wolfie.exe`. If linking fails with `Permission denied`, use a fresh build directory instead of deleting files blindly.
- This is a Windows-only app. Do not try to make Linux/macOS changes unless explicitly requested.

## General Engineering Rules

- Preserve the current module boundaries unless the task is explicitly architectural.
- Prefer small, local changes over broad rewrites.
- Keep new files ASCII unless the file already uses non-ASCII.
- Prefer explicit types and straightforward control flow over clever abstractions.
- Avoid introducing heavy frameworks or unnecessary generic infrastructure.

## Architectural Boundaries

Keep these responsibilities separated:

- UI code belongs in `src/ui` and `src/wolfie_app.*`
- domain measurement logic belongs in `src/measurement`
- device/backend logic belongs in `src/audio`
- file parsing and writing belongs in `src/persistence`
- shared data contracts belong in `src/core`

Do not:

- put WinMM, ASIO, COM, or registry logic into UI modules
- put file parsing/writing into UI modules
- let reusable widgets read global app state directly
- move measurement math into message handlers
- grow `WolfieApp` back into a monolith

## UI Guidance

- The app is Win32-based and hand-laid-out. Preserve the existing style unless the task is a UI redesign.
- Reusable rendering components should remain generic. `ResponseGraph` should accept view data, not workspace/application state.
- New tabs or dialogs should usually become separate UI modules in `src/ui`.
- Keep top-level window procedures thin and delegate to focused classes where possible.

## Audio And Platform Guidance

- `src/audio/winmm_audio_backend.*` is the only place for WinMM playback/capture session logic.
- `src/audio/asio_service.*` owns ASIO enumeration and control-panel access.
- Be conservative around audio resource lifetime. Cleanup order matters.
- Do not silently change sample format, channel layout, or timing assumptions unless the task requires it.

## Persistence Guidance

- Persist workspace-level data through `WorkspaceRepository`.
- Persist app-level data such as recent workspaces through `AppStateRepository`.
- If adding new persisted fields, update both load and save paths.
- Keep file formats backward-compatible when practical.

## Measurement Guidance

- Keep pure signal generation and analysis code independent from UI and platform APIs.
- `MeasurementController` should orchestrate workflow, not own rendering or persistence logic.
- If adding new analysis stages, prefer adding focused helpers to `src/measurement` rather than expanding `WolfieApp`.

## Refactoring Rules

When refactoring:

- preserve behavior first, then improve structure
- move code into the right module before changing semantics
- keep public interfaces small and explicit
- update `_architecture.md` if module responsibilities materially change

For larger refactors:

1. extract pure logic first
2. extract persistence/platform seams next
3. refactor orchestration last

## File And Build Hygiene

- Ignore `build-*` and IDE-generated directories unless the task is specifically about build configuration.
- Do not remove or overwrite user build outputs just to get a clean build.
- If an executable is locked, use another build directory.
- If you add new source files, update `CMakeLists.txt`.

## Patch Size Guidance

The editor interface can fail on oversized patches. To avoid overflow:

- Prefer multiple small `apply_patch` calls over one very large patch.
- Add files in small batches instead of creating many files in one patch.
- Split large file rewrites into staged edits:
  1. add or replace the skeleton
  2. add one logical section at a time
  3. do cleanup passes last
- If moving a large monolithic file into modules, create the destination files first, then replace the source file in a separate patch.
- Do not combine unrelated file additions, deletions, and large updates in one patch unless the change is genuinely small.
- If a previous large patch fails, retry with narrower hunks instead of repeating the same large patch.
- For very large generated-style content, prefer writing the minimal hand-maintained structure rather than pasting huge blocks at once.

Practical rule:

- Keep each patch focused on one module or one responsibility boundary when possible.

## Testing Expectations

There is no formal test suite yet.

For code changes, verify at least:

- the project configures
- the project builds
- the changed path is consistent with the module boundaries above

If a runtime behavior change cannot be exercised in the current environment, state that clearly.

## Good Change Patterns

- Add a new reusable graph or control under `src/ui`
- Add a new backend abstraction under `src/audio`
- Add new workspace fields via `src/core` models plus `src/persistence`
- Add new analysis helpers under `src/measurement`

## Bad Change Patterns

- Adding large helper functions back into `src/wolfie_app.cpp`
- Letting dialogs directly edit files
- Letting graphs pull state from `WolfieApp`
- Mixing COM/registry/device access into page classes
- Adding one-off utility code into unrelated modules because it is convenient
