# src/AGENTS.md

Purpose: implementation-layer supplement to the root [AGENTS.md](C:/Users/Mel/source/repos/Absolute-Convergence/sugar_bot/sugar_bot_cpp_upload/AGENTS.md).
Keep this file focused on implementation boundaries and the contract harness.

## 0) Scratch Memory (Update First)
- This section is scratch memory and must be updated after every code change.
- Root `AGENTS.md` is authoritative; this file adds `src/`-local implementation rules only.
- `src/` runtime is DataView-native end to end and must broaden additively instead of spawning sibling runtimes.
- `run_pipeline.cpp` is the observation-to-`DataView` seam; legacy embedded observation support is compatibility-only.
- `align.cpp` preserves truthful row-domain semantics; non-temporal multi-series and mixed temporal/non-temporal alignment still fail explicitly.
- Real runtime load today is narrow by design: row-wise execution plus built-in temporal/spatial/spatiotemporal topology adapters and the current locality ops.
- Broader topology families in `TopologySpec` are metadata-bearing until runtime adapters are implemented.
- `observation_capability.cpp`, `plan_capability.cpp`, `plan_sweep.cpp`, `program_evaluator.cpp`, and `run_manifest.cpp` are already load-bearing control-plane seams and should stay aligned.
- The contract suite in `src/contract_suite.cpp`, `src/contract_suite_data.cpp`, and `src/contract_suite_runtime.cpp` is the canonical harness for seam testing.
- First harness-shape step is complete: wrapper section keys `runtime_design_loads`, `runtime_overload_rejection`, and `metadata_preservation` now map onto existing focused sections without changing the default all-sections suite.
- The wrapper sections are now also registered in CTest as `sugar_contract_runtime_design_loads`, `sugar_contract_runtime_overload_rejection`, and `sugar_contract_metadata_preservation`.
- Focused section `locality_design_loads` now covers current locality execution design load only: built-in spatial locality, built-in spatiotemporal locality, and explicit `subject_series` redirection on the current runtime.
- `locality_design_loads` is also registered directly in CTest as `sugar_contract_locality_design_loads`.
- Focused section `locality_overload_rejection` now covers current locality overload rejection only: missing named topology, unresolved `subject_series`, and topology-family mismatch on the current runtime.
- `locality_overload_rejection` is also registered directly in CTest as `sugar_contract_locality_overload_rejection`.
- Focused section `locality_metadata_preservation` now covers current locality/topology metadata preservation only: semantic topology artifact provenance plus unresolved/resolved locality-subject provenance in plan-capability manifest snapshots.
- `locality_metadata_preservation` is also registered directly in CTest as `sugar_contract_locality_metadata_preservation`.
- `src/contract_suite_runtime.cpp` now carries shared internal locality PlanSpec builders for the three locality load classes; extend those helpers before adding more copy-pasted locality fixtures, and keep the canonical locality node id `mean` stable for manifest subject ids.
- Focused section `topology_design_loads` now covers current built-in topology resolution design load only: temporal, spatial, and spatiotemporal named built-ins on the DataView runtime.
- `topology_design_loads` is also registered directly in CTest as `sugar_contract_topology_design_loads`.
- Focused section `topology_overload_rejection` now covers current built-in topology resolution overload rejection only: missing topology name, missing required time/spatial payloads, and present-but-unimplemented semantic topology families.
- `topology_overload_rejection` is also registered directly in CTest as `sugar_contract_topology_overload_rejection`.
- Focused section `topology_metadata_preservation` now covers current topology metadata preservation only: missing built-in topology usage snapshots and semantic topology artifact/query metadata in plan-capability manifest snapshots.
- `topology_metadata_preservation` is also registered directly in CTest as `sugar_contract_topology_metadata_preservation`.
- Focused section `compile_bridge_design_loads` now covers current compile/runtime bridge design load only: supported locality plans executing through `run_custom_plan_spec(...)` plus explicit `subject_series` redirection through the compile/evaluate bridge.
- `compile_bridge_design_loads` is also registered directly in CTest as `sugar_contract_compile_bridge_design_loads`.
- Focused section `compile_bridge_overload_rejection` now covers current compile/runtime bridge overload rejection only: missing named topology, unresolved `subject_series`, topology-family mismatch, and unsupported-op compile failure.
- `compile_bridge_overload_rejection` is also registered directly in CTest as `sugar_contract_compile_bridge_overload_rejection`.
- There is currently no distinct metadata-preservation lane for the compile/runtime bridge; metadata/provenance assertions belong to plan-capability and topology/locality metadata sections unless the bridge itself grows a real metadata surface.
- The load-rated wrapper groups should stay narrow and focused; do not re-add broad mixed sections like `compile_bridge_runtime` or `plan_capabilities` into the wrapper lanes without a strong reason.
- Next test work should add more wrapping/sectioning and load-rated seam tests, not treat future-proof metadata seams as fully implemented runtime.
- Prefer the test pattern `design load / overload rejection / non-goal preservation`.

## 1) Implementation Boundaries
- `plan_compiler.cpp` emits templates.
- `instantiate_compiled_plan(...)` resolves executable storage.
- `program_evaluator.cpp` executes over `DataView`.
- `strategy_executor.cpp` consumes evaluated buffers.
- `plan_sweep.cpp` is compile/runtime bridge and sweep orchestration.
- `op_module_*.cpp` register metadata and warmup.
- `op_kernels_*.cpp` own kernel behavior.

## 2) Rules For Changes In `src/`
- Keep behavior explicit and deterministic.
- Keep app/reporting logic out of algorithmic modules.
- Do not reintroduce blanket temporal assumptions; axis/topology requirements must stay explicit.
- Add new capability through the compiled-program/DataView machine, not by reviving compatibility graph wrappers.
- Keep locality growth on top of the existing named-topology and subject-resolution seams.

## 3) Testing Guidance
- Prefer synthetic fixtures in the contract suite for invariant testing.
- Add focused contract sections instead of enlarging monoliths.
- Use app tests only when the change affects CLI/reporting surface wiring.
- When touching runtime-seam behavior, keep diagnosis, compile/runtime validation, execution, and manifest provenance in sync.
