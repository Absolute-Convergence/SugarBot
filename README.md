# SugarBot

SugarBot is a C++ rule-lab for testing decision logic over tabular data. It can ingest CSV data, normalize it into a shared runtime model, compile strategy plans, run backtests, sweep parameters, and expose the same execution path through a local CLI or a loopback TCP server/client pair.

It is useful for:

* Composing and testing signal logic.
* Running repeatable strategy experiments over CSV data.
* Inspecting runtime diagnostics, plan compatibility, and sweep results.
* Exercising a small hosted-service workflow locally.

It is not:

* A predictor.
* A black-box model.
* A production trading system.

License: ALL RIGHTS RESERVED. For internal use at Middle Tennessee State University only.

## Quick Start

Requirements:

* CMake 3.16 or newer
* A C++20 compiler
* Windows: Visual Studio 2022 MSVC x64
* Linux/WSL: GCC or Clang on x86-64 or ARM64/aarch64

SugarBot has no third-party C++ library dependencies.

The artifact paths below match the build commands in this section. The portable Linux / WSL command sets `SUGAR_OUT_ROOT="$PWD/build/artifacts"`, so final binaries land in `./build/artifacts/RelWithDebInfo/`. The portable Windows Visual Studio command leaves final binaries in `.\build\RelWithDebInfo\`. Preset builds use their preset-specific roots, such as `out/Linux/RelWithDebInfo` or `out\build\windows-msvc\RelWithDebInfo`.

### Linux / WSL

From the repository root:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON -DSUGAR_OUT_ROOT="$PWD/build/artifacts"
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Launch the main CLI:

```bash
./build/artifacts/RelWithDebInfo/sugarbot --help
```

### Windows

From a Visual Studio Developer PowerShell or a shell where CMake can find Visual Studio:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTING=ON
cmake --build build --config RelWithDebInfo --target sugarbot sugar_server sugar_client sugar_contracts
ctest --test-dir build -C RelWithDebInfo --output-on-failure
```

Launch the main CLI:

```powershell
.\build\RelWithDebInfo\sugarbot.exe --help
```

## Run The Demos

The polished demo plans live in `graphs/demos/`. They are designed to show current SugarBot behavior beyond the older interactive templates.

### Linux / WSL

Run these from the repository root after the Linux/WSL build:

```bash
./build/artifacts/RelWithDebInfo/sugarbot run \
  graphs/demos/nvda_momentum_volatility.layout.json \
  graphs/demos/nvda_momentum_volatility.planspec.json \
  "data/BATS_NVDA, 1D.csv"

./build/artifacts/RelWithDebInfo/sugarbot run \
  graphs/demos/btc_intraday_regime.layout.json \
  graphs/demos/btc_intraday_regime.planspec.json \
  data/BTCUSD_420.csv

./build/artifacts/RelWithDebInfo/sugarbot run \
  graphs/demos/nashville_housing_panel.layout.json \
  graphs/demos/nashville_housing_panel.planspec.json \
  data/Nashville-Real-Estate-Data-clean.csv

./build/artifacts/RelWithDebInfo/sugarbot run \
  graphs/demos/nvda_momentum_volatility.layout.json \
  graphs/demos/nvda_roc_sma_diff_cross.planspec.json \
  "data/BATS_NVDA, 1D.csv"
```

### Windows

Run these from the repository root after the Windows build:

```powershell
.\build\RelWithDebInfo\sugarbot.exe run `
  graphs\demos\nvda_momentum_volatility.layout.json `
  graphs\demos\nvda_momentum_volatility.planspec.json `
  "data\BATS_NVDA, 1D.csv"

.\build\RelWithDebInfo\sugarbot.exe run `
  graphs\demos\btc_intraday_regime.layout.json `
  graphs\demos\btc_intraday_regime.planspec.json `
  data\BTCUSD_420.csv

.\build\RelWithDebInfo\sugarbot.exe run `
  graphs\demos\nashville_housing_panel.layout.json `
  graphs\demos\nashville_housing_panel.planspec.json `
  data\Nashville-Real-Estate-Data-clean.csv

.\build\RelWithDebInfo\sugarbot.exe run `
  graphs\demos\nvda_momentum_volatility.layout.json `
  graphs\demos\nvda_roc_sma_diff_cross.planspec.json `
  "data\BATS_NVDA, 1D.csv"
```

What they cover:

* `nvda_momentum_volatility`: daily equity Monte Carlo sweep with MACD, Bollinger width, entropy, Hurst, volatility ratio, structured objective scoring, and result-diversity filtering.
* `btc_intraday_regime`: intraday crypto coarse/fine sweep with ISO timestamps, volume/return correlation, entropy, autocorrelation, and regime-change scoring.
* `nashville_housing_panel`: multi-entity real-estate panel ranking over Nashville-area regions using the cleaned housing CSV.
* `nvda_roc_sma_diff_cross`: final NVDA demo companion for the TradingView visualizer; it runs a 336,000-combo exhaustive sweep over ROC(SMA fast) minus ROC(SMA slow) threshold crosses with trade-hash/basin diversity and an 8-trade floor so the returned Top-K is duplicate-free and avoids low-trade outliers.

## Architecture In Brief

SugarBot uses one main runtime path:

```text
CSV -> RawTable -> DataSpec -> preprocess -> NumericFrame/FrameSet
    -> align -> DataView -> compile_plan -> CompiledProgram
    -> StrategyExecutor / sweep
```

The important split is:

* `DataSpec` describes the observation data: parsing, time handling, entity grouping, spatial columns, missing values, headers, and normalization.
* `PlanSpec` describes the strategy logic: nodes, params, bindings, execution settings, and optional sweep metadata.
* `DataView` is the shared runtime shape used by strategy execution and sweeps.

Legacy candle-style CSV ingestion still exists as a compatibility path, but it adapts into the same runtime, there is no second engine.

## Main Executables

* `sugarbot`: main CLI and interactive runtime.
* `SB`: generated alias beside `sugarbot`.
* `sugar_server`: loopback TCP server over the same internal service core.
* `sugar_client`: TCP client for one-command hosted runs and lower-level protocol commands.
* `sugar_contracts`: deterministic contract-suite runner.
* `sugar_app_tests`: app wiring tests; useful for development, not an end-user binary.

## Common Commands

Show CLI help:

```bash
./sugarbot --help
```

Print the accepted layout/data contract:

```bash
./sugarbot --dump-layout
```

Print the accepted strategy-plan contract:

```bash
./sugarbot --dump-plan
```

Inspect input preprocessing:

```bash
./sugarbot --prep-dump path/to/layout.json path/to/data.csv
```

Run the full local contract suite:

```bash
./sugar_contracts
```

Run one focused contract section:

```bash
./sugar_contracts --contract-section service_core
```

## Hosted Client / Server

The hosted surface is local-first. By default, `sugar_server` binds to loopback and uses the `local-ai` service profile. The server and client use the same runtime as `sugarbot run`; the client uploads layout, logic, and CSV files to the server, submits the job, polls until completion, prints the retained hosted summary, and disconnects.

The examples below use port `4123`, the default service port. Use the command block that matches your shell: Linux / WSL examples use POSIX paths and `\` continuations; Windows examples use PowerShell paths, `.exe` suffixes, and backtick continuations.

### Linux / WSL

Run these from a Linux / WSL shell.

Terminal 1:

```bash
mkdir -p hosted_storage
./build/artifacts/RelWithDebInfo/sugar_server \
  --service-profile local-ai \
  --port 4123 \
  --storage-root hosted_storage
```

Terminal 2, run any demo:

```bash
./build/artifacts/RelWithDebInfo/sugar_client run \
  --port 4123 \
  --layout graphs/demos/nvda_momentum_volatility.layout.json \
  --logic graphs/demos/nvda_momentum_volatility.planspec.json \
  --data "data/BATS_NVDA, 1D.csv"

./build/artifacts/RelWithDebInfo/sugar_client run \
  --port 4123 \
  --layout graphs/demos/btc_intraday_regime.layout.json \
  --logic graphs/demos/btc_intraday_regime.planspec.json \
  --data data/BTCUSD_420.csv

./build/artifacts/RelWithDebInfo/sugar_client run \
  --port 4123 \
  --layout graphs/demos/nashville_housing_panel.layout.json \
  --logic graphs/demos/nashville_housing_panel.planspec.json \
  --data data/Nashville-Real-Estate-Data-clean.csv

./build/artifacts/RelWithDebInfo/sugar_client run \
  --port 4123 \
  --layout graphs/demos/nvda_momentum_volatility.layout.json \
  --logic graphs/demos/nvda_roc_sma_diff_cross.planspec.json \
  --data "data/BATS_NVDA, 1D.csv"
```

Stop the server with `Ctrl+C` when finished.

### Windows

Run these from PowerShell on Windows.

Terminal 1:

```powershell
New-Item -ItemType Directory -Force hosted_storage | Out-Null
.\build\RelWithDebInfo\sugar_server.exe `
  --service-profile local-ai `
  --port 4123 `
  --storage-root hosted_storage
```

Terminal 2, run any demo:

```powershell
.\build\RelWithDebInfo\sugar_client.exe run `
  --port 4123 `
  --layout graphs\demos\nvda_momentum_volatility.layout.json `
  --logic graphs\demos\nvda_momentum_volatility.planspec.json `
  --data "data\BATS_NVDA, 1D.csv"

.\build\RelWithDebInfo\sugar_client.exe run `
  --port 4123 `
  --layout graphs\demos\btc_intraday_regime.layout.json `
  --logic graphs\demos\btc_intraday_regime.planspec.json `
  --data data\BTCUSD_420.csv

.\build\RelWithDebInfo\sugar_client.exe run `
  --port 4123 `
  --layout graphs\demos\nashville_housing_panel.layout.json `
  --logic graphs\demos\nashville_housing_panel.planspec.json `
  --data data\Nashville-Real-Estate-Data-clean.csv

.\build\RelWithDebInfo\sugar_client.exe run `
  --port 4123 `
  --layout graphs\demos\nvda_momentum_volatility.layout.json `
  --logic graphs\demos\nvda_roc_sma_diff_cross.planspec.json `
  --data "data\BATS_NVDA, 1D.csv"
```

Stop the server with `Ctrl+C` when finished. Add `--compact` when a script needs the full one-line hosted JSON payload.

## Project Presets

The portable build commands above are the simplest way to build SugarBot on a new machine. The repository also includes CMake presets for day-to-day development.

Native Windows:

```powershell
cmake --preset windows-msvc
cmake --build --preset windows-msvc-release
```

WSL from the mounted Windows checkout:

```powershell
.\tools\Initialize-SugarBuildEnvironment.ps1
```

Then use `linux-debug-mounted` or `linux-release-mounted` from Visual Studio or from WSL.

Native Linux checkout:

```bash
cmake --preset linux-release
cmake --build --preset linux-release
```

## Architecture Notes For Releases

Prebuilt binaries are OS- and architecture-specific. An x86-64 Linux binary will not run on ARM64/aarch64 Linux; it will usually fail with `Exec format error`.

For GitHub releases:

* Include a source archive as the architecture-neutral artifact.
* Label binary assets by OS and architecture, for example `windows-x86_64`, `linux-x86_64`, or `linux-arm64`.
* Only publish a binary for an architecture after building and smoke-testing on that architecture.

Current validation baseline:

* Windows x86-64 with MSVC is supported.
* Linux/WSL x86-64 is supported.
* Linux ARM64/aarch64 should be built from source on an ARM64 machine.
* Native Windows ARM64 is experimental until it has real host or CI validation.

## Repository Layout

* `app/`: CLI parsing, command dispatch, reporting, and entry points.
* `include/`: public headers and stable contracts.
* `src/`: ingest, runtime, compiler, evaluator, sweeps, service core, and contracts.
* `graphs/demos/`: polished runnable examples.
* `graphs/templates/`: built-in interactive templates.
* `graphs/testing/`: parser probes, CLI artifacts, and historical test plans.
* `data/`: sample CSV and layout/data-spec files.
* `docs/`: protocol and architecture notes.
* `tools/`: helper scripts for local builds and release smoke checks.
* `release/`: packaging helpers and release notes.

## More Detail

Useful discovery commands:

```bash
./sugarbot --dump-layout
./sugarbot --dump-plan
./sugarbot --dump-ops
```

For the hosted protocol shape, see `docs/hosted_observability_contract.md`.

For maintenance rules, build-lane notes, and agent workflow constraints, see `AGENTS.md`.

## Disclaimer

SugarBot is a research and experimentation tool. Backtests, reports, and test coverage do not guarantee future performance or production suitability.
