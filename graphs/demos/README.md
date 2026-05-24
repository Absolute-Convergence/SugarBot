# SugarBot Demo Plans

These demos are meant to be run after building `sugarbot` or `SB`.

From a Linux/WSL build using the README quick-start layout:

```bash
./build/artifacts/RelWithDebInfo/sugarbot run graphs/demos/quickstart_ohlcv.layout.json graphs/demos/quickstart_ohlcv.planspec.json data/sample.csv
./build/artifacts/RelWithDebInfo/sugarbot run graphs/demos/nvda_momentum_volatility.layout.json graphs/demos/nvda_momentum_volatility.planspec.json "data/BATS_NVDA, 1D.csv"
./build/artifacts/RelWithDebInfo/sugarbot run graphs/demos/btc_intraday_regime.layout.json graphs/demos/btc_intraday_regime.planspec.json data/BTCUSD_420.csv
./build/artifacts/RelWithDebInfo/sugarbot run graphs/demos/nashville_housing_panel.layout.json graphs/demos/nashville_housing_panel.planspec.json data/Nashville-Real-Estate-Data-clean.csv
./build/artifacts/RelWithDebInfo/sugarbot run graphs/demos/nvda_momentum_volatility.layout.json graphs/demos/nvda_roc_sma_diff_cross.planspec.json "data/BATS_NVDA, 1D.csv"
./build/artifacts/RelWithDebInfo/sugarbot run graphs/demos/quickstart_ohlcv.layout.json graphs/demos/sweep_engines/i_exhaustive_threshold_sweep.planspec.json data/sample.csv --report compact
./build/artifacts/RelWithDebInfo/sugarbot run graphs/demos/quickstart_ohlcv.layout.json graphs/demos/sweep_engines/i_monte_carlo_threshold_sweep.planspec.json data/sample.csv --report compact
./build/artifacts/RelWithDebInfo/sugarbot run graphs/demos/quickstart_ohlcv.layout.json graphs/demos/sweep_engines/i_coarse_fine_threshold_sweep.planspec.json data/sample.csv --report compact
./build/artifacts/RelWithDebInfo/sugarbot run graphs/demos/quickstart_ohlcv.layout.json graphs/demos/sweep_engines/i_bayesian_posterior_threshold_sweep.planspec.json data/sample.csv --report posterior
./build/artifacts/RelWithDebInfo/sugarbot run graphs/demos/quickstart_ohlcv.layout.json graphs/demos/sweep_engines/i_advanced_report_projection_sweep.planspec.json data/sample.csv --report pareto
./build/artifacts/RelWithDebInfo/sugarbot run graphs/demos/quickstart_ohlcv.layout.json graphs/demos/sweep_engines/i_advanced_report_projection_sweep.planspec.json data/sample.csv --report trace
./build/artifacts/RelWithDebInfo/sugarbot run graphs/demos/topology_readiness/f8_relational_time_graph.layout.json graphs/demos/topology_readiness/f8_relational_time_graph.planspec.json data/topology_readiness/f8_relational_time_graph.csv --report topology
./build/artifacts/RelWithDebInfo/sugarbot run graphs/demos/topology_readiness/f8_hierarchy_semantic.layout.json graphs/demos/topology_readiness/f8_hierarchy_semantic.planspec.json data/topology_readiness/f8_hierarchy_semantic.csv --report topology
./build/artifacts/RelWithDebInfo/sugarbot run graphs/demos/quickstart_ohlcv.layout.json graphs/demos/dynamic_execution/g10_constant_declared_dynamic.planspec.json data/sample.csv
./build/artifacts/RelWithDebInfo/sugarbot run graphs/demos/quickstart_ohlcv.layout.json graphs/demos/dynamic_execution/g10_row_varying_declared_dynamic.planspec.json data/sample.csv
./build/artifacts/RelWithDebInfo/sugarbot run graphs/demos/topology_readiness/f8_relational_time_graph.layout.json graphs/demos/dynamic_execution/g10_topology_declared_dynamic.planspec.json data/topology_readiness/f8_relational_time_graph.csv --report json
```

From a Windows build:

```powershell
.\build\RelWithDebInfo\sugarbot.exe run graphs\demos\quickstart_ohlcv.layout.json graphs\demos\quickstart_ohlcv.planspec.json data\sample.csv
.\build\RelWithDebInfo\sugarbot.exe run graphs\demos\nvda_momentum_volatility.layout.json graphs\demos\nvda_momentum_volatility.planspec.json "data\BATS_NVDA, 1D.csv"
.\build\RelWithDebInfo\sugarbot.exe run graphs\demos\btc_intraday_regime.layout.json graphs\demos\btc_intraday_regime.planspec.json data\BTCUSD_420.csv
.\build\RelWithDebInfo\sugarbot.exe run graphs\demos\nashville_housing_panel.layout.json graphs\demos\nashville_housing_panel.planspec.json data\Nashville-Real-Estate-Data-clean.csv
.\build\RelWithDebInfo\sugarbot.exe run graphs\demos\nvda_momentum_volatility.layout.json graphs\demos\nvda_roc_sma_diff_cross.planspec.json "data\BATS_NVDA, 1D.csv"
.\build\RelWithDebInfo\sugarbot.exe run graphs\demos\quickstart_ohlcv.layout.json graphs\demos\sweep_engines\i_exhaustive_threshold_sweep.planspec.json data\sample.csv --report compact
.\build\RelWithDebInfo\sugarbot.exe run graphs\demos\quickstart_ohlcv.layout.json graphs\demos\sweep_engines\i_monte_carlo_threshold_sweep.planspec.json data\sample.csv --report compact
.\build\RelWithDebInfo\sugarbot.exe run graphs\demos\quickstart_ohlcv.layout.json graphs\demos\sweep_engines\i_coarse_fine_threshold_sweep.planspec.json data\sample.csv --report compact
.\build\RelWithDebInfo\sugarbot.exe run graphs\demos\quickstart_ohlcv.layout.json graphs\demos\sweep_engines\i_bayesian_posterior_threshold_sweep.planspec.json data\sample.csv --report posterior
.\build\RelWithDebInfo\sugarbot.exe run graphs\demos\quickstart_ohlcv.layout.json graphs\demos\sweep_engines\i_advanced_report_projection_sweep.planspec.json data\sample.csv --report pareto
.\build\RelWithDebInfo\sugarbot.exe run graphs\demos\quickstart_ohlcv.layout.json graphs\demos\sweep_engines\i_advanced_report_projection_sweep.planspec.json data\sample.csv --report trace
.\build\RelWithDebInfo\sugarbot.exe run graphs\demos\topology_readiness\f8_relational_time_graph.layout.json graphs\demos\topology_readiness\f8_relational_time_graph.planspec.json data\topology_readiness\f8_relational_time_graph.csv --report topology
.\build\RelWithDebInfo\sugarbot.exe run graphs\demos\topology_readiness\f8_hierarchy_semantic.layout.json graphs\demos\topology_readiness\f8_hierarchy_semantic.planspec.json data\topology_readiness\f8_hierarchy_semantic.csv --report topology
.\build\RelWithDebInfo\sugarbot.exe run graphs\demos\quickstart_ohlcv.layout.json graphs\demos\dynamic_execution\g10_constant_declared_dynamic.planspec.json data\sample.csv
.\build\RelWithDebInfo\sugarbot.exe run graphs\demos\quickstart_ohlcv.layout.json graphs\demos\dynamic_execution\g10_row_varying_declared_dynamic.planspec.json data\sample.csv
.\build\RelWithDebInfo\sugarbot.exe run graphs\demos\topology_readiness\f8_relational_time_graph.layout.json graphs\demos\dynamic_execution\g10_topology_declared_dynamic.planspec.json data\topology_readiness\f8_relational_time_graph.csv --report json
```

## What They Show

- `quickstart_ohlcv`: fast first-success OHLCV workflow. The checked-in layout and PlanSpec are under `graphs/demos/`; the example CSV is `data/sample.csv`.
- `nvda_momentum_volatility`: daily equity trend/regime sweep with MACD, Bollinger width, entropy, Hurst, volatility ratio, `core.select`, a structured objective, and final result-diversity filtering.
- `btc_intraday_regime`: intraday crypto regime sweep with ISO timestamps, true-range volatility, autocorrelation, entropy, regime-change scoring, and coarse/fine search.
- `nashville_housing_panel`: multi-entity real-estate panel run over four regions, using housing-domain fields, a custom market-heat signal, and a structured reported-metric objective.
- `nvda_roc_sma_diff_cross`: NVDA ROC(SMA) diff-cross sweep matching the TradingView visualizer inputs (`fast_len`, `slow_len`, `roc_len`, `thresh`) with a 336,000-combo exhaustive grid, an 8-trade floor, and trade-hash/basin diversity enabled for duplicate-free Top-K output.
- `sweep_engines/i_*`: Milestone I tutorial fixtures. The engine examples reuse the quickstart layout and sample CSV while changing only `sweep.engine` and typed `sweep.engine_config` across exhaustive, Monte Carlo, coarse/fine, and Bayesian posterior examples. `i_advanced_report_projection_sweep` enables result diversity, Pareto, search geometry, duplicate trade-path, replay trace, table/objective, and JSON report projections over one small sweep.
- `topology_readiness/f8_relational_time_graph`: F8 readiness fixture for directed/weighted relational graph topology, time-valid edges, version overlays, generic topology operators, and focused topology diagnostics.
- `topology_readiness/f8_hierarchy_semantic`: F8 readiness fixture for hierarchy traversal and semantic embedding topology through the same `topology.*` operator/report path.
- `dynamic_execution/g10_*`: Milestone G10 declared dynamic execution demos for constant-route equivalence, row-varying declared routing, and topology-informed routing over the F8 row-domain graph.
