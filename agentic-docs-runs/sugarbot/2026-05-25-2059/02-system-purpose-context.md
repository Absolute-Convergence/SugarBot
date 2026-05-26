---
artifact_id: "02-system-purpose-context"
harness_id: "agentic-docs.system-purpose-context"
library_version: "1.0.0"
subject: "SugarBot"
scope: "Active SugarBot runtime, app, service, contracts, demos, and architecture docs"
generated_at: "2026-05-25T21:00:28.8150479-04:00"
inputs:
  - "01-codebase-inventory.md"
  - "docs/architecture_insight_summary.md"
  - "docs/architecture_executive_deep_dive.md"
confidence_policy: "observed | inferred | speculative labels required"
review_state: "draft"
---

# System Purpose

One-sentence purpose: SugarBot is a deterministic, artifact-driven strategy execution platform that turns explicit observation data and PlanSpec logic into validated backtest, sweep, panel, report, or hosted-service results. Label: inferred from observed source structure.

Expanded purpose: The codebase separates observation facts from strategy facts. CSV/layout artifacts become a row-aligned `DataView`; PlanSpec JSON becomes a typed computation graph; the engine compiles and evaluates that graph into signal buffers; the executor simulates trade state; reports and hosted/service surfaces project the result. Label: observed in `run_pipeline.h`, `data_view.h`, `plan_spec.h`, `compiled_program.h`, `program_evaluator.h`, `strategy_executor.h`, and `plan_sweep.h`.

Problem solved: SugarBot reduces ambiguity in analytical strategy runs by making data schema, target binding, operator capability, topology needs, sweep policy, and report provenance explicit before producing results. Label: inferred from observed validation-heavy runtime and contract suite.

# Operating Context

| Context part | Description | Confidence | Evidence |
| --- | --- | --- | --- |
| Upstream inputs | CSV files, `DataSpec`/`LayoutSpec`, PlanSpec JSON, topology declarations, service protocol uploads | observed | `include/ingest/run_pipeline.h`, `graphs/demos/README.md`, `src/service/protocol_dispatch.h` |
| Runtime core | `ResolvedObservationWorld`, `ResolvedRuntimeInput`, `DataView`, `PlanSpec`, `OperatorRegistry`, `CompiledProgram`, `StrategyExecutor` | observed | active headers under `include/ingest`, `include/engine`, `include/sweep`, `include/ops` |
| Downstream outputs | `BacktestResult`, `RunReport`, `PlanSweepReport`, `PanelReport`, `EvaluationReport`, text/JSON reports, hosted job results | observed | `include/sweep/metrics.h`, `include/sweep/plan_sweep.h`, `include/sweep/panel_runner.h`, service result store |
| Users/operators | CLI users, maintainers, hosted service clients, test/contract runners | inferred | app entry points and protocol service are present; no user taxonomy document observed |
| Dependent systems | Local filesystem, CMake/CTest, optional TCP client/server, Windows/Linux build toolchains | observed | `CMakeLists.txt`, `CMakePresets.json`, `src/service/network_*.cpp` |
| Likely business process | Run strategy experiments, compare parameter candidates, rank panels/entities, inspect topology/dynamic execution reports | inferred | demos and report projections show these workflows |

# Value Proposition

| Value area | What the system absorbs or reduces | Confidence | Evidence |
| --- | --- | --- | --- |
| Schema discipline | Converts loosely authored CSV/JSON artifacts into validated runtime data structures | observed | `build_resolved_observation_world`, `bind_resolved_runtime_target` |
| Strategy reuse | One PlanSpec runtime supports single runs, sweeps, panels, demos, app commands, and hosted jobs | observed | `run_custom_plan_spec`, panel runner, service runtime |
| Diagnostic safety | Plan/report/contract paths expose invalid capabilities before execution or in structured reports | observed | `plan_sweep_runtime_validation.cpp`, contracts, app diagnostics |
| Search scale | Sweep engines reuse the same execution path while changing candidate selection policy | observed | `PlanSweepEngineKind`, `make_sweep_index`, `PlanSweepReport` |
| Operational packaging | Hosted service adds sessions, uploads, quotas, scheduling, cancellation, and result retrieval | observed | `ServerRuntime`, `ProtocolDispatcher`, service contracts |
| Future acceleration | Row-aligned buffers and candidate/entity batching create clear GPU extension points | inferred | `CompiledProgram` buffer contract, sweep/panel architecture |

# Boundary Of Responsibility

| Owns | Does not own | Assumes |
| --- | --- | --- |
| Parsing and validating SugarBot data/layout/PlanSpec artifacts | External market data correctness | CSV inputs are supplied by callers and are meaningful for the desired analysis |
| Building aligned observation worlds and topology catalogs | Broker/exchange connectivity or live trading | Backtesting inputs are historical/offline observations |
| Compiling and evaluating typed operator graphs | Arbitrary runtime code execution from user plans | PlanSpec nodes map to registered operators |
| Simulating finite-state strategy behavior | Portfolio accounting beyond implemented executor settings | `ExecSettings` captures the intended timing/cost/reversal rules |
| Sweep, panel, and report projection over runtime results | Deciding business merit of a strategy | Ranking objectives express what to optimize |
| Hosted job/session lifecycle | Public internet security posture beyond observed controls | Deployment chooses appropriate bind/auth/TLS policy |

# Confidence Table

| Claim | Evidence | Confidence |
| --- | --- | --- |
| SugarBot uses a layered CMake architecture with ingest, ops, engine, sweep, service, contracts, app, and executables | `CMakeLists.txt` library/executable declarations | observed |
| `DataView` is the active runtime observation facade | `DataView` class exposes row count, column spans, axes, topology resolution | observed |
| `PlanSpec` is the strategy/run schema | `PlanSpec` contains nodes, params, bindings, sweep, run, execution fields | observed |
| The executor is deliberately narrow and consumes final signal buffers | `StrategyExecutor::run` signature and comments in `strategy_executor.h` | observed |
| Hosted service behavior wraps the same runtime rather than defining a separate strategy model | service depends on core libs and contracts validate hosted unified reports | inferred |
| GPU acceleration should start in evaluator/sweep/panel layers | row-buffer and candidate/entity architecture; GPU roadmap | inferred |
| Public hosted deployment is production-secure as-is | no auth/TLS deployment evidence in inspected source | speculative; not claimed |

# Questions For Maintainers

| Question | Why it matters |
| --- | --- |
| Which generated/stability outputs are considered canonical examples versus historical artifacts? | The final explainer can link only to durable examples if maintainers identify them |
| Is hosted service intended for local/team LAN usage only, or eventual public exposure? | Security language and deployment cautions depend on this |
| Should PlanSpec schema evolution be documented as a versioning policy? | The code already behaves like multiple public schemas |
| Which benchmark workloads should define future GPU success? | The roadmap needs observed bottleneck evidence before engineering investment |
