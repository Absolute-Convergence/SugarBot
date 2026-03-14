# include/AGENTS.md

Purpose: header-level supplement to the root [AGENTS.md](C:/Users/Mel/source/repos/Absolute-Convergence/sugar_bot/sugar_bot_cpp_upload/AGENTS.md).
Do not restate repo history here; keep this file short.

## 0) Scratch Memory (Update First)
- This section is scratch memory and must be updated after every code change.
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

