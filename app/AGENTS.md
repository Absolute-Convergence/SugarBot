# app/AGENTS.md

Purpose: app-layer supplement to the root [AGENTS.md](C:/Users/Mel/source/repos/Absolute-Convergence/sugar_bot/sugar_bot_cpp_upload/AGENTS.md).
Keep this file about CLI/orchestration boundaries only.

## 0) Scratch Memory (Update First)
- This section is scratch memory and must be updated after every code change.
- 2026-04-21: Phase 4.5 UX close-out hardened the app layer around result wording and interactive file selection. `app_commands.cpp` now documents coarse-fine `k_stage2` as internal candidate breadth while final rows stay under `sweep.top_k`; `app_interactive.cpp` now always prompts for the CSV source in template/custom PlanSpec flows, validates existing files at prompt time, and reprompts missing paths for workflow 1 instead of failing only after all paths are collected. `sugar_app_tests` now cover coarse-fine TopK display, bool-op node-list hints, missing-path reprompting, and template/custom CSV prompt visibility. Validation is Windows `.sandbox/sandbox_vs_phase4_finish` Debug rebuild of `sugar_app_tests` / `sugar_Bot` plus passing `ctest -C Debug -R sugar_app_suite --output-on-failure`.
- 2026-04-20: app CLI plan onboarding now matches the richer layout onboarding. `--dump-plan` / `--dump-logic` are the user-facing guide for raw `PlanSpec` authoring, `--help` now points users at `--align-dump` before cross-series or ranked plan work, and `run` plus `--plan-report` now print explicit pre-binding/runtime-binding/post-binding stage headers so observation-world and runtime reports read as two DAG phases instead of duplicate blocks. Validation for this app-surface pass is the current Windows `.sandbox/sandbox_vs_phase4_finish` build of `sugar_app_tests` and `sugar_Bot`, passing `ctest -C Debug -R sugar_app_suite --output-on-failure`, plus manual `sugarbot.exe --help` and `sugarbot.exe --dump-plan`. Keep future plan-surface documentation and stage-label work in `app_commands.cpp` / `app_reporters.cpp` rather than scattering hints through ad hoc error strings only.
- 2026-04-20: app CLI onboarding now has a dedicated layout-contract surface. `--dump-layout` is the user-facing guide for raw-root `DataSpec` plus wrapped `LayoutSpec` `ingest` usage, `--help` now points first-time users at that guide and explicitly warns that `data_spec` is not a valid wrapper key, and `sugar_app_tests` now freeze both the new command wiring and the explicit layout/time-parse hints. Keep future noninteractive layout/documentation work in these CLI/reporting seams instead of scattering partial schema clues across unrelated commands.
- 2026-04-19: app entrypoints now include the first networked hosted-service surfaces. `app/entrypoints/server_main.cpp` and `client_main.cpp` should stay thin argument-to-service shims over `src/service/network_server.*` and `src/service/network_client.*`, just like `main.cpp` stays thin over the local app runtime. Keep protocol framing, session/job logic, and socket behavior out of the rest of `app/`; those concerns now belong to the internal service layer, with Linux/WSL as the real socket platform and Windows entrypoints remaining explicit unsupported-platform stubs for now.
- 2026-04-18: app diagnostics now have two explicit noninteractive seams: shared input diagnostics and `--feature-dump`. `app_reporters.cpp` should surface `PreprocessReport` header/line/drop/warning facts directly on prep/align/obs/plan reports instead of rebuilding ingest summaries locally, and `--feature-dump <layout.json> <logic.json> <data.csv>` should stay a thin DataView-first CSV emitter for single-series or `logic.run.scope=primary_series` only. Do not invent app-local multi-entity aggregation semantics for feature dumping.
- 2026-04-18: `app/tests` is now split by surface instead of one monolith. Keep shared fixtures and repo/file-scan helpers in `app_test_support.*`, keep `app_tests.cpp` as a thin registry/summary entrypoint, and add new coverage in the nearest split file (`app_cli_tests.cpp`, `app_runtime_tests.cpp`, `app_command_tests.cpp`, `app_interactive_tests.cpp`, `app_run_tests.cpp`).
- 2026-04-18: `app/` is now foldered by concern: `entrypoints`, `cli`, `commands`, `interactive`, `reporting`, `runtime`, and `tests`. Keep includes explicit from the `app/` root, and when source-scan tests/contracts inspect app files, make them recursive so future cleanup does not break harness coverage.
- 2026-03-29: `app_cli.cpp` / `app_commands.cpp` now expose the canonical power-user surface `sugarbot run <layout.json> <logic.json> <data.csv>`. This path loads `LayoutSpec`, rejects legacy embedded `series_inputs`, builds a shared `ResolvedObservationWorld`, and then either binds an explicit primary target or runs ranked multi-entity execution from `PlanSpec.run`; it does not reuse the interactive prompt flow or repurpose `--run-panel`. `app_reporters.cpp` now has an observation-world provenance reporter beside runtime provenance, and `sugar_app_tests` now cover parse, single-series run, explicit multi-entity target run, ranked multi-entity run, missing-scope rejection, and legacy-logic quarantine. `sugar_app_tests` baseline is now 349.
- 2026-03-29: `app_runtime.cpp` now consumes named shared runtime-builder profiles from `run_pipeline` instead of hard-coding alignment/runtime defaults locally. Interactive and panel runtime assembly still use compatibility behavior today, but they now do it explicitly through shared builder helpers rather than by leaning on the old config-only overload. `align-dump` also now propagates `DataSpec` non-temporal multi-series policy instead of silently forcing rejection through app-local defaults, and app tests now freeze the machine-readable `series_count=` / `series[i]=...` output instead of assuming prose text that the reporter never emitted. `sugar_app_tests` baseline is now 315.
- 2026-03-28: app panel/reporting surfaces now consume the explicit multi-entity ranking seam. `app_commands.cpp` passes a concrete ranking spec into shared runtime code, and `app_reporters.cpp` prints ranking metric, drawdown weight when relevant, plus shared-runtime `primary_selection_mode` and modeled alignment policy instead of leaving those as hidden behavior. `sugar_app_tests` baseline is now 303.
- 2026-03-28: `sugar_app_tests` fixture helpers are now hermetic and sandbox-safe: they write under a temp-owned `sugar_app_tests` directory instead of `out/test_assets`, and template execution no longer depends on launching from the repo root because `strategy_menu.cpp` now resolves built-in template JSONs against the configured source root when relative cwd lookup fails.
- 2026-03-28: panel reporting now treats multi-entity results as `series`-keyed first and prints `entity=` only when entity identity differs from series identity; avoid reintroducing app-surface wording that implies those labels are always interchangeable.
- 2026-03-28: app-layer CLI unification is blocked on shared runtime/contract repairs. Do not treat `--run-panel`, hidden primary-series heuristics, or current panel output semantics as stable design truth for the future noninteractive `run` surface until series/entity identity, explicit multi-entity targeting, and topology/reporting foundations are repaired.
- Root `AGENTS.md` is authoritative; this file adds `app/`-local rules only.
- App execution is always DataView-first. App surfaces must not invent separate runtime families for temporal, spatial, or spatiotemporal inputs.
- `app/entrypoints/main.cpp` is a thin parse-and-dispatch root.
- `app_runtime.*` is thin glue over shared `run_pipeline` helpers.
- `app_commands.*`, `app_interactive.*`, and `app_reporters.*` should consume shared diagnosis/reporting truth rather than reimplement it.
- Legacy embedded observation inputs remain compatibility-only and require explicit source choice when both legacy and caller-supplied inputs exist.
- `--dump-ops`, prep/align dump, runtime preview, and plan reporting should reflect shared registry/runtime truth, not app-local heuristics.
- `sugar_app_tests` is the surface-wiring suite; use it to prove shared reports/manifests are presented consistently, not to duplicate core contract coverage.
- If app tests keep growing, prefer splitting them by surface/module rather than extending one giant file indefinitely.
- Keep the foldered app map stable unless a stronger boundary appears: `entrypoints`, `cli`, `commands`, `interactive`, `reporting`, `runtime`, and `tests`.

## 1) App Responsibilities
- Parse args and prompt flow.
- Normalize file paths and dispatch.
- Build typed runtime packages and hand them to shared runtime code.
- Present human-readable output without coupling formatting to core logic.

## 2) Surfaces To Preserve
- `--data-spec`
- `--prep-dump`
- `--align-dump`
- `--run-panel`
- `--dump-ops`
- `--run-contract-suite`

Do not silently repurpose existing flags.

## 3) Rules For Changes In `app/`
- Keep app/orchestration thin and explicit.
- Do not add app-only execution modes or app-only capability heuristics.
- Prefer shared runtime/diagnosis/manifests over bespoke app logic.
- Preserve explicit and non-destructive diagnostics.

## 4) Validation Guidance
- After app changes, run `sugar_app_tests` when feasible.
- For app changes that touch diagnosis/reporting, verify at least one CLI surface and one shared runtime path agree on the same underlying facts.
