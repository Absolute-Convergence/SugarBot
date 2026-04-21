# include/AGENTS.md

Purpose: header-level supplement to the root [AGENTS.md](C:/Users/Mel/source/repos/Absolute-Convergence/sugar_bot/sugar_bot_cpp_upload/AGENTS.md).
Do not restate repo history here; keep this file short.

## 0) Scratch Memory (Update First)
- This section is scratch memory and must be updated after every code change.
- 2026-04-18: the hosted-service expansion plan is internal-first. Do not introduce `include/service` or other public service/session/job headers just to mirror planned implementation work; keep hosted-service APIs private until a stable reusable public contract is truly needed.
- 2026-04-18: the public header map now fully mirrors the current public module layout. `include/legacy`, `include/ingest`, `include/ops`, `include/engine`, `include/sweep`, and `include/contracts` are the only public browsing roots, and the old flat public header layer is gone. Prefer stabilizing this map over further public-header reshuffling.
- 2026-04-17: real mirrored public header directories are now in place for `legacy`, `ops`, `engine`, and `contracts`, and the wrapper-only trees `compat`, `data`, `plan`, `runtime`, and `testing` have been removed. Continue from this baseline by migrating the remaining flat public headers in module-sized batches, with `ingest` and `sweep` as the next likely groups.
- 2026-04-17: do not keep expanding grouped wrapper header trees under `include/`. Header organization should converge toward real module directories that mirror `src/` naming where possible: `legacy`, `ingest`, `ops`, `engine`, `sweep`, and `contracts`. If compatibility is needed during that migration, prefer turning the existing flat headers into temporary shims instead of adding new wrapper-only files.
- 2026-03-29: public ingestion/orchestration headers now expose the unified noninteractive artifact contract. `layout_spec.h` is the public wrapper for `ingest` plus declared topologies while still accepting bare `DataSpec` JSON through the loader, `plan_spec.h` now has a minimal `PlanSpec.run` section (`PlanRunScope`, `target_entity`, `top_k`, `ranking_metric`, optional `ranking_dd_weight`) for deterministic CLI dispatch only, and `run_pipeline.h` now exposes `ResolvedObservationWorld` plus explicit target binding as the shared seam beneath `sugarbot run <layout.json> <logic.json> <data.csv>`. Keep `PlanSpec.run` small; do not let it absorb ingest/topology/runtime policy.
- 2026-03-29: `data_spec.h`, `align.h`, and `run_pipeline.h` now move more runtime truth into shared public contracts: `DataSpec` carries alignment intent, `align.h` has explicit `EqualLengthRowDomain` non-temporal multi-series policy, and `run_pipeline.h` exposes named strict vs compatibility runtime-builder helpers instead of leaving that distinction implicit in overload choice. This public seam is now covered by `alignment_non_temporal_design_loads`, `alignment_non_temporal_policy`, `alignment_policy_metadata_preservation`, and `target_resolution_design_loads`.
- 2026-03-28: `panel_runner.h` now exposes the neutral multi-entity ranking contract directly: `PanelRankingMetric`, `PanelRankingSpec`, per-row `ranking_metric`, aggregate `PanelResult::ranking`, and failure provenance that keeps `series_key` distinct from `entity_key`. This header seam is validated by `multi_entity_runner_design_loads` / `multi_entity_runner_overload_rejection`.
- 2026-03-28: public runtime headers now carry explicit target/alignment provenance seams. `align.h` models `NonTemporalMultiSeriesPolicy`, `run_manifest.h` preserves `alignment_non_temporal_multi_series` plus `RuntimePrimarySelectionMode`, and `run_pipeline.h` now exposes an explicit-target `RuntimeBuildOptions` path beside the old compatibility wrapper. `plan_capability.h` also gained `AmbiguousRuntimeDefaultSeries` so multi-series unqualified-ref rejection is explicit instead of silently inheriting the first series.
- 2026-03-28: `TopologySpec` now carries explicit origin/bearing classification, and public topology identity rules are tighter: declared topologies may not silently replace existing same-name entries and may not claim reserved `builtin.*` names.
- 2026-03-28: public runtime/layout foundation work must make topology identity explicit before any `LayoutSpec` promotion: reserve builtin topology namespaces, reject silent same-name replacement for public layout-declared topologies, and surface topology origin/bearing explicitly instead of inferring it from ad hoc runtime behavior.
- 2026-03-21: `include/sweep/sweep.h` now spells `MonteCarloIndexConfig::draws` as plain `10000` instead of `10'000` so Flawfinder can scan the header without tripping over the digit separator; runtime behavior is unchanged.
- Root `AGENTS.md` is authoritative; this file only adds header-local contract rules.
- Stable public engine contracts may not depend on a type narrower than `DataView`.
- `DataView`, `AlignedBundle`, and `NeighborhoodQuery` are the additive public runtime/query seams.
- `PlanSpec` stays strategy-only; observation/world-model metadata belongs in `DataSpec`, `IndexSchema`, and `RunManifest`.
- `IndexSchema` remains axis metadata only; topology belongs in `TopologySpec` / `TopologyCatalog`.
- Public headers must keep temporal, spatial, and spatiotemporal data in one runtime family.
- Real execution load today is limited to current built-in topology adapters plus the currently implemented ops; future topology families in public headers are metadata-bearing unless runtime adapters exist.
- `run_pipeline.h` owns shared runtime packaging plus legacy embedded-observation compatibility hooks.
- `run_manifest.h` owns neutral capability and topology/locality provenance seams for future external producers/consumers.
- Do not introduce a public coarse `ExecutionMode` enum.

## 1) Header Priorities
- Backward-compatible, additive changes first.
- Keep names domain-agnostic and explicit.
- Include what you use and minimize transitive coupling.
- Keep comments focused on contract semantics and invariants.

## 2) Public Contract Boundaries
- `time_key.h` defines canonical `TimeKey`.
- `numeric_frame.h` and `align.h` define the stable observation storage/alignment seam.
- `data_view.h` defines the stable indexed observation runtime seam.
- `compiled_program.h` and `program_evaluator.h` define the runtime-machine contract.
- `op_registry.h`, `plan_compiler.h`, `plan_capability.h`, and `observation_capability.h` define capability and compile-time overlays.
- `strategy_executor.h`, `plan_sweep.h`, and `strategy_menu.h` are DataView-first only.

## 3) Compatibility Rules
- `CandleSeries` is a supported compatibility shape, not a stable engine runtime contract.
- Candle compatibility remains explicit through `legacy_candle_frame_adapter.h`.
- Public headers must stay free of retired graph-era runtime types.
- Legacy embedded observation inputs are compatibility-only and must not return as public runtime truth.

## 4) Validation Guidance
- Prefer adding focused contract sections over enlarging broad catch-all sections.
- Header changes that widen runtime semantics should be paired with contract coverage in `src/contract_suite_*`.
