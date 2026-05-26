---
artifact_id: "05-invariant-proof-ledger"
harness_id: "agentic-docs.invariant-proof-ledger"
library_version: "1.0.0"
subject: "SugarBot"
scope: "Correctness and evidence ledger for active SugarBot runtime"
generated_at: "2026-05-25T21:00:28.8150479-04:00"
inputs:
  - "03-transformation-map.md"
  - "04-call-graph-control-flow.md"
  - "04b-behavioral-dynamics-inventory.md"
  - "CMakeLists.txt"
  - "src/contracts/"
  - "app/tests/"
confidence_policy: "observed | inferred | speculative labels required"
review_state: "draft"
---

# Stage Invariants

| Stage | Expected invariant | Evidence in code | Proof strength | Failure symptom | Recommended proof improvement |
| --- | --- | --- | --- | --- | --- |
| Artifact parsing | JSON/CSV intent is represented explicitly or rejected | `parse_plan_spec_json`, `load_plan_spec_file`, ingest parser files, app tests | moderate | run starts with wrong schema or vague parse error | Add schema snapshot tests for representative layout/PlanSpec versions |
| Alignment | Every runtime column span shares the active row domain | `AlignedBundle`, `DataView::row_count`, alignment contracts in CTest | strong | buffer length mismatch or wrong temporal pairing | Keep row-count assertions near evaluator inputs |
| Target binding | Primary series/column is explicit before execution | `RuntimeTargetSpec`, `bind_resolved_runtime_target`, target-resolution CTest entries | strong | plan uses wrong entity or fails late | Include primary target in every report provenance projection |
| Operator capability | Plan ops declare required params, axes, topology, warmup, and types | `OpSpec`, `OperatorRegistry`, operator-axis/runtime contracts | moderate | preflight says compatible but evaluator fails or miscomputes | Add per-op metadata/evaluator parity tests for new ops |
| Compile | DAG is valid, typed, topological, and signal-bound | `plan_compiler.cpp`, `CompiledProgram` contract comments | moderate | missing signal buffers or cyclic/unsupported node graph | Add compile error snapshot tests for representative invalid graphs |
| Evaluation | Every output buffer is row-aligned, deterministic, and owns warmup defaults | `compiled_program.h`, `program_evaluator.cpp`, runtime ops contracts | moderate | NaN/false defaults shift entries/exits | Add warmup/NaN/default-signal tests per op family |
| Dynamic execution | Routes/subgraphs are declared and bounded before output gating | `PlanExecutionSpec`, `CompiledDynamicExecutionMetadata`, dynamic contracts/demos | moderate | inactive subgraphs leak into strategy outputs | Add exact static-vs-constant-dynamic equivalence assertions to regression set |
| Strategy execution | Trade lifecycle follows `ExecSettings` and signal buffers | `StrategyExecutor::run`, app/runtime tests | moderate | incorrect fill timing, reversal, costs, or closeout | Add table-driven executor tests for all timing/reversal combinations |
| Sweep/panel | Repeated runs use same runtime semantics with visible failures | `plan_sweep.cpp`, `panel_runner.cpp`, sweep/panel contracts | moderate | top-k excludes failures silently or ranking drifts | Add deterministic tie-break and partial-failure report tests |
| Hosted service | Session ownership, quotas, cancellation, and results are enforced around the same runtime | service contracts and CTest labels | strong for covered cases | cross-session result leak, stuck job, wrong final status | Keep live TCP and worker cancellation tests in hosted regression label |

# Correctness Claim Ledger

| Claim | Evidence | Proof strength | Caveat |
| --- | --- | --- | --- |
| The build graph routes app, service, contracts, and tests to shared core libraries | `CMakeLists.txt` target links | strong | Link structure does not prove every command uses every runtime path |
| `DataView` is row-domain based | `DataView::row_count`, `col`, `axes`, `topology_catalog` | strong | Does not by itself prove every producer builds a valid bundle |
| Compiled programs evaluate row-wise buffers in deterministic topological order | `CompiledProgram` scheduling enum/comment, compiler/evaluator code | moderate | Determinism should remain covered by contract tests for new ops |
| Dynamic execution is bounded declared routing | `PlanExecutionBoundsSpec`, compiler dynamic validation, evaluator masks/gates | moderate | Documentation must avoid implying fully lazy execution |
| Hosted service enforces ownership and cancellation behaviors | service contract sections and `ServerRuntime` methods | strong for test-covered cases | Deployment security posture remains outside observed code |
| Sweeps are policy over candidate params, not separate strategy semantics | `PlanSweepSpec`, `run_custom_plan_spec`, sweep runtime adapter | moderate | Adaptive policies introduce proposal logic that needs traceability |

# Missing Proof Ledger

| Claim likely needed | Missing evidence | Recommended control |
| --- | --- | --- |
| Reports are backward-compatible public schemas | Formal schema/version contract not observed | Add report JSON schema snapshots and compatibility notes |
| CPU performance bottlenecks justify GPU work | No benchmark run in this harness | Add benchmark fixture suite and record evaluator/sweep/panel timings |
| All op registry metadata is complete and current | New op metadata can drift from evaluator | Add registry-vs-evaluator coverage contract for every registered op |
| Public hosted service is safe without extra controls | Auth/TLS/deployment policy not observed | Document deployment boundary; do not claim public readiness |
| GPU output can be treated as equivalent to CPU | No GPU backend or tolerance policy exists | Define per-op parity tolerances before implementation |

# Visual Proof Plan

| Proof area | Walkthrough treatment |
| --- | --- |
| Shared runtime | Layer diagram from CMake targets to one runtime path |
| Row-domain invariant | before/after table from CSV row to DataView span to signal buffer |
| Compile contract | node DAG becomes slots and bound signal roles |
| Dynamic execution | route/subgraph diagram with static fallback and bounded masks |
| Sweep/panel repeatability | candidate/entity matrix feeding the same run kernel |
| Hosted safety | service sequence diagram with validation, queue, cancel, result ownership |
| Caveats | risk/proof ledger slide with evidence strength badges |
| Live behavior | trace replay or scrubber only when backed by demos, source, contracts, or clearly labeled source-backed toy traces |
