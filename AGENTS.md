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

## General Engineering Rules

- Preserve the current module boundaries unless the task is explicitly architectural.
- Prefer small, local changes over broad rewrites.
- Keep new files ASCII unless the file already uses non-ASCII.
- Prefer explicit types and straightforward control flow over clever abstractions.
- Avoid introducing heavy frameworks or unnecessary generic infrastructure.

## Architectural Boundaries

Pay attention the rules in _architecture.md file.

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

- Use cmake
- Ignore `build-*` and IDE-generated directories unless the task is specifically about build configuration.
- Do not remove or overwrite user build outputs just to get a clean build.
- If an executable is locked, use another build directory.
- If you add new source files, update `CMakeLists.txt`.
- Run builds sequentially to avoid race conditions. Too often they would block each other.

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
There is no need to run a test suite for small or cosmetic UI changes. Save tokens and time.


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
