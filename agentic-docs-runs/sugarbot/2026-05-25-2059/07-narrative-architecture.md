---
artifact_id: "07-narrative-architecture"
harness_id: "agentic-docs.narrative-architecture"
library_version: "1.0.0"
subject: "SugarBot"
scope: "Narrative structure for a mixed executive/maintainer explainer"
generated_at: "2026-05-25T21:00:28.8150479-04:00"
inputs:
  - "02-system-purpose-context.md"
  - "03-transformation-map.md"
  - "04-call-graph-control-flow.md"
  - "05-invariant-proof-ledger.md"
  - "06-edge-case-risk-ledger.md"
confidence_policy: "observed | inferred | speculative labels required"
review_state: "draft"
---

# Executive Storyline

| Beat | Content |
| --- | --- |
| Opening claim | SugarBot is best understood as a validated strategy runtime, not as a single trading-bot script |
| Problem | Strategy experiments fail quietly when data meaning, target binding, operator capability, and report interpretation are implicit |
| Why now | The codebase now spans topology, dynamic routing, sweeps, panels, hosted jobs, and GPU planning; coherence depends on the shared runtime contract |
| Pipeline | Artifact truth becomes observation truth, PlanSpec truth becomes compiled graph truth, and execution truth becomes report truth |
| Proof | CMake layers, `DataView`, `PlanSpec`, `CompiledProgram`, contracts, and service tests all point to the same architecture |
| Risk | The largest risks are wrong target binding, metadata drift, warmup/NaN shifts, hosted deployment assumptions, and future GPU parity |
| Decision / next step | Extend through the right boundary: ingest for data, registry for ops, evaluator for buffers, sweep policy for search, service for hosted controls |

# Maintainer Storyline

| Beat | Content |
| --- | --- |
| Entry point | Start with `CMakeLists.txt`, then read `run_pipeline.h`, `data_view.h`, `plan_spec.h`, `compiled_program.h`, and `plan_sweep.h` |
| Data shape journey | CSV/layout artifacts become `FrameSet`, `AlignedBundle`, `TopologyCatalog`, `ResolvedRuntimeInput`, and `DataView` |
| Transformation sequence | PlanSpec is parsed, validated through registry metadata, compiled into slots, evaluated into buffers, and consumed by the executor |
| Control flow | CLI and service are adapters; sweeps and panels repeat the same runtime over candidates or entities |
| Proof mechanisms | Contracts encode architecture promises for runtime gates, topology, dynamic execution, service security, concurrency, and numeric portability |
| Edge cases | Target binding, registry/evaluator mismatch, NaN/warmup semantics, partial success, and report schema growth need focused tests |
| Live behavior | Dynamic routing, sweep search, panel execution, topology locality, and hosted job lifecycle deserve trace-backed visual treatment |
| Safe modification guidance | Keep semantics in the core layer that owns them; do not move data validation into app code or hosted policy into executor code |

# Slide Outline

| # | Slide id | Section | Title | Explanatory job | Audience | Visual concept | Visualization class | Proof/risk marker | Takeaway |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | `runtime-not-script` | Thesis | SugarBot Is A Validated Runtime, Not A Script | Reset the mental model | both | claim card plus layer chips | static | proof | The project coheres around one runtime contract |
| 2 | `two-truths` | Architecture | Observation Truth And Strategy Truth Meet At Runtime | Explain the center of gravity | both | two-column bridge | static | proof | `DataView + PlanSpec` is the key boundary |
| 3 | `artifact-pipeline` | Pipeline | Artifacts Become Reports Through A Typed Chain | Show state transformations | both | live pipeline scrubber | animated-sequence | proof | Every run crosses explicit artifact boundaries |
| 4 | `compile-buffer-execute` | Engine | The Compiler Makes Buffers; The Executor Simulates Trades | Separate graph semantics from trade lifecycle | maintainer | DAG to buffer to FSM | interactive-state-machine | proof | The executor should stay narrow |
| 5 | `operators-capabilities` | Capabilities | Operator Metadata Is Runtime Correctness, Not Catalog Text | Explain registry importance | maintainer | metadata hub | static | risk | Bad metadata can make diagnostics lie |
| 6 | `scale-surfaces` | Scale | Sweeps, Panels, And Hosted Jobs Reuse The Same Core | Connect advanced features | both | candidate/entity/job fan-out | static | proof | Advanced surfaces are adapters over the runtime |
| 7 | `dynamic-routing` | Dynamic execution | Dynamic Execution Is Declared Routing Over A Static Graph | Clarify mental model | both | route masks and gated outputs | comparison-replay | caution | Dynamic does not mean arbitrary graph mutation |
| 8 | `sweep-search` | Search | Sweep Search Is A Trace Of Proposals, Observations, And Filters | Explain candidate search behavior | both | sweep trace replay | trace-replay | proof | Sweeps are search policy over the same runtime |
| 9 | `panel-execution` | Panels | Panel Execution Rebinds One Plan Across Many Entities | Show partial success and ranking | both | entity matrix scrubber | interactive-scrubber | caution | Failed entities must stay visible |
| 10 | `topology-locality` | Topology | Topology Operators Resolve A Subject, Query, And Provenance | Explain runtime locality capabilities | maintainer | neighborhood explorer | interactive-scrubber | proof | Topology is a runtime capability contract |
| 11 | `hosted-lifecycle` | Hosted service | Hosted Mode Adds Job Controls Around Local Semantics | Show service state lifecycle | both | job timeline | animated-sequence | caution | Hosted policy should not change run meaning |
| 12 | `gpu-lane` | Future acceleration | GPU Support Belongs Behind The Buffer And Batch Boundaries | Place GPU roadmap in architecture | both | CPU/GPU backend lane | static | caution | Acceleration must preserve CPU semantics |
| 13 | `stewardship` | Guidance | Stewardship Means Keeping Semantics In The Owning Layer | End with modification rules | both | layer/risk checklist | static | risk | The architecture stays strong when boundaries stay explicit |

# Narrative Pacing

| Moment | Pacing guidance |
| --- | --- |
| Slides 1-2 | Slow down enough to replace the "trading bot" mental model with "validated runtime" |
| Slide 3 | Move visually through transformations; avoid overexplaining individual structs |
| Slide 4 | Pause on the compiler/evaluator/executor split because it guides extension decisions |
| Slide 5 | Name the subtle risk: metadata is a correctness surface |
| Slide 6 | Move briskly; show that breadth does not mean multiple runtimes |
| Slide 7 | Slow down on route masks and static equivalence |
| Slide 8 | Let the trace reveal proposals, observations, and filters |
| Slide 9 | Emphasize failures as first-class panel output |
| Slide 10 | Make subject/query/provenance concrete |
| Slide 11 | Keep hosted state transitions operational, not strategic |
| Slide 12 | Tie GPU to architecture, not hardware excitement |
| Slide 13 | Use concise rules and concrete source landmarks |

# Terminology Standard

| Preferred display name | Code names | Glossary term | Terms to avoid |
| --- | --- | --- | --- |
| Observation truth | `DataSpec`, `LayoutSpec`, `ResolvedObservationWorld`, `DataView` | Data facts available to a run | "just CSV" |
| Strategy truth | `PlanSpec`, `NodeSpec`, `PlanBindings`, `PlanExecutionSpec` | Requested computation and run semantics | "strategy script" |
| Runtime contract | `DataView + PlanSpec` | Boundary reconciled by compiler/capability checks | "bot logic" |
| Row buffers | `NumericBuffer`, `BoolBuffer`, `CompiledProgram` slots | Deterministic evaluation outputs | "temporary arrays" when correctness matters |
| Declared dynamic routing | `CompiledDynamicExecutionMetadata` | Bounded route/subgraph masks | "runtime graph mutation" |
| Hosted wrapper | `ProtocolDispatcher`, `ServerRuntime`, job stores | Operational shell around core runtime | "separate service runtime" |

# Suggested Title And Subtitle

Title: SugarBot Runtime Explainer

Subtitle: How explicit data, typed PlanSpecs, row buffers, contracts, and reports form one extensible execution architecture
