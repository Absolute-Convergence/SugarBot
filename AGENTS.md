# AGENTS.md - SugarBot Agent Instructions

This file is the project constitution for any AI agent working in this repo (Codex, IDE agents, etc.).
Follow it even when chat context is reset.

---

## 1) Project identity

SugarBot is a C++ strategy experimentation engine for time-series data, built to answer:

> "What would have happened if I made this decision differently?"

SugarBot is not a predictor, not a black box, and not a single trading strategy. It is a decision-rule laboratory
for building, composing, executing, and evaluating rule-based strategies over historical data.

SugarBot is an active research and learning project, not a production trading system.

---

## 2) Non-negotiable architecture (layer boundaries)

SugarBot is intentionally built from clean, orthogonal layers. The simplified flow is:

`CSV -> CandleSeries -> SeriesView -> Indicators -> StrategyPlan -> StrategyExecutor -> Backtester -> Metrics/Sweeps`

Hard rules:
- Single responsibility per layer. No layer "reaches around" another.
- Indicators are signals, not decisions. They are pure transforms that do not place trades.
- StrategyPlan is declarative. It defines what a strategy is (identity + wiring + params), not how it runs.
- StrategyExecutor is runtime and strategy-agnostic. It consumes a StrategyPlan and produces a trade stream.
- Sweeps are first-class and strategy-agnostic. Optimization/search is part of the core design, not bolted on.
- Metrics are explicit and evolvable. Scoring/reporting can change independently of execution.

---

## 3) Design philosophy for changes

### 3.1 Flexibility-first, with "free" speed
- Prefer runtime polymorphism at generalized/orchestration layers (interfaces/virtual).
- Templates are welcome only in leaf kernels where they do not reduce future flexibility.
- Never leak template-heavy types into user-facing configuration pathways.

### 3.2 Minimal churn refactors
- Keep diffs small and staged.
- Additive changes first; migrate callers gradually.
- Avoid repo-wide rewrites unless explicitly requested.

### 3.3 Preserve the core runtime boundary
Treat these as stable boundaries:
- StrategyPlan (declarative wiring)
- StrategyExecutor (runtime FSM)
- Plan compilation/wiring layer

Do not redesign these wholesale during refactors unless explicitly asked.

---

## 4) Indicators (signal graph integrity)

Indicators must remain:
- Composable by design (indicator graphs, not only flat pipelines).
- Explicit about warm-up behavior and NaN propagation (no silent alignment bugs).
- Pure transformations: no trading decisions inside indicators.

If you introduce new indicators or composition utilities:
- Document warm-up contracts clearly.
- Prefer correctness/inspectability over micro-optimizations.
- Ensure outputs remain time-aligned.

---

## 5) Sweeps (search as a tool)

Sweeps exist to explore decision logic across parameter spaces:
- Strategy-agnostic Top-K ranking
- Coarse -> fine exploration
- Optional train/validate evaluation

Guiding principle:
> Sweeps are tools chosen to explore different parameter-performance manifolds efficiently.

If you add a new sweep engine/policy:
- Keep the sweep interface generic (candidate -> eval -> score).
- Keep UI/console formatting outside sweep engines; emit structured progress events only.
- Make it easy to plug new engines in without changing strategies.

---

## 6) Strategy templates (reference quality)

- Gated Diff Cross is the canonical reference template for clean separation and composition.
- Prefer StrategyPlan + Executor patterns over ad-hoc logic paths.

Legacy strategies may be migrated over time, but do not mass-migrate unless requested.

---

## 7) Working style (how to operate as an agent)

### Before editing
1) Identify the call flow and the minimal file set.
2) Propose a plan with compile checkpoints.
3) If the change likely touches >5 files, pause and ask for confirmation or propose a smaller staging.

### While editing
- Edit only what is necessary.
- Keep behavior semantically correct; exact output text ordering is not a requirement unless specified.
- Do not paste large file contents into chat; prefer concise diffs/patches.

### After editing
- Provide:
  - a concise diff summary (what/why)
  - exact build/test commands to validate

---

## 8) Build targets & environments (reference)

- Windows builds use MSVC (x64).
- Linux binaries are tested on Ubuntu / WSL2 (x86_64).

When giving commands, prefer CMake build invocations that work with the repo's build directories.

---

## 9) Licensing & confidentiality

ALL RIGHTS RESERVED.
Do not propose redistributing SugarBot source or derived materials.
When sharing changes, prefer patches/diffs and avoid large verbatim reproductions of source.

---
