---
artifact_id: "04b-behavioral-dynamics-inventory"
harness_id: "agentic-docs.behavioral-dynamics-inventory"
library_version: "1.0.0"
subject: "SugarBot"
scope: "Live/static visualization decisions for the SugarBot runtime explainer"
generated_at: "2026-05-25T21:00:28.8150479-04:00"
inputs:
  - "03-transformation-map.md"
  - "04-call-graph-control-flow.md"
  - "graphs/demos/README.md"
  - "graphs/demos/dynamic_execution/README.md"
  - "graphs/demos/topology_readiness/README.md"
  - "docs/gpu_support_roadmap.md"
confidence_policy: "observed | inferred | speculative labels required"
review_state: "draft"
---

# Behavioral Dynamics Ledger

| Operation | Code location | State variables | Trigger | Ordered steps | Branches | Observable trace | Visualization class | Why live | Fidelity requirement | Static fallback | Risk if misleading |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Artifact pipeline | `src/ingest/run_pipeline.cpp`, `include/ingest/data_view.h`, `include/sweep/plan_spec.h` | artifact kind, row domain, target binding, compiled state, report state | `sugarbot run <layout> <planspec> <csv>` | CSV/layout -> frames -> aligned bundle -> `DataView` -> `CompiledProgram` -> report | layout vs embedded compatibility, primary target binding | `graphs/demos/quickstart_ohlcv.*`, `data/sample.csv` | animated-sequence | Shows irreversible state shape changes | trace-backed | Pipeline nodes with evidence labels | Suggesting raw CSV flows directly into executor |
| Compile-buffer-execute | `include/engine/compiled_program.h`, `src/engine/program_evaluator.cpp`, `src/engine/strategy_executor.cpp` | DAG node, slot, buffer row, signal, position | PlanSpec compile and run | node -> slot -> numeric/bool buffers -> entry/exit/gate -> FSM metrics | missing allow_trade defaults true, warmup false/NaN, fill mode | Source-backed toy trace from compiled-program contract and executor signature | interactive-state-machine | Motion separates graph evaluation from trade lifecycle | source-backed toy | DAG, buffer, FSM panels visible at once | Implying executor understands the full DAG |
| Dynamic routing | `PlanExecutionSpec`, `CompiledDynamicExecutionMetadata`, evaluator masks/gates | route condition, active subgraph, mask, gated output | declared dynamic PlanSpec | declare -> compile -> evaluate condition -> mask inactive rows -> gate output -> report | constant route, row-varying route, topology route | `graphs/demos/dynamic_execution/g10_*`, dynamic README | comparison-replay | Shows why dynamic means bounded routing, not graph mutation | trace-backed | Route/mask/output table | Implying arbitrary runtime graph construction |
| Sweep search | `src/sweep/*`, `include/sweep/plan_sweep.h` | candidate index, proposal, observation, score, Top-K, Pareto, trace digest | PlanSpec `sweep.enabled` | expand params -> propose -> run candidate -> observe -> rank/filter -> report | exhaustive, Monte Carlo, coarse/fine, Bayesian posterior | `graphs/demos/sweep_engines/i_advanced_report_projection_sweep.planspec.json --report trace` | trace-replay | Search behavior is sequential and accumulative | trace-backed | Candidate ledger with Top-K rows | Implying every engine explores candidates in the same order |
| Panel execution | `src/sweep/panel_runner.cpp`, `include/sweep/panel_runner.h` | entity, primary binding, success/failure, ranking score | Plan run scope or panel command | iterate series -> bind primary -> run PlanSpec -> score -> retain Top-K | missing primary column, execution failure, cancellation | `graphs/demos/nashville_housing_panel.*`, `data/Nashville-Real-Estate-Data-clean.csv` | interactive-scrubber | Shows same plan over multiple entities and visible failures | trace-backed | Entity matrix with counts | Hiding failed entities from the ranking story |
| Topology locality | `include/ingest/neighborhood_query*.h`, `src/ops/op_module_topology.cpp` | subject, topology family, query kind, neighbors, weights, provenance | topology operator node | resolve subject -> resolve topology -> query neighbors -> aggregate -> report provenance | relational time-valid, hierarchy, semantic | `graphs/demos/topology_readiness/*`, `data/topology_readiness/*` | interactive-scrubber | Neighborhood membership is easier to inspect interactively | trace-backed | Topology usage table | Suggesting topology is a global feature toggle instead of a runtime capability |
| Hosted lifecycle | `src/service/server_runtime.cpp`, `src/service/protocol_dispatch.cpp`, service contracts | session, upload, validation, job state, queue, cancellation, result | protocol request | connect -> upload -> validate -> submit -> queue -> execute -> result/cancel -> cleanup | validate-only vs execute, rejection, cancellation, ownership | `src/contracts/contract_suite_service*.cpp` | animated-sequence | Queue/state transitions explain hosted controls | source-backed toy | Lifecycle timeline table | Implying hosted service changes run semantics |
| GPU lane | `docs/gpu_support_roadmap.md`, `include/engine/compiled_program.h`, sweep/panel code | backend, batch, device, parity gate | future optional GPU backend | CPU canonical -> backend interface -> row kernels -> batched sweeps/panels -> sharding | 1x RTX 3060, 6x non-parallel 3060s, 4x A100 | GPU roadmap document | static | Current code has no GPU backend; static roadmap avoids overclaiming | inferred roadmap | CPU/GPU lane table | Implying GPU support exists today |

# Live Visualization Shortlist

| Priority | Visualization | Reason |
| --- | --- | --- |
| 1 | Dynamic routing comparison replay | Highest misunderstanding risk: declared routing can be mistaken for graph mutation |
| 2 | Sweep search trace replay | Candidate proposal/observation/ranking is inherently sequential |
| 3 | Compile-buffer-execute state machine | Clarifies the core extension boundary between evaluator and executor |
| 4 | Panel execution entity matrix | Makes partial success and ranking visibility concrete |
| 5 | Topology locality scrubber | Best way to show subject/query/provenance together |

# Static-Only Decisions

| Operation | Decision | Reason |
| --- | --- | --- |
| CMake library ownership | static | Dependency ownership is clearer as a graph/table |
| Operator registry metadata catalog | static with light reveal | Metadata consumers matter more than temporal state |
| Stewardship guidance | static | Rules should be scan-friendly and not animated |
| GPU lane v1 | static | GPU is a roadmap, not active runtime behavior |

# Trace Gaps

| Desired visual | Missing trace/evidence | Recommended control |
| --- | --- | --- |
| Exact compile-buffer-execute replay | No checked-in per-row evaluator trace artifact | Add optional diagnostic feature dump or tiny generated JSON trace |
| Exact hosted lifecycle replay | Contracts exist, but no small visual trace JSON | Add a service protocol trace fixture from a validation/submit/cancel flow |
| Exact sweep engine comparison | Demo commands exist; generated report snippets may drift | Capture compact trace JSON fixtures for each sweep engine |
