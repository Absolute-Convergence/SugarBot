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

License: ALL RIGHTS RESERVED.

## Quick Start

Requirements:

* CMake 3.16 or newer
* A C++20 compiler
* Windows: Visual Studio 2022 MSVC x64
* Linux/WSL: GCC or Clang on x86-64 or ARM64/aarch64

SugarBot's default build has no third-party C++ library dependencies. Optional Parquet support requires Apache Arrow C++ and Parquet C++ and is enabled with `-DSUGAR_ENABLE_PARQUET=ON`.

The commands below build only the main `sugarbot` executable. That is enough for `--help`, `--onramp`, the interactive tutorial, and local `sugarbot run ...` commands.

Run commands from the repository root unless a section says otherwise. On Windows, you can open File Explorer to the SugarBot folder, click the address bar, type `powershell`, and press Enter.

The beginner commands are:

* `sugarbot --help`: print the command reference.
* `sugarbot --onramp`: explain the CSV + layout + PlanSpec workflow.
* `sugarbot`: start the interactive tutorial.

The interactive tutorial uses checked-in files from `data/` and `graphs/demos/`, so start it from the repository root or from a full package that includes those folders. `--help` and `--onramp` do not need the demo files.

### CSV And Parquet

CSV remains the default format for small examples and files you want to inspect by hand. When SugarBot is built with `SUGAR_ENABLE_PARQUET=ON`, commands that accept tabular data can also read `.parquet` and `.pq` files, and feature dumps can be written as Parquet.

Parquet support is local CLI/library support in v1. Hosted service Parquet upload/output remains deferred until the service protocol has a binary-safe upload contract.

Use Parquet instead of CSV when you care about:

| Concern | CSV | Parquet |
|---|---|---|
| File size | Larger | Usually much smaller |
| Type preservation | Weak | Strong |
| Analytics reads | Slower | Faster, especially column scans |
| Spark / DuckDB / pandas / Synapse use | Usable | Preferred |
| Human readability | High | Low |
| Implementation simplicity | Easy | More setup |

Examples:

```bash
sugarbot run layout.json logic.json data.parquet
sugarbot --obs-report layout.json data.parquet
sugarbot --feature-dump layout.json logic.json data.parquet --output-format parquet --output features.parquet
```

### Windows: Build And Run

Open a Visual Studio Developer PowerShell, or another PowerShell where CMake can find Visual Studio, and run:

```powershell
cmake -S . -B build\windows-msvc -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTING=ON
cmake --build build\windows-msvc --config RelWithDebInfo --target sugarbot
.\build\windows-msvc\RelWithDebInfo\sugarbot.exe --help
.\build\windows-msvc\RelWithDebInfo\sugarbot.exe --onramp
.\build\windows-msvc\RelWithDebInfo\sugarbot.exe
```

### Linux / WSL: Build And Run

Open a Linux or WSL terminal in the repository root and run:

```bash
cmake -S . -B build/linux-release -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON -DSUGAR_OUT_ROOT="$PWD/build/linux-artifacts"
cmake --build build/linux-release --target sugarbot --parallel
./build/linux-artifacts/RelWithDebInfo/sugarbot --help
./build/linux-artifacts/RelWithDebInfo/sugarbot --onramp
./build/linux-artifacts/RelWithDebInfo/sugarbot
```

### Visual Studio Or CMake Preset Builds

Visual Studio and the repository presets write to `out/...`, not to the portable `build/...` tree above. If you built in an IDE, use these paths.

Windows IDE / `windows-msvc` preset output:

```powershell
.\out\build\windows-msvc\RelWithDebInfo\sugarbot.exe --help
.\out\build\windows-msvc\RelWithDebInfo\sugarbot.exe --onramp
.\out\build\windows-msvc\RelWithDebInfo\sugarbot.exe
```

Linux/WSL IDE / `linux-release` preset output:

```bash
./out/Linux/RelWithDebInfo/sugarbot --help
./out/Linux/RelWithDebInfo/sugarbot --onramp
./out/Linux/RelWithDebInfo/sugarbot
```

### Prebuilt Executables

If `sugarbot.exe` is already built and sitting in your current Windows folder:

```powershell
.\sugarbot.exe --help
.\sugarbot.exe --onramp
.\sugarbot.exe
```

If `sugarbot` is already built and sitting in your current Linux/WSL folder:

```bash
chmod +x ./sugarbot
./sugarbot --help
./sugarbot --onramp
./sugarbot
```

For the no-argument interactive tutorial, type the menu number you want and press Enter. Type `q` at a menu prompt to leave without running anything.

## Run The Demos

The polished demo plans live in `graphs/demos/`. They are designed to show current SugarBot behavior beyond the older interactive templates.

The commands below use the portable `build/...` output paths from Quick Start. If you built from Visual Studio or a CMake preset, replace the executable path with the matching `out/...` path from the preset section.

### Linux / WSL

Run these from the repository root after the Linux/WSL build:

```bash
./build/linux-artifacts/RelWithDebInfo/sugarbot run \
  graphs/demos/nvda_momentum_volatility.layout.json \
  graphs/demos/nvda_momentum_volatility.planspec.json \
  "data/BATS_NVDA, 1D.csv"

./build/linux-artifacts/RelWithDebInfo/sugarbot run \
  graphs/demos/btc_intraday_regime.layout.json \
  graphs/demos/btc_intraday_regime.planspec.json \
  data/BTCUSD_420.csv

./build/linux-artifacts/RelWithDebInfo/sugarbot run \
  graphs/demos/nashville_housing_panel.layout.json \
  graphs/demos/nashville_housing_panel.planspec.json \
  data/Nashville-Real-Estate-Data-clean.csv

./build/linux-artifacts/RelWithDebInfo/sugarbot run \
  graphs/demos/nvda_momentum_volatility.layout.json \
  graphs/demos/nvda_roc_sma_diff_cross.planspec.json \
  "data/BATS_NVDA, 1D.csv"
```

### Windows

Run these from the repository root after the Windows build:

```powershell
.\build\windows-msvc\RelWithDebInfo\sugarbot.exe run `
  graphs\demos\nvda_momentum_volatility.layout.json `
  graphs\demos\nvda_momentum_volatility.planspec.json `
  "data\BATS_NVDA, 1D.csv"

.\build\windows-msvc\RelWithDebInfo\sugarbot.exe run `
  graphs\demos\btc_intraday_regime.layout.json `
  graphs\demos\btc_intraday_regime.planspec.json `
  data\BTCUSD_420.csv

.\build\windows-msvc\RelWithDebInfo\sugarbot.exe run `
  graphs\demos\nashville_housing_panel.layout.json `
  graphs\demos\nashville_housing_panel.planspec.json `
  data\Nashville-Real-Estate-Data-clean.csv

.\build\windows-msvc\RelWithDebInfo\sugarbot.exe run `
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

These examples assume your shell is already in the folder that contains `sugarbot`. If you are running from the repository root, use the full executable path shown in Quick Start.

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

This section uses `sugar_server` and `sugar_client`, which are separate executables. The Quick Start builds only `sugarbot`; build these extra targets before using the hosted examples.

The hosted examples below use the portable `build/...` paths. If you built from Visual Studio or a CMake preset, build the same targets there and use the matching `out/...` executable paths.

The examples below use port `4123`, the default service port. Use the command block that matches your shell: Linux / WSL examples use POSIX paths and `\` continuations; Windows examples use PowerShell paths, `.exe` suffixes, and backtick continuations.

### Linux / WSL

Run these from a Linux / WSL shell.

Build the hosted executables if needed:

```bash
cmake --build build/linux-release --target sugar_server sugar_client --parallel
```

Terminal 1:

```bash
mkdir -p hosted_storage
./build/linux-artifacts/RelWithDebInfo/sugar_server \
  --service-profile local-ai \
  --port 4123 \
  --storage-root hosted_storage
```

Terminal 2, run any demo:

```bash
./build/linux-artifacts/RelWithDebInfo/sugar_client run \
  --port 4123 \
  --layout graphs/demos/nvda_momentum_volatility.layout.json \
  --logic graphs/demos/nvda_momentum_volatility.planspec.json \
  --data "data/BATS_NVDA, 1D.csv"

./build/linux-artifacts/RelWithDebInfo/sugar_client run \
  --port 4123 \
  --layout graphs/demos/btc_intraday_regime.layout.json \
  --logic graphs/demos/btc_intraday_regime.planspec.json \
  --data data/BTCUSD_420.csv

./build/linux-artifacts/RelWithDebInfo/sugar_client run \
  --port 4123 \
  --layout graphs/demos/nashville_housing_panel.layout.json \
  --logic graphs/demos/nashville_housing_panel.planspec.json \
  --data data/Nashville-Real-Estate-Data-clean.csv

./build/linux-artifacts/RelWithDebInfo/sugar_client run \
  --port 4123 \
  --layout graphs/demos/nvda_momentum_volatility.layout.json \
  --logic graphs/demos/nvda_roc_sma_diff_cross.planspec.json \
  --data "data/BATS_NVDA, 1D.csv"
```

Stop the server with `Ctrl+C` when finished.

### Windows

Run these from PowerShell on Windows.

Build the hosted executables if needed:

```powershell
cmake --build build\windows-msvc --config RelWithDebInfo --target sugar_server sugar_client
```

Terminal 1:

```powershell
New-Item -ItemType Directory -Force hosted_storage | Out-Null
.\build\windows-msvc\RelWithDebInfo\sugar_server.exe `
  --service-profile local-ai `
  --port 4123 `
  --storage-root hosted_storage
```

Terminal 2, run any demo:

```powershell
.\build\windows-msvc\RelWithDebInfo\sugar_client.exe run `
  --port 4123 `
  --layout graphs\demos\nvda_momentum_volatility.layout.json `
  --logic graphs\demos\nvda_momentum_volatility.planspec.json `
  --data "data\BATS_NVDA, 1D.csv"

.\build\windows-msvc\RelWithDebInfo\sugar_client.exe run `
  --port 4123 `
  --layout graphs\demos\btc_intraday_regime.layout.json `
  --logic graphs\demos\btc_intraday_regime.planspec.json `
  --data data\BTCUSD_420.csv

.\build\windows-msvc\RelWithDebInfo\sugar_client.exe run `
  --port 4123 `
  --layout graphs\demos\nashville_housing_panel.layout.json `
  --logic graphs\demos\nashville_housing_panel.planspec.json `
  --data data\Nashville-Real-Estate-Data-clean.csv

.\build\windows-msvc\RelWithDebInfo\sugar_client.exe run `
  --port 4123 `
  --layout graphs\demos\nvda_momentum_volatility.layout.json `
  --logic graphs\demos\nvda_roc_sma_diff_cross.planspec.json `
  --data "data\BATS_NVDA, 1D.csv"
```

Stop the server with `Ctrl+C` when finished. Add `--compact` for concise hosted output: `run`/`result` print the canonical nested `job_result.metadata.report`, while `status` prints progress and retention facts for long or canceled jobs. Use `--raw-json` when a script needs the full one-line protocol JSON payload. The older hosted `metadata.primary_backtest`, `metadata.sweep`, and `metadata.panel` fields remain compatibility aliases; new tooling should read `metadata.report`.

## Project Presets

The portable build commands above are the simplest way to build SugarBot on a new machine. The repository also includes CMake presets for day-to-day development.

Native Windows:

```powershell
cmake --preset windows-msvc
cmake --build --preset windows-msvc-release --target sugarbot
```

WSL from the mounted Windows checkout:

```powershell
.\tools\Initialize-SugarBuildEnvironment.ps1
```

Then use `linux-debug-mounted` or `linux-release-mounted` from Visual Studio, or run this from WSL in the mounted repository:

```bash
cmake --preset linux-release-mounted
cmake --build --preset linux-release-mounted --target sugarbot
```

Native Linux checkout:

```bash
cmake --preset linux-release
cmake --build --preset linux-release --target sugarbot
```

Omit `--target sugarbot` when you intentionally want to build every default target.

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
