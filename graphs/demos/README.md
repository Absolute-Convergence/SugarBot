# SugarBot Demo Plans

These demos are meant to be run after building `sugarbot` or `SB`.

From a Linux/WSL build using the README quick-start layout:

```bash
./build/artifacts/RelWithDebInfo/sugarbot run graphs/demos/nvda_momentum_volatility.layout.json graphs/demos/nvda_momentum_volatility.planspec.json "data/BATS_NVDA, 1D.csv"
./build/artifacts/RelWithDebInfo/sugarbot run graphs/demos/btc_intraday_regime.layout.json graphs/demos/btc_intraday_regime.planspec.json data/BTCUSD_420.csv
./build/artifacts/RelWithDebInfo/sugarbot run graphs/demos/nashville_housing_panel.layout.json graphs/demos/nashville_housing_panel.planspec.json data/Nashville-Real-Estate-Data-clean.csv
./build/artifacts/RelWithDebInfo/sugarbot run graphs/demos/nvda_momentum_volatility.layout.json graphs/demos/nvda_roc_sma_diff_cross.planspec.json "data/BATS_NVDA, 1D.csv"
```

From a Windows build:

```powershell
.\build\RelWithDebInfo\sugarbot.exe run graphs\demos\nvda_momentum_volatility.layout.json graphs\demos\nvda_momentum_volatility.planspec.json "data\BATS_NVDA, 1D.csv"
.\build\RelWithDebInfo\sugarbot.exe run graphs\demos\btc_intraday_regime.layout.json graphs\demos\btc_intraday_regime.planspec.json data\BTCUSD_420.csv
.\build\RelWithDebInfo\sugarbot.exe run graphs\demos\nashville_housing_panel.layout.json graphs\demos\nashville_housing_panel.planspec.json data\Nashville-Real-Estate-Data-clean.csv
.\build\RelWithDebInfo\sugarbot.exe run graphs\demos\nvda_momentum_volatility.layout.json graphs\demos\nvda_roc_sma_diff_cross.planspec.json "data\BATS_NVDA, 1D.csv"
```

## What They Show

- `nvda_momentum_volatility`: daily equity trend/regime sweep with MACD, Bollinger width, entropy, Hurst, volatility ratio, `core.select`, a structured objective, and final result-diversity filtering.
- `btc_intraday_regime`: intraday crypto regime sweep with ISO timestamps, true-range volatility, autocorrelation, entropy, regime-change scoring, and coarse/fine search.
- `nashville_housing_panel`: multi-entity real-estate panel run over four regions, using housing-domain fields, a custom market-heat signal, and a structured reported-metric objective.
- `nvda_roc_sma_diff_cross`: NVDA ROC(SMA) diff-cross sweep matching the TradingView visualizer inputs (`fast_len`, `slow_len`, `roc_len`, `thresh`) with a 336,000-combo exhaustive grid, an 8-trade floor, and trade-hash/basin diversity enabled for duplicate-free Top-K output.
