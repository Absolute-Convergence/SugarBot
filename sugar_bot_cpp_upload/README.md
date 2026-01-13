# Sugar_Bot (C++)

**An object-oriented C++ framework for quantitative analysis and technical trading.**
Reads financial `.csv` candle data, computes indicator signals, backtests strategies, and performs multi-parameter (M×N) sweeps.

## Quick Start
1. Build with preferred C++17+ compiler.
2. Open terminal in folder with x64-Release.
3. Run:
   ```bash
   ./sugar_bot path/to/FILE_NAME.csv
   ```

## Layout 
Repo keeps sources/headers in the root to avoid include-path issues. Optional: refactor into `src/`, `include/` and `app` later.

## Overview:
| Layer              | Role                                                   | Example                                                          |
| :----------------- | :----------------------------------------------------- | :--------------------------------------------------------------- |
| **Data Layer**     | Loads and stores market candles.                       | `CandleSeries`, `load_candles_csv()`                             |
| **Indicators**     | Compute transforms like SMA, EMA, and ROC.             | `SMAIndicator`, `EMAIndicator`, `ROCIndicator`, `ROCOfIndicator` |
| **Strategy Layer** | Defines entry/exit logic from indicator signals.       | `RocSmaCrossoverStrategy`                                        |
| **Backtester**     | Executes a strategy over historical data.              | `Backtester::run()`                                              |
| **Sweeper**        | Performs parameter sweeps to find best configurations. | `sweep_roc_sma()`                                                |


## Repo Structure

```
sugar_Bot/
├─ app/
│  └─ main.cpp
├─ data/                    # sample CSVs 
├─ include/
│  ├─ backtester.h
│  ├─ candle.h
│  ├─ csv.h
│  ├─ indicator.h
│  ├─ indicators_composite.h
│  ├─ indicators_ema.h
│  ├─ indicators_roc.h
│  ├─ indicators_sma.h
│  ├─ metrics.h
│  ├─ series.h
│  ├─ strategy.h
│  ├─ strategy_roc_sma.h
│  ├─ sweep.h
│  └─ utils.h
├─ out/                     # build output (should be .gitignored)
├─ src/
│  ├─ backtester.cpp        # currently absent
│  ├─ csv.cpp
│  ├─ indicators_composite.cpp
│  ├─ indicators_ema.cpp
│  ├─ indicators_roc.cpp
│  ├─ indicators_sma.cpp
│  ├─ series.cpp            # currently absent
│  ├─ strategy_roc_sma.cpp
│  ├─ sweep.cpp             # currently absent
│  └─ utils.cpp
├─ CMakeLists.txt
└─ README.md




```
## Architecture

```
main.cpp
 ├─ Backtester
 │   └─ Strategy (e.g. RocSmaCrossoverStrategy)
 │        ├─ Indicators (SMA, EMA, ROC, ROCOfIndicator)
 │        └─ CandleSeries (market data)
 └─ Sweep (multi-parameter optimization)
```

## Implemented Indicators: 
| Indicator        | Description                                                  |
| :--------------- | :----------------------------------------------------------- |
| `SMAIndicator`   | Simple Moving Average                                        |
| `EMAIndicator`   | Exponential Moving Average                                   |
| `ROCIndicator`   | Rate of Change over closes                                   |
| `ROCOfIndicator` | Composes ROC of any base indicator (e.g. ROC(SMA), ROC(EMA)) |


## License
All Rights Reserved
