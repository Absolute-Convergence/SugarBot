---
artifact_id: "09-narration-package"
harness_id: "agentic-docs.narration-writing"
library_version: "1.0.0"
subject: "SugarBot"
scope: "Narration package for trace-backed SugarBot walkthrough"
generated_at: "2026-05-25T21:00:28.8150479-04:00"
inputs:
  - "04b-behavioral-dynamics-inventory.md"
  - "07-narrative-architecture.md"
  - "08-visual-storyboard.md"
confidence_policy: "observed | inferred | speculative labels required"
review_state: "draft"
---

# Slide 1

Slide Id: `runtime-not-script`

Slide Title: SugarBot Is A Validated Runtime, Not A Script

Narration: SugarBot looks like it has many products: a CLI, a server, demos, panels, sweeps, reports, and a contract runner. The source says something tighter. These surfaces point back to one runtime pattern: load explicit artifacts, validate what they mean, compile a typed graph, evaluate row-aligned buffers, execute a finite-state strategy, then report the result. That makes the system easier to extend, but only when new work lands at the right boundary.

Takeaway: Treat SugarBot as one validated runtime with several adapters.

Optional Analogy: None.

Caption Highlights: CMake layers, shared runtime, adapters.

Emphasized Terms: validated runtime, explicit artifacts, row-aligned buffers.

# Slide 2

Slide Id: `two-truths`

Slide Title: Observation Truth And Strategy Truth Meet At Runtime

Narration: The core split is between observation truth and strategy truth. Observation truth is what the data can honestly provide: columns, rows, axes, primary series, and topology providers. Strategy truth is what the plan asks to compute: nodes, params, operator requirements, signal bindings, sweep policy, and optional dynamic routing. The compiler and runtime checks reconcile those two sides. This is why `DataView` and `PlanSpec` are the important boundary.

Takeaway: The architectural center is the contract between `DataView` and `PlanSpec`.

Optional Analogy: None.

Caption Highlights: Observation truth, strategy truth, reconciliation.

Emphasized Terms: DataView, PlanSpec, OperatorRegistry.

# Slide 3

Slide Id: `artifact-pipeline`

Slide Title: Artifacts Become Reports Through A Typed Chain

Narration: This live pipeline follows the quickstart fixture shape. A run starts as CSV and layout files, then becomes frames, an aligned bundle, topology-aware runtime input, a `DataView`, a compiled program, evaluated buffers, and finally reports. The important part is not the animation; it is the state boundary between each step. Each boundary is a chance to validate meaning before the executor can produce a plausible but wrong result.

Takeaway: Every run crosses explicit artifact, runtime, compile, and report boundaries.

Optional Analogy: None.

Caption Highlights: quickstart fixtures, row domain, runtime target, reports.

Emphasized Terms: ResolvedObservationWorld, DataView, CompiledProgram.

# Slide 4

Slide Id: `compile-buffer-execute`

Slide Title: The Compiler Makes Buffers; The Executor Simulates Trades

Narration: The interactive state machine separates three jobs. The compiler validates PlanSpec nodes and assigns slots. The evaluator fills numeric and bool buffers over the active row domain. The executor consumes only price columns, entry and exit signals, an allow-trade gate, and execution settings. New indicators belong in the registry and evaluator. Fill timing, costs, reversal rules, and trade lifecycle belong in the executor.

Takeaway: Keep graph semantics out of the trade-state executor.

Optional Analogy: None.

Caption Highlights: DAG, slots, signal buffers, FSM.

Emphasized Terms: compile, evaluate, execute.

# Slide 5

Slide Id: `operators-capabilities`

Slide Title: Operator Metadata Is Runtime Correctness, Not Catalog Text

Narration: `OperatorRegistry` is more than a list of names. Its metadata tells the compiler the input and output types, tells diagnostics which axes and topology families are required, and tells users how an operator behaves. If a new operator is added only as evaluator code, the runtime can lose one of its strongest properties: early, useful rejection. Registry metadata must evolve with kernels, capability checks, and contracts.

Takeaway: A correct operator is metadata, implementation, diagnostics, and tests together.

Optional Analogy: None.

Caption Highlights: Types, params, warmup, axes, topology requirements.

Emphasized Terms: OpSpec, capability metadata, diagnostics.

# Slide 6

Slide Id: `scale-surfaces`

Slide Title: Sweeps, Panels, And Hosted Jobs Reuse The Same Core

Narration: The advanced surfaces are not separate runtimes. Sweeps repeat PlanSpec execution across parameter candidates. Panels repeat it across aligned entities. Hosted service code stages artifacts, validates requests, applies quotas, schedules jobs, handles cancellation, and stores results. Those are operational wrappers around the same execution model. This is why local and hosted behavior can be compared.

Takeaway: The broad feature set stays coherent because it fans into one runtime.

Optional Analogy: None.

Caption Highlights: Sweep candidates, panel entities, hosted jobs.

Emphasized Terms: run_custom_plan_spec, PanelResult, ServerRuntime.

# Slide 7

Slide Id: `dynamic-routing`

Slide Title: Dynamic Execution Is Declared Routing Over A Static Graph

Narration: Dynamic execution needs careful wording. The active design is not arbitrary runtime graph construction. PlanSpec declares subgraphs, routes, cases, fallback subgraphs, outputs, and bounds. The compiler validates those declarations. The evaluator computes route decisions, masks inactive subgraph rows, and gates declared outputs. The comparison replay uses the G10 demos to show constant static equivalence, row-varying routing, and topology-informed routing without changing the compiled-graph contract.

Takeaway: Dynamic execution is bounded routing over a compiled graph.

Optional Analogy: None.

Caption Highlights: Declared routes, masks, gated outputs, bounds.

Emphasized Terms: declared dynamic, route masks, deterministic replay.

# Slide 8

Slide Id: `sweep-search`

Slide Title: Sweep Search Is A Trace Of Proposals, Observations, And Filters

Narration: Sweep behavior is best understood as a trace. A policy proposes candidates, each candidate runs through the same PlanSpec runtime, the result becomes an observation, and report logic ranks, filters, and explains the retained set. Exhaustive, Monte Carlo, coarse/fine, successive halving, and Bayesian posterior policies differ in how they choose candidates. The visualization should never imply that all engines walk the space in the same order.

Takeaway: Sweeps change search policy, not strategy semantics.

Optional Analogy: None.

Caption Highlights: proposals, observations, Top-K, Pareto, trace.

Emphasized Terms: candidate, observation, ranking, replay trace.

# Slide 9

Slide Id: `panel-execution`

Slide Title: Panel Execution Rebinds One Plan Across Many Entities

Narration: Panel execution is not a separate strategy model. The same PlanSpec is applied repeatedly while each aligned entity is bound as the primary target. Successful entities receive scores and rankings. Failed entities must remain visible because hiding them would make the Top-K look more complete than it is. The matrix view is useful because it keeps primary binding, success, failure, and ranking in one place.

Takeaway: Panel rankings are only trustworthy when failures stay visible.

Optional Analogy: None.

Caption Highlights: entity binding, success counts, failures, ranking.

Emphasized Terms: primary binding, PanelResult, failures.

# Slide 10

Slide Id: `topology-locality`

Slide Title: Topology Operators Resolve A Subject, Query, And Provenance

Narration: Topology is a runtime capability, not a feature toggle. A topology operator must resolve a subject, choose a topology provider, issue a supported query, compute over the neighbor set, and preserve provenance in reports. The F8 topology demos split the surface across relational time-valid graphs, hierarchy traversal, and semantic similarity. A scrubber helps because the meaning changes with subject, query kind, and topology family.

Takeaway: Topology work should preserve subject, query, and provenance together.

Optional Analogy: None.

Caption Highlights: subject, query kind, neighbor set, provenance.

Emphasized Terms: topology capability, NeighborhoodQuery, provenance.

# Slide 11

Slide Id: `hosted-lifecycle`

Slide Title: Hosted Mode Adds Job Controls Around Local Semantics

Narration: Hosted mode adds a control plane around the same runtime. A client connects, uploads or references artifacts, validates a request, submits a job, waits through queue and execution states, then retrieves a result or cancellation. Service contracts cover ownership, abuse rejection, fairness, backpressure, and cancellation. The key caveat is that these controls govern job handling; they should not redefine what a valid SugarBot run means.

Takeaway: Hosted service policy wraps local semantics rather than replacing them.

Optional Analogy: None.

Caption Highlights: session, upload, validate, queue, cancel, result.

Emphasized Terms: ProtocolDispatcher, ServerRuntime, job state.

# Slide 12

Slide Id: `gpu-lane`

Slide Title: GPU Support Belongs Behind The Buffer And Batch Boundaries

Narration: GPU support should follow the architecture that already exists. The safest first targets are evaluator-side row buffers and batched sweep or panel work. A single small run may not benefit, especially on a consumer GPU, because transfer and launch overhead can dominate. Large candidate batches, long row domains, many entities, and topology or posterior workloads are better fits. The CPU path should remain canonical until parity tests are explicit.

Takeaway: GPU work should accelerate shared buffers and batches without changing semantics.

Optional Analogy: None.

Caption Highlights: CPU canonical path, optional GPU backend, parity gate.

Emphasized Terms: row buffers, batched sweeps, CPU/GPU parity.

# Slide 13

Slide Id: `stewardship`

Slide Title: Stewardship Means Keeping Semantics In The Owning Layer

Narration: The practical rule is simple: put new meaning where that meaning belongs. Data-domain concepts belong in ingest, alignment, topology, and `DataView`. Computation belongs in operator metadata and evaluator support. Trading mechanics belong in `StrategyExecutor`. Search belongs in sweep policy. Hosted limits belong in service policy. Report interpretation belongs in report structures. The architecture can absorb a lot of growth, but it becomes harder to trust when those responsibilities blur.

Takeaway: SugarBot stays maintainable when extension work respects the runtime boundaries.

Optional Analogy: None.

Caption Highlights: data, compute, execute, search, service, report.

Emphasized Terms: owning layer, explicit boundary, contract-first.
