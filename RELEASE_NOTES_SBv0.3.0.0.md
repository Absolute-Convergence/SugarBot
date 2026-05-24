# SugarBot v0.3.0.0 Release Notes

Release date: May 5, 2026

Package: `SBv0.3.0.0.zip`

## Summary

SugarBot v0.3.0.0 is a source release focused on making the project demo-ready as a local strategy research and hosted-loopback analysis tool.

This release consolidates the C++ runtime, sweep engine, reporting layer, and local TCP service path into one shared execution model. The same strategy logic can now run through the direct CLI or through the hosted `sugar_server` / `sugar_client` loopback workflow.

The package includes source code, CMake build files, documentation, sample data, runnable demo plans, hosted protocol docs, and release smoke tooling. It does not include prebuilt binaries.

## Highlights

- Added a polished hosted client/server workflow using `sugar_server` and `sugar_client run`.
- Added Windows x86-64 hosted transport support through WinSock, bringing the loopback service path to parity with Linux/WSL.
- Expanded structured sweep reporting with stage summaries, metric catalogs, ranked candidates, validation metadata, and hosted JSON diagnostics.
- Added result-diversity controls for final sweep leaderboards, including trade-hash dedupe, basin-aware filtering, candidate oversampling, and minimum-trade filters.
- Improved Monte Carlo and coarse/fine sweep correctness so declared parameter spaces, evaluation budgets, and reported candidate counts line up consistently.
- Added practical demo plans under `graphs/demos/`, including NVDA, BTC intraday, Nashville housing panel, and a final NVDA ROC(SMA) diff-cross sweep designed to match the companion TradingView visualizer.
- Added release smoke automation through `tools/Run-ReleaseSmoke.ps1`.
- Simplified build and release paths around the `sugarbot`, `sugar_server`, `sugar_client`, `sugar_contracts`, and `sugar_app_tests` targets.

## Included User-Facing Tools

- `sugarbot`: main CLI for local runs, diagnostics, and interactive workflows.
- `SB`: generated alias beside `sugarbot`.
- `sugar_server`: loopback hosted-service process.
- `sugar_client`: hosted-service client, including one-command `run` support.
- `sugar_contracts`: deterministic contract-suite runner.
- `sugar_app_tests`: application wiring tests for development validation.

## Demo Coverage

The package includes four primary demo plans:

- `nvda_momentum_volatility`: daily NVDA equity sweep using momentum, volatility, and regime features.
- `btc_intraday_regime`: BTC 420-minute intraday coarse/fine sweep with regime-oriented signals.
- `nashville_housing_panel`: multi-entity real-estate panel ranking over Nashville-area data.
- `nvda_roc_sma_diff_cross`: NVDA ROC(SMA) differential-cross exhaustive sweep aligned with the TradingView visualizer inputs.

The final NVDA diff-cross demo uses a 336,000-combination exhaustive grid and diversity filtering so the returned Top-K output is duplicate-free and avoids low-trade outliers.

## Hosted Workflow

The hosted path is local-first and loopback-oriented. A typical run is:

1. Start `sugar_server` on `127.0.0.1`.
2. Use `sugar_client run` to upload layout, logic, and CSV data.
3. Poll until completion.
4. Retrieve the retained hosted summary or use `--compact` for full one-line JSON output.

Hosted result payloads use the `sugarbot.hosted.v1` schema and include structured metadata for validation, execution, sweep stages, ranked candidates, diagnostics, and warnings.

## Build And Platform Notes

This package is architecture-neutral source. Build it on the target machine.

Supported validation baseline:

- Windows x86-64 with Visual Studio 2022 MSVC.
- Linux/WSL x86-64 with GCC or Clang.
- Linux ARM64/aarch64 from source on an ARM64 machine.

Native Windows ARM64 remains experimental until validated on real host hardware or trusted CI.

Prebuilt binaries, if produced separately, should be labeled by operating system and architecture. Do not reuse an x86-64 Linux binary on ARM64/aarch64 Linux.

## Validation Baseline

The v0.3.0.0 release package is based on the following validation state:

- Windows build of `sugar_app_tests`, `sugar_contracts`, `sugarbot`, `sugar_server`, and `sugar_client` passed with `0` warnings.
- `sugar_app_tests.exe` passed `866/866`.
- Focused `compile_bridge_sweep_runtime` contracts passed `102/102`.
- Focused `service_protocol_framing` contracts passed `35/35`.
- Full Windows CTest passed `32/32`.
- Windows release smoke passed.
- The final hosted NVDA diff-cross demo completed the 336,000-combination sweep in about 99 seconds on the demo machine, returning duplicate-free Top-K candidates.

## Known Limitations

- SugarBot is a research and experimentation tool, not a production trading system.
- Backtest results and sweep rankings do not imply future performance.
- The hosted service is loopback-first and is not an authenticated internet-facing deployment.
- Secure non-loopback deployment, adaptive/Bayesian sweeps, ensemble workflows, and expanded graph/topology features are deferred future work.
- Native Windows ARM64 support is not part of the validated release baseline.

## Package Contents

`SBv0.3.0.0.zip` includes:

- C++ source under `app/`, `include/`, and `src/`.
- CMake project files and presets.
- Sample CSV data under `data/`.
- Demo and template plans under `graphs/`.
- Hosted observability and architecture docs under `docs/`.
- Release smoke tooling under `tools/`.
- Root `README.md` and quick-start `Readme.txt`.

## License And Use

License: ALL RIGHTS RESERVED.

This package is for internal research and experimentation use at Middle Tennessee State University.
