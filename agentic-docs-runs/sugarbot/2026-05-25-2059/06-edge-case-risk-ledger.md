---
artifact_id: "06-edge-case-risk-ledger"
harness_id: "agentic-docs.edge-case-risk-ledger"
library_version: "1.0.0"
subject: "SugarBot"
scope: "Operational and correctness risks in active SugarBot architecture"
generated_at: "2026-05-25T21:00:28.8150479-04:00"
inputs:
  - "01-codebase-inventory.md"
  - "03-transformation-map.md"
  - "04-call-graph-control-flow.md"
  - "04b-behavioral-dynamics-inventory.md"
  - "05-invariant-proof-ledger.md"
confidence_policy: "observed | inferred | speculative labels required"
review_state: "draft"
---

# Risk Ledger

| Risk name | Category | Code location | Trigger condition | Failure symptom | Consequence | Severity | Confidence | Recommended control | Suggested test/monitor | Slide treatment |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Wrong primary target | Ordering/implicit binding | `bind_resolved_runtime_target`, `PanelRunSpec` | Multi-series data without clear target | Plausible but wrong run result | Misleading strategy conclusion | high | inferred | Require target in multi-series workflows and report provenance | target-resolution rejection and report snapshot tests | risk card |
| Registry metadata drift | Schema/capability drift | `OpSpec`, op modules, evaluator | New op works but lacks axis/topology metadata | Preflight passes/fails incorrectly | Trust erosion in diagnostics | high | inferred | Metadata/evaluator parity contract | per-op capability matrix test | proof/risk split |
| NaN/warmup mismatch | Null/NaN handling | `CompiledProgram`, evaluator, kernels | Rolling/window ops produce prefixes | Entry/exit shifts by rows | Incorrect trades and PnL | high | inferred | Warmup contract per op family | golden buffer tests for NaN/warmup | before/after table |
| Dynamic execution expectation gap | Mode inconsistency | `PlanExecutionSpec`, evaluator masks/gates | User expects lazy graph mutation | Confusing docs or wrong mental model | Unsafe extension design | medium | observed | Rename/describe as declared bounded routing | static-vs-dynamic equivalence demos | concept diagram |
| Sweep summary hides failures | Partial success | `PlanSweepReport` | Many candidates fail compile/execute | Top-K looks healthy with high hidden failure rate | Bad optimization decisions | medium | inferred | Prominent candidate outcome counts | sweep with mixed valid/invalid candidates | ledger row |
| Panel partial failure | Partial success | `PanelResult`, `PanelReport` | Some entities lack primary column | Missing entities disappear from ranking | Biased top-k | medium | observed | Always show entity counts and failures | panel missing-column test | panel matrix |
| Hosted public exposure assumption | External dependency/security | `src/service/*` | Service bound beyond trusted context | Unauthenticated or unencrypted access if deployed naively | Data/job exposure risk | high | inferred | Document deployment boundary and remote-bind policy | protocol remote-bind/security contract | caution slide |
| Cancellation granularity | Concurrency | executor/sweep/service worker paths | Long candidate or op batch | Job remains running after cancel | Operational friction | medium | observed | Check cancellation in long loops and batch boundaries | slow job cancellation test | service sequence |
| Report schema sprawl | Schema drift | `PlanSweepReport`, evaluation report files | New report fields mix execution and projection semantics | Hard-to-stabilize JSON | Consumer breakage | medium | inferred | Schema snapshots and field ownership rules | JSON report compatibility snapshots | maintainer guidance |
| Future GPU numeric drift | Nondeterminism/type coercion | no GPU code yet; `docs/gpu_support_roadmap.md` | GPU reductions/window ops differ from CPU | Ranking/trade differences | Misleading accelerated results | high | speculative | CPU canonical path, tolerance policy, parity contracts | CPU/GPU parity benchmark gates | GPU roadmap callout |

# Highest Priority Risks

| Priority | Risk | Why it is high leverage |
| --- | --- | --- |
| 1 | Wrong primary target | The system can succeed while answering the wrong question |
| 2 | Registry metadata drift | It weakens the contract-before-computation model |
| 3 | NaN/warmup mismatch | Small buffer differences can change trade timing |
| 4 | Hosted exposure assumption | Service controls are not the same as deployment security |
| 5 | Future GPU numeric drift | Acceleration is only useful if canonical CPU semantics survive |

# Suggested Tests And Monitors

| Control | Concrete test or monitor |
| --- | --- |
| Multi-series target discipline | Test multi-series runs reject missing target or include target provenance |
| Op metadata parity | Enumerate `make_default_registry().list_ops()` and verify evaluator support plus required metadata |
| Warmup/NaN golden buffers | Fixture each rolling/window op with expected prefix and missing behavior |
| Dynamic execution contract | Constant dynamic route must match static equivalent; row-varying route must report active/inactive rows |
| Sweep partial-failure visibility | Sweep fixture with invalid candidate subset must expose counts and last error |
| Hosted ownership and cancellation | Keep service concurrency and live cancel tests in hosted regression label |
| Report schema governance | Snapshot text/JSON projections for run, sweep, panel, topology, dynamic execution |
| GPU readiness | Benchmark and parity gate before enabling auto GPU path |

# Suggested Slide Treatments

| Risk | Treatment |
| --- | --- |
| Wrong primary target | Show a two-series table where choosing `A.close` versus `B.close` changes result meaning |
| Metadata drift | Show registry metadata feeding compiler, diagnostics, and evaluator |
| Warmup/NaN mismatch | Show signal shift caused by one missing row |
| Dynamic execution expectation | Show static graph with route masks, not graph mutation |
| Hosted exposure | Show control-plane wrapper and deployment caveat |
| GPU drift | Show CPU canonical lane beside optional GPU backend and parity gate |
