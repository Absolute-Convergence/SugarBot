# Retired Legacy Files

These files were moved out of active runtime/build paths to preserve reference behavior while reducing legacy coupling.

Retired headers:
- `include/indicators_composite_numeric.h`
- `include/indicators_zscore.h`
- `include/indicators_sma.h`
- `include/indicators_ema.h`
- `include/indicators_roc.h`
- `include/indicators_stddev.h`
- `include/indicators_atr.h`
- `include/indicators_rsi.h`
- `include/atr_kernels.h`
- `include/rsi_kernels.h`
- `include/indicators_donchian.h`
- `include/indicators_composite_bool.h`

Retired sources:
- `src/indicators_composite_numeric.cpp`
- `src/indicators_zscore.cpp`
- `src/indicators_sma.cpp`
- `src/indicators_ema.cpp`
- `src/indicators_roc.cpp`
- `src/indicators_stddev.cpp`
- `src/indicators_atr.cpp`
- `src/indicators_rsi.cpp`
- `src/atr_kernels.cpp`
- `src/rsi_kernels.cpp`
- `src/indicators_donchian.cpp`
- `src/indicators_composite_bool.cpp`

Canonical active replacements:
- Graph composition: `include/indicator_graph.h`, `src/indicator_graph.cpp`
- Bool graph composition (logic/cmp/cross): `include/indicator_graph.h`, `src/indicator_graph.cpp`
- Shared vector math: `include/vector_kernels.h`, `src/vector_kernels.cpp`
- ATR/RSI TS kernels: `src/op_kernels_ts.h`, `src/op_kernels_ts.cpp` (`ts_true_range_ohlc`, `ts_true_range_delta`, `ts_atr_wilder`, `ts_atr_from_series`, `ts_rsi_wilder`)
- Donchian strategy type contract: `include/donchian_types.h` (`DonchianOutput`)
- Donchian runtime math path: `src/op_kernels_ts.cpp` (`ts_roll_max`, `ts_roll_min`) + strategy graph wiring in `src/strategy_donchian_breakout.cpp`

Notes:
- Keep retired files for behavior-drift forensics (NaN propagation, warmup semantics, edge cases).
- Do not reintroduce retired files into runtime include/compile paths.
