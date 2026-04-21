# SugarBot Codebase Map

This file is the human navigation map for the repository.

## Design Intent

The repository is organized around the same module boundaries already used in `CMakeLists.txt`.
Private implementation files live under focused subdirectories in `src/`.

The long-term goal is not two parallel header maps. It is one human-readable module map that lines up between `include/` and `src/`.

Current reality:
- `src/` already has the clearer module layout.
- `include/` now mirrors the public module layout instead of staying flat.
- the wrapper-tree experiment under `include/compat`, `include/data`, `include/plan`, `include/runtime`, and `include/testing` has been removed.
- real mirrored public header directories now exist for `legacy`, `ingest`, `ops`, `engine`, `sweep`, and `contracts`.
- there are no remaining flat public headers at the root of `include/`.

Revised direction:
- keep the mirrored module layout stable
- do not reintroduce wrapper-only header trees
- prefer future cleanup in private/internal headers before reshuffling the public map again

## Directory Guide

- `app/`
  Foldered app/orchestration surface.
  Subdirectories separate entrypoints, parsing, dispatch, interactive flow, reporting, runtime glue, and app tests.

- `app/entrypoints/`
  Thin executable roots for `sugarbot` and `sugar_contracts`.

- `app/cli/`
  Argument parsing and command-shape definitions.

- `app/commands/`
  Non-interactive command dispatch and CLI command handlers.

- `app/interactive/`
  Prompt-driven menu flow and interactive orchestration.

- `app/reporting/`
  Human-readable surface formatting for runtime and plan diagnostics.

- `app/runtime/`
  Thin app-layer glue over shared runtime-building helpers.

- `app/tests/`
  App-surface wiring checks and source-scan protections.
  Shared fixtures and helper utilities live in `app_test_support.*`, while the registered test files are split by surface.

- `include/`
  Public contracts and stable headers.
  It now mirrors the public module map directly.

- `include/legacy/`
  Public compatibility-era shapes and adapters.
  This lines up with `src/legacy/`.

- `include/ingest/`
  Data ingestion, schema, alignment, topology, locality, and runtime-world headers.
  This lines up with `src/ingest/`.

- `include/ops/`
  Operator registry surfaces and kernel-facing contracts.
  This lines up with `src/ops/`.

- `include/engine/`
  Plan compilation, execution-machine, and evaluator-facing headers.
  This lines up with `src/engine/`.

- `include/sweep/`
  Sweep orchestration, panel execution, metrics, and menu/template-facing headers.
  This lines up with `src/sweep/`.

- `include/contracts/`
  Public contract-harness entry points.
  This lines up with `src/contracts/`.

- `src/legacy/`
  Compatibility-only implementation for retired or legacy input shapes.
  Current example: `series.cpp`.

- `src/ingest/`
  Observation loading and runtime-world construction.
  This is the path from CSV and JSON specs into `DataView`.
  Key files: `data_spec.cpp`, `preprocess.cpp`, `align.cpp`, `data_view.cpp`, `run_pipeline.cpp`.

- `src/ops/`
  Operator registry, operator modules, and kernel implementations.
  If you are adding or changing an op, start here.

- `src/engine/`
  Plan compilation, compiled-program evaluation, and execution FSM behavior.
  This is the core strategy machine.

- `src/sweep/`
  Plan diagnosis, sweep orchestration, ranked multi-entity execution, and panel helpers.

- `src/contracts/`
  Deterministic contract harness and seam-level tests.
  Large by design; not the best entry point for understanding the engine.

- `src/app_support/`
  Shared helpers used by the app surface but not part of `app/` argument parsing itself.
  This is implementation-only unless a true public contract emerges later.

- `src/service/`
  Internal hosted-service control-plane code.
  This is where session tracking, staged artifact handling, owned results, request validation, and local no-socket job execution now live.

- `graphs/`
  Checked-in `PlanSpec` examples and built-in template JSON.

- `data/`
  Sample datasets and `DataSpec` examples.

## Read In This Order

If you are coming back to the repo and want the shortest path back to understanding:

1. `README.md`
2. `include/ingest/data_spec.h`
3. `src/ingest/preprocess.cpp`
4. `src/ingest/align.cpp`
5. `include/ingest/data_view.h`
6. `include/sweep/plan_spec.h`
7. `include/engine/plan_compiler.h`
8. `src/engine/plan_compiler.cpp`
9. `src/engine/program_evaluator.cpp`
10. `app/commands/app_commands.cpp`
11. `src/service/server_runtime.cpp`

Read `src/contracts/` after that, not before.

## Next Cleanup

The public header migration is now complete for the current module map.

If more cleanup is needed, the next sensible order is:

1. Keep the wrapper-tree experiment retired.
   Do not add browse-only header namespaces back into `include/`.

2. Keep the public module names stable.
   `legacy`, `ingest`, `ops`, `engine`, `sweep`, and `contracts` are now the browsing roads.

3. Focus future cleanup on private/internal seams.
   Candidates are private header placement in `src/`, internal naming drift, and whether `app_support` should merge into a clearer app/runtime boundary later.

4. Add compatibility shims only if a concrete external need appears.
   Do not create duplicate public trees preemptively.

## Mental Model

The main runtime road is:

`CSV -> DataSpec -> preprocess -> NumericFrame/FrameSet -> align -> DataView -> compile PlanSpec -> evaluate -> StrategyExecutor`

The best way to stay oriented is to ask which side of that road a file belongs to:

- observation ingest
- operator definition
- compile/evaluate/execute
- sweep/panel/control-plane
- app surface
- contract harness

If a file does not obviously fit one of those, it probably deserves a closer look.
