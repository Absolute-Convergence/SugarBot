# app/AGENTS.md

Purpose: app-layer supplement to the root [AGENTS.md](C:/Users/Mel/source/repos/Absolute-Convergence/sugar_bot/sugar_bot_cpp_upload/AGENTS.md).
Keep this file about CLI/orchestration boundaries only.

## 0) Scratch Memory (Update First)
- This section is scratch memory and must be updated after every code change.
- Root `AGENTS.md` is authoritative; this file adds `app/`-local rules only.
- App execution is always DataView-first. App surfaces must not invent separate runtime families for temporal, spatial, or spatiotemporal inputs.
- `app/main.cpp` is a thin parse-and-dispatch root.
- `app_runtime.*` is thin glue over shared `run_pipeline` helpers.
- `app_commands.*`, `app_interactive.*`, and `app_reporters.*` should consume shared diagnosis/reporting truth rather than reimplement it.
- Legacy embedded observation inputs remain compatibility-only and require explicit source choice when both legacy and caller-supplied inputs exist.
- `--dump-ops`, prep/align dump, runtime preview, and plan reporting should reflect shared registry/runtime truth, not app-local heuristics.
- `sugar_app_tests` is the surface-wiring suite; use it to prove shared reports/manifests are presented consistently, not to duplicate core contract coverage.
- If app tests keep growing, prefer splitting them by surface/module rather than extending one giant file indefinitely.

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
