# src/AGENTS.md

Purpose: implementation-layer memory for SugarBot source files in `src/`.

## Core principles
- Preserve strict layer boundaries:
  - StrategyPlan = declarative identity/wiring.
  - StrategyExecutor = runtime FSM execution.
  - Sweep/orchestration = search tooling, strategy-agnostic.
- No "reach-around" dependencies between layers.
- Prefer additive refactors and staged migrations.

## Sweep architecture guidance
- Keep sweep engines/policies modular and runtime-configurable.
- Keep progress formatting outside core algorithms.
- Keep evaluation contracts explicit and pluggable.
- Avoid embedding long custom sweep logic in UI/menu files.

## Current direction (from this project phase)
- `src/plan_sweep.cpp` owns PlanSpec custom strategy execution/sweep orchestration.
- `src/sweep_session.cpp` owns generic session-style sweep orchestration contracts.
- `src/sweep.cpp` retains legacy/general sweep utilities and index engine implementations.

## Editing rules in src/
- Small diffs preferred.
- Preserve behavior unless change is explicitly requested.
- When adding a new sweep engine, integrate via modular entry points (not template-specific ad hoc paths).
- Keep warning level clean on MSVC (`/W4`).

## Validation
- Primary local validation:
  - `cmake --build out/build/x64-Debug --config Debug --target sugar_Bot`
  - `ctest --test-dir out/build/x64-Debug -C Debug --output-on-failure`
