# SugarBot

Last updated: 2026-03-14

SugarBot is a C++ strategy experimentation engine for decision-rule research over tabular observations.

The current engine is built around one DataView-first compiled-program runtime that can carry temporal, spatial, spatiotemporal, and panel-shaped datasets without splitting into separate runtime families.

It is:
- A rule lab for composing signals, wiring strategies, running execution, and evaluating outcomes.
- Built for inspectability, deterministic contracts, and controlled iteration.

It is not:
- A predictor.
- A black-box model.
- A production trading system.

License: ALL RIGHTS RESERVED.

## Architecture

SugarBot supports one canonical runtime family plus one explicit compatibility path:

1. Canonical runtime pipeline
`CSV -> RawTable -> DataSpec -> preprocess -> NumericFrame/FrameSet -> align -> DataView -> compile_plan -> CompiledProgram -> StrategyExecutor / sweep`

2. Legacy candle compatibility path
`CSV -> CandleSeries -> legacy_candle_frame_adapter -> NumericFrame -> align -> DataView -> canonical runtime`

Current contract highlights:
- `DataView` and `AlignedBundle` are the one stable indexed observation runtime family.
- `PlanSpec` is strategy DSL only; `DataSpec`, `IndexSchema`, and `RunManifest` own observation policy, axis roles, alignment intent, and provenance.
- `TimeKey` is canonical (`int64_t`), and `TimeKind::Iso8601` is normalized to UTC epoch-seconds with intraday fidelity preserved.
- Row domain is first-class: `row_count()` is not derived from time payload, and single-series non-temporal inputs are supported without inventing synthetic time.
- Alignment is still calendar/time based only; mixed temporal/non-temporal inputs and non-temporal multi-series alignment reject explicitly instead of silently falling back to row position.
- Runtime topology is a named sidecar seam: built-in temporal, spatial, and spatiotemporal topologies are mounted onto the runtime catalog as `builtin.temporal.window`, `builtin.spatial.metric_2d`, and `builtin.spatiotemporal.composite`.
- Current locality execution is intentionally narrow and explicit: `spatial.radius_mean` and `st.radius_mean` run on the DataView/NeighborhoodQuery substrate.
- Shared observation-capability and plan-capability diagnosis power prep/align dumps, runtime preview, interactive and panel reporting, and manifest snapshots.
- Custom sweep now compiles once through template compilation and instantiates executable plans per bound parameter set.

## Build

Requirements:
- CMake 3.16+
- C++20 compiler
- Windows: MSVC x64
- Linux/WSL2: GCC or Clang

All shipped presets enable `BUILD_TESTING=ON`.

### Windows (PowerShell)

Run from Visual Studio or a terminal using the Windows Ninja configuration:

```powershell
cmake --preset windows-debug
cmake --build --preset windows-debug
```

Standard Windows build artifacts are kept under `out/build/x64-debug` or `out/build/x64-release`, with installs under the matching `out/install/...` tree.

Visual Studio note:
- Visual Studio folder mode is intentionally Windows-local only and should expose the two Ninja configs from [CMakeSettings.json](/C:/Users/Mel/source/repos/Absolute-Convergence/sugar_bot/sugar_bot_cpp_upload/CMakeSettings.json):
  - `x64-Debug`
  - `x64-Release`
- WSL/Linux support remains available through [CMakePresets.json](/C:/Users/Mel/source/repos/Absolute-Convergence/sugar_bot/sugar_bot_cpp_upload/CMakePresets.json) and normal CLI use, not through the Visual Studio folder config list.

### Linux / WSL2

```bash
cmake --preset linux-release
cmake --build --preset linux-release
```

The configure/build tree for preset builds lives under `out/build/wsl-gcc-debug` or `out/build/wsl-gcc-release`.

Visual Studio note:
- `CMakeSettings.json` is intentionally Windows-local only so the Visual Studio folder workflow does not mix Windows and WSL configurations.
- WSL remains supported through `CMakePresets.json` and normal CLI/WSL invocations.

By default on Linux/WSL, final runtime artifacts are redirected to:
`$HOME/devbuild/sugar_Bot/out/Linux/<CONFIG>`

Override that location with `SUGAR_OUT_ROOT` if you need a different output root.

## Executables

- `sugarbot`: main CLI and interactive runtime. This is the DataView-first app surface for frame-first ingestion, custom/template runs, panel runs, diagnostics, and contract dispatch.
- `SB`: generated launcher alias beside `sugarbot` in build and install output.
- `sugar_contracts`: standalone deterministic contract-suite runner with focused section filtering.
- `sugar_app_tests`: app-surface wiring suite for CLI/orchestration behavior. This is a build/test target, not an installed end-user binary.

## Install For Easy Launch

Use a normal `cmake --install` step from the configured build tree if you want `sugarbot`, `sugar_contracts`, and the `SB` launcher alias under a stable install root such as `out/install/x64-debug/bin` or `out/install/wsl-gcc-release/bin`.

For preset builds, that usually means installing the matching preset build directory after configuration and build are complete.

## Quick Start

If you launch `sugarbot` with no mode flags, it enters the interactive flow.

### Legacy preset ingestion (no DataSpec file)

```bash
./sugarbot path/to/data.csv
```

(Windows: `.\sugarbot.exe path\\to\\data.csv` or `.\SB.exe path\\to\\data.csv`)

This path still resolves into the canonical preprocess -> NumericFrame -> align -> DataView runtime. Without `--data-spec`, SugarBot uses runtime ingest defaults, `--format`, and CSV-header/value auto-detect to choose a compatible input profile.

If you want to force the preset-style interpretation without a DataSpec file, use interactive mode with `--format auto`, `--format legacy_ohlcv_date`, or `--format iso_ohlcv_time`.

### Frame-first ingestion (recommended)

```bash
./sugarbot --data-spec path/to/data_spec.json path/to/data.csv
```

This is the authoritative ingestion path. `DataSpec` controls parsing, axis roles, optional entity grouping, optional spatial coordinates, header policy, and normalization before everything enters the same DataView runtime.

For diagnostics before execution, use the same pair of files with `--prep-dump` or `--align-dump`.

## CLI Modes

- Default interactive mode: `sugarbot [csv] [--data-spec spec.json] [--format auto|legacy_ohlcv_date|iso_ohlcv_time]`
  - Enters the menu-driven template/custom flow and can reuse shared runtime diagnostics before execution.
- `--data-spec <data_spec.json> [<csv>]`
  - Marks the runtime input path as explicit frame-first ingestion for interactive/template/custom execution.
- `--prep-dump <data_spec.json> <csv>`
  - Runs preprocess only and prints schema/frame diagnostics plus shared observation-capability output.
- `--align-dump <data_spec.json> <csv> --series-key-col <col> [--calendar union|intersection]`
  - Builds aligned multi-series runtime diagnostics and prints shared observation/topology truth before execution.
- `--run-panel <data_spec.json> <csv> [--primary <col>] [--topk N]`
  - Builds a panel-style runtime package and then runs template or custom PlanSpec flow with the same shared capability reporting used elsewhere.
- `--dump-ops`
  - Prints the operator registry and runtime contract metadata, including axis scope, required axes, topology requirements, default topology bindings, and locality-subject binding surface.
- `--run-contract-suite [--contract-section <key> ...]`
  - Runs the deterministic contract suite from the app binary. Repeating `--contract-section` filters to focused sections such as `runtime_design_loads`, `runtime_overload_rejection`, `metadata_preservation`, `locality_design_loads`, `topology_overload_rejection`, `compile_bridge_design_loads`, or `runtime_gates`.

## Strategies and Plans

Built-in templates currently include:
- Gated Diff Cross
- Donchian Breakout
- ZScore Mean Reversion
- Swing Breakout Validation
- Custom User PlanSpec

Built-in templates load observation-agnostic PlanSpec JSON from `graphs/templates/` and run through the same DataView-first compile/runtime bridge as custom plans.

`PlanSpec` owns strategy graph structure, params, bindings, execution defaults, and optional sweep metadata. It is not the observation/world-model contract.

PlanSpec runtime is DataView-native for single-series runs and for the currently supported multi-series, panel, spatial, and spatiotemporal shapes that the shared runtime can already build.

Custom sweep follows the compiled-template path: compile once with `compile_plan_template(...)`, then instantiate executable plans per override set through `instantiate_compiled_plan(...)`.

Series-prefixed refs such as `series:column` are supported in custom plans for multi-series bindings. Cross-series execution is still intentionally narrow today (`ms.diff`, `ms.ratio`, `ms.zscore`), and locality execution is intentionally narrow today (`spatial.radius_mean`, `st.radius_mean`) over built-in topologies.

Legacy embedded observation inputs (`series_inputs`) remain compatibility-only adapter metadata for older self-contained plans. They are resolved inside `run_pipeline` and do not define a separate runtime family.

Checked-in active examples under `graphs/` are observation-agnostic. Retired embedded-input examples live under `retirement/legacy_reference/graphs/`.

## DataSpec Notes

DataSpec is the parsing and normalization policy boundary for observations. It controls:
- `input_profile` defaults and legacy/ISO preset intent
- time column plus `time_kind`
- value columns plus rename map
- primary value column
- missing-data policy and missing tokens
- dedupe policy and sort behavior
- optional entity grouping
- optional spatial axis (`spatial_x_col`, `spatial_y_col`)
- CSV header policy:
  - `RequiredHeaders`
  - `OptionalHeaders`
  - `HeaderlessPositional` with explicit `columns_by_index`

Important current behavior:
- Spatial-only `DataSpec` is valid without `time_col`.
- Preprocess preserves source order for non-temporal single-series runtime inputs and leaves semantic time empty instead of inventing ordinal `TimeKey` storage.
- `IndexSchema` remains axis metadata only; topology is carried separately through runtime topology catalogs and manifests.
- Headerless ingestion is explicit and deterministic through `HeaderlessPositional` plus `columns_by_index`.
- In app runtime ingestion, explicit `--data-spec` is authoritative. Without it, SugarBot may use `--format` or header/value auto-detect to select a compatible profile.

## Testing

Deterministic contract coverage can be run directly:

```bash
./sugar_contracts
```

or through the app surface:

```bash
./sugarbot --run-contract-suite
```

Focused contract sections are available from both entry points. Examples:

```bash
./sugar_contracts --contract-section runtime_design_loads
./sugar_contracts --contract-section topology_design_loads --contract-section locality_overload_rejection
```

The focused harness is now load-rated:
- `runtime_design_loads` freezes what the current runtime is explicitly meant to carry.
- `runtime_overload_rejection` freezes explicit clean rejection for heavier unsupported loads.
- `metadata_preservation` freezes provenance and non-goal preservation without pretending those seams execute.

CTest integration:

```bash
ctest --preset windows-debug
```

Current registered CTest lanes include:
- full contract suite
- focused runtime gates
- focused locality, topology, and compile-bridge seam sections
- load-rated wrapper lanes
- app-surface suite (`sugar_app_tests`)

## Repository Layout

- `app/` CLI parsing, command dispatch, runtime glue, reporters, and interactive flow
- `include/` public contracts and stable headers
- `src/` ingest, runtime, compiler, evaluator, sweep, manifests, and contract-suite implementation
- `graphs/` active observation-agnostic PlanSpec examples and built-in template JSON
- `data/` sample CSV inputs, sample DataSpec files, and local dump artifacts
- `retirement/legacy_reference/` retired compatibility-era graphs and reference material
- `out/` user-managed preset build and install trees
- `release/` packaging helpers and release-related notes

## Disclaimer

SugarBot is a research and experimentation tool. Backtests, capability reports, and contract coverage do not guarantee future performance or production suitability.
