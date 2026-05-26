---
artifact_id: "01-codebase-inventory"
harness_id: "agentic-docs.codebase-inventory"
library_version: "1.0.0"
subject: "SugarBot"
scope: "Active source under D:/repositories/sugar_bot excluding retirement/legacy_reference"
generated_at: "2026-05-25T21:00:28.8150479-04:00"
inputs:
  - "CMakeLists.txt"
  - "include/"
  - "src/"
  - "app/"
  - "graphs/demos/"
  - "docs/architecture_insight_summary.md"
  - "docs/architecture_executive_deep_dive.md"
confidence_policy: "observed | inferred | speculative labels required"
review_state: "draft"
---

# Scope

Inventory of the active SugarBot runtime, app, service, contract, demo, and documentation surfaces. This artifact maps what exists without producing the final walkthrough.

# Files And Modules

| Area | File path | Apparent purpose | Important dependencies | Evidence label |
| --- | --- | --- | --- | --- |
| Build graph | `CMakeLists.txt` | Defines layered libraries, executables, CTest registrations, warning/sanitizer defaults | CMake, CTest, MSVC/Unix presets | observed |
| Build presets | `CMakePresets.json` | Windows MSVC and Linux GCC/Clang build/test lanes | CMake 3.16+, Visual Studio/Ninja | observed |
| Ingest | `include/ingest/run_pipeline.h`, `src/ingest/run_pipeline.cpp` | Builds `ResolvedObservationWorld` and `ResolvedRuntimeInput` from data/layout artifacts | `DataSpec`, `LayoutSpec`, alignment, topology catalog | observed |
| Data facade | `include/ingest/data_view.h`, `src/ingest/data_view.cpp` | Exposes row-aligned columns, axes, topologies, and neighborhood queries | `AlignedBundle`, `TopologyCatalog`, `NeighborhoodQuery` | observed |
| Plan schema | `include/sweep/plan_spec.h`, `src/ingest/plan_spec.cpp` | JSON-facing strategy graph, params, bindings, sweep, run, and dynamic execution schema | `ExecSettings`, metrics, JSON parser | observed |
| Operator catalog | `include/ops/op_registry.h`, `src/ops/op_registry.cpp`, `src/ops/op_module_*.cpp` | Registers op metadata, params, warmup, axes, topology needs, and module grouping | `IndexSchema`, `TopologySpec`, locality subject | observed |
| Engine | `include/engine/*.h`, `src/engine/*.cpp` | Compiles PlanSpec DAGs, evaluates row buffers, and executes strategy FSM | `CompiledProgram`, `DataView`, `StrategyExecutor` | observed |
| Sweeps | `include/sweep/plan_sweep.h`, `src/sweep/*.cpp` | Runs parameter searches, candidate policies, ranking, diversity, Pareto, posterior, reports | `PlanSpec`, `BacktestResult`, sweep engines | observed |
| Panels | `include/sweep/panel_runner.h`, `src/sweep/panel_runner.cpp` | Runs one PlanSpec across multiple aligned entities and ranks results | `DataView`, `PlanRunSpec`, `PanelResult` | observed |
| Service | `src/service/*.h`, `src/service/*.cpp` | Hosted session, artifact, job, quota, protocol, network, and result lifecycle | TCP framing, job scheduler, worker pool | observed |
| App surface | `app/cli`, `app/commands`, `app/runtime`, `app/interactive`, `app/reporting` | Parses commands, builds runtime sessions, prints reports, runs tutorials | Core libraries, contract support | observed |
| Contracts | `src/contracts/*.cpp`, `include/contracts/contract_suite.h` | Deterministic architecture and regression contract runner | Runtime, service, topology, app support | observed |
| App tests | `app/tests/*.cpp` | CLI/runtime/report/app workflow tests | `sugar_app_support` | observed |
| Demos | `graphs/demos/`, `data/` | Layouts, PlanSpecs, and CSV fixtures for quickstart, sweeps, topology, dynamic execution, panels | CLI and runtime | observed |
| Prior docs | `docs/architecture_*.md`, `docs/gpu_support_roadmap.md` | Existing source-backed architecture and GPU notes | Source files above | observed |

# Public Entry Points

| Entry point | File | How invoked | Expected inputs | Expected outputs | Evidence label |
| --- | --- | --- | --- | --- | --- |
| `sugarbot` | `app/entrypoints/main.cpp` | CLI executable | argv parsed by `parse_app_command` | command-specific stdout/stderr and exit code | observed |
| `sugar_contracts` | `app/entrypoints/contract_main.cpp` | CLI executable, optional `--contract-section` | contract section keys | pass/fail process exit | observed |
| `sugar_server` | `app/entrypoints/server_main.cpp` | TCP service executable | server CLI args | framed protocol server | observed |
| `sugar_client` | `app/entrypoints/client_main.cpp` | TCP client executable | client CLI args | framed protocol requests/responses | observed |
| CTest | `CMakeLists.txt` | `ctest` presets or CMake test runner | built executables | named contract/app tests | observed |
| Demo commands | `graphs/demos/README.md` | `sugarbot run <layout> <planspec> <csv>` | checked-in layout, PlanSpec, CSV | run/sweep/panel/topology/dynamic reports | observed |

# Major Functions And Classes

| Symbol | File path | Role | Inputs | Outputs | Side effects / called functions |
| --- | --- | --- | --- | --- | --- |
| `build_resolved_observation_world` | `include/ingest/run_pipeline.h`, `src/ingest/run_pipeline.cpp` | Builds canonical observation world | `DataSpec` or `LayoutSpec`, CSV path, align config, topologies | `ResolvedObservationWorld` | Reads CSV, preprocesses, aligns, mounts topology catalog |
| `bind_resolved_runtime_target` | `include/ingest/run_pipeline.h`, `src/ingest/run_pipeline.cpp` | Chooses primary series/column | observation world, primary column, target spec | `ResolvedRuntimeInput` | Validates target binding |
| `DataView` | `include/ingest/data_view.h` | Runtime facade for aligned data | `AlignedBundle` | spans, axes, topologies, queries | No ownership; resolves topology/locality |
| `PlanSpec` | `include/sweep/plan_spec.h` | Strategy graph and run schema | JSON | typed C++ struct | Parsed by `parse_plan_spec_json` |
| `OperatorRegistry` | `include/ops/op_registry.h` | Op catalog and capability metadata | op entries | lookup/list/description | Mutated by `add`; built by `make_default_registry` |
| `compile_plan` | `include/engine/plan_compiler.h`, `src/engine/plan_compiler.cpp` | Compiles PlanSpec into executable strategy plan | PlanSpec, registry, compile env | `CompileResult` | Validates ops, types, dynamic execution metadata |
| `CompiledProgram` | `include/engine/compiled_program.h` | Row-aligned buffer execution contract | compiled numeric/bool nodes | slots and evaluation order | None; data object |
| `evaluate_strategy_plan_program` | `include/engine/program_evaluator.h`, `src/engine/program_evaluator.cpp` | Materializes signal buffers | DataView, primary ColumnRef, StrategyPlan | `ProgramSignalOutputs` | Pure runtime buffer evaluation; errors on unsupported shapes |
| `StrategyExecutor::run` | `include/engine/strategy_executor.h`, `src/engine/strategy_executor.cpp` | Finite-state trade simulation | DataView, primary, plan, settings, optional cancel callback | `BacktestResult` | Reads price/signals, honors cancel callback |
| `run_custom_plan_spec` | `include/sweep/plan_sweep.h`, `src/sweep/plan_sweep.cpp` | Main DataView-first PlanSpec runtime | DataView, primary, PlanSpec, runtime options | `BacktestResult` or `PlanSweepReport` | Writes report to stream |
| `run_ranked_multi_entity_custom_planspec_over_dataview` | `include/sweep/panel_runner.h`, `src/sweep/panel_runner.cpp` | Panel/entity ranker | DataView, PlanSpec, primary column, top K | `PanelResult` | Iterates series and records failures |
| `ServerRuntime` | `src/service/server_runtime.h`, `src/service/server_runtime.cpp` | Hosted job lifecycle | sessions, submit requests, queued jobs | job status/results | Mutates job/result/artifact stores |
| `ProtocolDispatcher` | `src/service/protocol_dispatch.h`, `src/service/protocol_dispatch.cpp` | JSON protocol router | `ProtocolRequest` or JSON text | `ProtocolResponse` or JSON text | Upload staging, abuse tracking, runtime calls |

# Data Objects And Schemas

| Object | File path | Meaning | Example shape | Evidence label |
| --- | --- | --- | --- | --- |
| `DataSpec` | `include/ingest/data_spec.h` | Input data schema | CSV columns, time/entity axes, profile | observed |
| `LayoutSpec` | `include/ingest/layout_spec.h` | DataSpec plus topology declarations | layout JSON for demos | observed |
| `FrameSet` | `include/ingest/numeric_frame.h` | Preprocessed frames before alignment | one or more series frames | observed |
| `AlignedBundle` | `include/ingest/align.h` | Row-aligned series bundle | aligned columns and index schema | observed |
| `TopologyCatalog` | `include/ingest/topology_spec.h` | Mounted topology providers | built-in and declared topology specs | observed |
| `ColumnRef` | `include/ingest/data_view.h` | Runtime column address | `{series, col}` | observed |
| `PlanSpec` | `include/sweep/plan_spec.h` | Plan graph/run schema | params, numeric nodes, bool nodes, bindings, sweep, execution | observed |
| `CompiledProgram` | `include/engine/compiled_program.h` | Slot-based row-buffer DAG | numeric/bool nodes, evaluation order, dynamic metadata | observed |
| `BacktestResult` / `RunReport` | `include/sweep/metrics.h` | Execution and interpreted result facts | trades, PnL, drawdown, provenance, dynamic summary | observed |
| `PlanSweepReport` | `include/sweep/plan_sweep.h` | Sweep result and diagnostics | top candidates, diversity, geometry, Pareto, posterior, trace | observed |
| `PanelReport` | `include/sweep/panel_runner.h` | Multi-entity ranking report | top rows, failures, counts | observed |
| Protocol messages | `src/service/protocol_framing.h` | Hosted request/response envelope | JSON/framed protocol | observed |

# Side Effects

| Side effect | Location | Description | Evidence label |
| --- | --- | --- | --- |
| File reads | ingest/app/service | CSV, layout JSON, PlanSpec JSON, demo artifacts | observed |
| File writes | app/service/reporting | generated reports/artifacts, service upload/result storage, build aliases | observed |
| Network calls | `src/service/network_*.cpp` | TCP server/client protocol transport | observed |
| Logging/stdout/stderr | app commands, service, sweep progress | human and JSON report output, diagnostics, security log callback | observed |
| Global/static state | op catalog builders, empty topology catalog helper | mostly immutable helpers after construction | observed |
| Concurrency | `JobWorkerPool`, service contracts | background worker jobs and cancellation | observed |
| Cancellation | `StrategyExecutor`, sweep options, service worker paths | cooperative cancel callbacks and terminal states | observed |

# Tests And Examples

| Test/example | Location | Validation behavior | Evidence label |
| --- | --- | --- | --- |
| Contract suite executable | `src/contracts/contract_suite.cpp`, `CMakeLists.txt` | Sectioned contracts for runtime, topology, service, sweep, numeric portability, hygiene | observed |
| App suite executable | `app/tests/*.cpp`, `CMakeLists.txt` | CLI parsing, app runtime, reports, preflight, interactive behavior | observed |
| Hosted service contracts | `src/contracts/contract_suite_service*.cpp` | Sessions, upload/path abuse, quotas, result ownership, cancellation, concurrency, TCP protocol | observed |
| Runtime contracts | `src/contracts/contract_suite_runtime_*.cpp` | compile bridge, locality, topology, dynamic execution, operator axes, hygiene | observed |
| Demo plans | `graphs/demos/README.md` | quickstart, NVDA, BTC, Nashville panel, sweep engines, topology readiness, dynamic execution | observed |
| Stability artifacts | `graphs/stability/` | text/JSON report outputs from previous runs | observed |

# Unknowns

| Unknown | Impact | Evidence gap |
| --- | --- | --- |
| Current performance bottlenecks | GPU and optimization priorities remain inferred | No benchmark run was executed for this harness |
| Public deployment threat model | Hosted service docs can describe controls but not production security posture | No deployment spec or auth/TLS layer observed |
| Compatibility promises for JSON schemas | Docs can identify schema surfaces but not versioning policy | No formal schema/versioning governance file observed |
| Numerical tolerances for future GPU parity | Roadmap can recommend but not assert tolerances | No GPU backend exists in active code |
