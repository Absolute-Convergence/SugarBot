# AGENTS.md - SugarBot Agent Instructions

This file is the repo-level constitution for SugarBot agents.
Folder-local `AGENTS.md` files supplement this file and should stay short.

---

## 0) Scratch Memory (Update First)
- This section is scratch memory and must be updated after every code change.
- 2026-03-14: alignment now rejects trailing schema-size mismatches, legacy embedded single-frame inputs preserve distinct series-key provenance when no entity axis exists, and pre-alignment observation row_count now derives from the max entity row_count instead of unordered map iteration order.
- AGENTS docs were compressed on 2026-03-13 for lighter-model handoff: this root file is authoritative; `include/`, `src/`, and `app/` AGENTS add only local boundary rules.
- `README.md` was substantially refreshed on 2026-03-14 to match the current DataView-first runtime, shared capability reporting, build presets, and load-rated contract lanes; preserve its section layout if editing content again.
- Project-identity wording may acknowledge broader world-modeling direction, but lighter models should stay grounded in the current DataView-first runtime and the explicit capability load ratings below.
- Canonical engine path is `NumericFrame/FrameSet -> align -> DataView -> CompiledProgram -> ProgramEvaluator -> StrategyExecutor`.
- No stable engine contract may depend on a type narrower than `DataView`.
- `DataView` / `AlignedBundle` are the one stable indexed observation runtime family; temporal, spatial, and spatio-temporal capability must broaden additively there, not through sibling runtimes.
- Compiled-program execution remains row-wise over the active observation domain; broaden axes, locality, and query services, not the execution machine.
- `NeighborhoodQuery` is the canonical locality/query seam for future spatial and spatio-temporal growth.
- `PlanSpec` remains strategy DSL only. `DataSpec`, `IndexSchema`, and `RunManifest` own observation policy, axis roles, alignment intent, and provenance.
- `PlanSpec.source_path` anchors compatibility path resolution; legacy `series_inputs` survive only inside `run_pipeline` compatibility helpers.
- `ResolvedRuntimeInput` in `run_pipeline` is the shared runtime package for app-supplied observations and legacy embedded-observation compatibility.
- `StrategyExecutor`, menu/template/panel/custom execution, and `run_custom_plan_spec(...)` are DataView-first only.
- Custom sweep compiles once through `compile_plan_template(...)` and varies bound params through `instantiate_compiled_plan(...)`.
- `TimeKey` is `int64_t`; ISO8601 normalizes to UTC epoch-seconds with intraday fidelity preserved.
- `align_frames(...)` remains calendar/time alignment only. Single-series non-temporal inputs are supported; mixed temporal/non-temporal inputs and non-temporal multi-series alignment still fail explicitly.
- Row-domain split is complete: `row_count()` is not derived from time payload, and `time()` is a semantic compatibility alias rather than raw-storage truth.
- Spatial-only `DataSpec` is valid without `time_col`; preprocess preserves source order and leaves semantic time empty.
- Observation capability (`OC1`-`OC4`), topology foundation (`TP1`-`TP7`), and locality-subject foundation (`LS1`-`LS4`) are complete as foundation/reporting work.
- Real runtime load today is intentionally narrow: row-wise ops, temporal ops, minimal cross-series ops, and current locality ops (`spatial.radius_mean`, `st.radius_mean`) over built-in temporal/spatial/spatiotemporal topologies.
- Control-plane load today is broader than runtime load: observation diagnosis, plan diagnosis, runtime validation, and manifest/report provenance already carry more future-facing seams than execution does.
- Metadata-bearing only today: non-built-in topology families/artifacts, future graph/semantic/probabilistic locality, and future Python/Pandas or SQL integrations.
- `RunManifest` preserves additive topology artifact provenance, topology usage, capability outcomes, and resolved locality-subject provenance.
- Contract suite is DataView-native and split across `src/contract_suite.cpp`, `src/contract_suite_data.cpp`, and `src/contract_suite_runtime.cpp`; default behavior still runs all sections.
- First load-rated harness step is now in place: contract-suite wrapper sections `runtime_design_loads`, `runtime_overload_rejection`, and `metadata_preservation` select existing leaf sections without changing the default all-sections run.
- CTest now exposes the first load-rated contract wrapper entries directly: `sugar_contract_runtime_design_loads`, `sugar_contract_runtime_overload_rejection`, and `sugar_contract_metadata_preservation`.
- Focused runtime section `locality_design_loads` now exists for the currently real locality execution seam; `runtime_design_loads` includes it.
- CTest now also exposes the focused locality design-load entry directly as `sugar_contract_locality_design_loads`.
- Focused runtime-overload section `locality_overload_rejection` now exists for the same seam; it freezes explicit rejection for missing named topology, unresolved `subject_series`, and topology-family mismatch without broadening runtime promises.
- `runtime_overload_rejection` now includes `locality_overload_rejection`, and CTest exposes it directly as `sugar_contract_locality_overload_rejection`.
- Focused metadata section `locality_metadata_preservation` now exists for the same seam; it freezes manifest/topology-usage preservation for semantic topology artifact provenance plus unresolved/resolved locality-subject provenance without treating those paths as runtime execution.
- `metadata_preservation` now includes `locality_metadata_preservation`, and CTest exposes it directly as `sugar_contract_locality_metadata_preservation`.
- The locality seam harness now shares small internal builders for common `spatial.radius_mean` / `st.radius_mean` PlanSpec shapes and standard exit binding; those helpers intentionally preserve the canonical locality node id `mean` so manifest/topology-usage subject ids stay stable.
- Focused section `topology_design_loads` now exists for the built-in topology resolution seam; it freezes temporal, spatial, and spatiotemporal named built-in resolution on the current runtime without broadening that seam into graph or custom-topology execution.
- `runtime_design_loads` now includes `topology_design_loads`, and CTest exposes it directly as `sugar_contract_topology_design_loads`.
- Focused section `topology_overload_rejection` now exists for the same seam; it freezes explicit `DataView::resolve_topology(...)` rejection for missing named topology, missing required time/spatial payloads, and present-but-unimplemented semantic topology families.
- `runtime_overload_rejection` now includes `topology_overload_rejection`, and CTest exposes it directly as `sugar_contract_topology_overload_rejection`.
- Focused section `topology_metadata_preservation` now exists for the same seam; it freezes topology-usage snapshot preservation for missing built-in topology usage and semantic/custom topology artifact provenance without treating those paths as executable runtime capability.
- `metadata_preservation` now includes `topology_metadata_preservation`, and CTest exposes it directly as `sugar_contract_topology_metadata_preservation`.
- Focused section `compile_bridge_design_loads` now exists for the compile/runtime bridge seam; it freezes supported `run_custom_plan_spec(...)` execution for spatial-only and spatiotemporal locality plans plus explicit `subject_series` redirection through the compile/evaluate bridge.
- `runtime_design_loads` now includes `compile_bridge_design_loads`, and CTest exposes it directly as `sugar_contract_compile_bridge_design_loads`.
- Focused section `compile_bridge_overload_rejection` now exists for the same seam; it freezes explicit bridge rejection for missing named topology, unresolved `subject_series`, topology-family mismatch, and unsupported-op compile failure without any legacy fallback path.
- `runtime_overload_rejection` now includes `compile_bridge_overload_rejection`, and CTest exposes it directly as `sugar_contract_compile_bridge_overload_rejection`.
- Compile/runtime bridge no longer claims a distinct metadata-preservation lane; manifest/topology/locality provenance belongs to plan-capability and topology/locality metadata seams, while the bridge seam is now rated only for real execution design load and explicit overload rejection.
- The load-rated wrapper groups were tightened on 2026-03-14 to avoid false confidence from broad legacy sections: `runtime_design_loads`, `runtime_overload_rejection`, and `metadata_preservation` now prioritize focused seam sections instead of broad mixed integration sections.
- Last observed focused baselines: full `sugar_contracts` 863, `runtime_gates` 380, `operator_axes` 106, `compile_bridge_runtime` 72, `row_storage` 47, `topology_contracts` 53, `axis_queries` 75, `observation_capabilities` 44, `plan_capabilities` 71, `kernel_warmup` 15.
- `sugar_app_tests` baseline is 301 passes.
- Next testing goal: keep the contract infrastructure as the canonical harness, but wrap/section it further and add load-rated seam tests that distinguish design load, overload rejection, and metadata-only preservation.
- Do not introduce a coarse `ExecutionMode` enum in place of axis metadata.
- Legacy graph-era runtime types remain retired under `retirement/legacy_reference/`; `Backtester` stays retired.
- `app/main.cpp` is a thin composition root; `sugar_app_support` is shared by `sugar_Bot` and `sugar_app_tests`; core targets are split into `sugar_ingest`, `sugar_ops`, `sugar_engine`, and `sugar_sweep`.
- Emitted app binary is `sugarbot` with an `SB` alias; user-managed build roots stay under `out/build/...` and `out/install/...`.
- `codex_build/` and `codex_asan/` are disposable agent-only sandboxes; checked-in docs, examples, and runtime assumptions must never depend on them.

---

## 1) Project Identity
SugarBot is a C++ strategy experimentation engine for decision-rule research over tabular observations.
Its direction is broader world-modeling over structured observation, locality, and logical exploration, but near-term work must stay grounded in the current DataView-first compiled-program runtime.
Separation of concerns, modularity, and extensibility are core design constraints.

It is:
- A rule lab for signal composition, logical wiring, execution, and evaluation.
- Designed for inspectability and controlled iteration.

It is not:
- A predictor.
- A black-box model.
- A production trading system.

---

## 2) Canonical Architecture

### Canonical runtime pipeline
`CSV -> RawTable -> DataSpec -> preprocess -> NumericFrame or FrameSet -> align -> DataView -> compile_plan -> CompiledProgram -> StrategyExecutor / sweep`

### Legacy candle compatibility pipeline
`CSV -> CandleSeries -> legacy_candle_frame_adapter -> NumericFrame -> align -> DataView -> canonical runtime` (never reintroduce as hidden assumption) 

### Stable boundary rules
- `PlanSpec` remains declarative strategy DSL only.
- `StrategyExecutor` remains runtime FSM and strategy-agnostic.
- Plan compilation/wiring remains distinct from execution.
- Sweep/orchestration stay strategy-agnostic.
- Compatibility layers may adapt into the canonical path, but they do not define parallel engine contracts.

No layer may reach around another layer's contract.

---

## 3) Contract Invariants

### Data model
- `NumericFrame` owns time plus numeric columns indexed by `ColumnId`.
- All present numeric columns must match `row_count`.
- `FrameSet` requires a shared schema so `ColumnId` meaning stays consistent across entities.
- `NumericFrame` / `FrameSet` may carry optional `IndexSchema` metadata.
- `AlignedBundle` / `DataView` may expose propagated `IndexSchema` metadata.

### Observation metadata
- `DataSpec` is the parsing and normalization policy boundary.
- Axis roles, spatial representation, alignment intent, and future filtering breadcrumbs belong in `DataSpec`/preprocess-side metadata plus `RunManifest`, not in `PlanSpec`.
- `IndexSchema` remains axis metadata only; general topology must not be moved there.

### Runtime program
- Stable engine execution is expressed through `CompiledProgram` + `ProgramEvaluator`, not graph-era `Indicator` / `BoolSignal` object graphs.
- New engine capability should land as DataView-native operators/kernels and compiled-program semantics.
- Operator contracts must keep row-wise axis-agnostic ops distinct from true temporal/spatial/spatiotemporal locality.
- NaN/warmup behavior must stay explicit and contract-tested.

### Compatibility containment
- `CandleSeries` is a compatibility shape, not a stable engine contract.
- Candle compatibility is one-way through `legacy_candle_frame_adapter`.
- Legacy embedded observation inputs remain compatibility-only inside `run_pipeline`.

---

## 4) Current Capability Load Ratings

### Runtime-bearing today
- Row-domain runtime access through `AlignedBundle` and `DataView`.
- Temporal-only, spatial-only single-series, and spatiotemporal observation execution on the same runtime family.
- Current built-in topology resolution for temporal, spatial, and spatiotemporal built-ins.
- Shared locality-subject resolution for the currently supported subject kinds.
- Current locality execution only for `spatial.radius_mean` and `st.radius_mean`.
- Minimal cross-series execution through `ms.diff`, `ms.ratio`, and `ms.zscore`.

### Control-plane-bearing today
- Shared observation capability diagnosis.
- Shared plan capability diagnosis.
- Compile/runtime topology and locality validation.
- Manifest capability snapshots and topology/locality provenance.
- App/reporting surfaces that consume shared diagnostics.

### Metadata-bearing today
- Non-built-in topology families such as relational, semantic, probabilistic, and other future graph-like seams.
- Topology artifact references and external producer provenance.
- Future Python/Pandas preprocessing and future SQL-facing presentation seams.

### Explicit non-goals and current rejections
- No separate temporal, spatial, or spatiotemporal runtime families.
- No general graph engine or dynamic topology execution.
- No silent row-position fallback for unsupported non-temporal multi-series alignment.
- No app-local execution modes standing in for runtime truth.

---

## 5) Testing Direction
- The contract infrastructure is the canonical place to test new seams.
- Keep contract sections small, named, and independently runnable.
- Prefer shared synthetic fixtures/helpers over large monolithic test bodies.
- Use three load classes per seam:
  - design load: what the seam is explicitly meant to carry today
  - overload rejection: what it must reject cleanly and consistently today
  - non-goal preservation: what metadata/provenance it must preserve without pretending to execute
- Do not treat a future-proof seam as a failed runtime feature when the code only promises metadata or control-plane behavior today.
- Prefer contract tests for invariant and differential testing; use app tests mainly for surface-consistency and wiring checks.

---

## 6) Working Style For Agents

### Before editing
1. Identify the exact call flow and minimal file set.
2. Confirm which pipeline is in scope: legacy, frame-first, or both.
3. For broad changes, stage by contract layer and avoid cross-layer rewrites.

### While editing
- Edit only necessary files.
- Keep behavior deterministic unless explicitly changed.
- Preserve compatibility shims when requested.
- For large changes, update the relevant AGENTS docs in the same patch.

### After editing
- Provide a concise diff summary and rationale.
- Provide concrete build/test commands used or not used.

---

## 7) Build, Validation, And Environment
- Windows: MSVC x64.
- Linux/WSL2: Ubuntu x86_64.
- Prefer CMake invocations compatible with existing build directories.
- CTest registration should keep contracts, runtime gates, and app tests separable under `BUILD_TESTING`.
- After public runtime-header layout changes in a codex sandbox, prefer a clean-first rebuild before trusting failures; stale sandbox objects can masquerade as engine bugs.

---

## 8) Licensing And Confidentiality
ALL RIGHTS RESERVED.

- Do not propose redistribution of source or derived proprietary content.
- Prefer concise diffs/patches over large verbatim source dumps.
