READ ME: Last updated: 01/29/2026

Abstract: 

**SugarBot** is a C++ strategy experimentation engine for time-series data.

It is designed to answer one question rigorously and at scale:

> **“What would have happened if I made this decision differently?”**

SugarBot is not a predictor, not a black box, and not a single trading strategy.
It is a **decision-rule laboratory**: a system for building, composing, executing,
and evaluating rule-based strategies over historical data.

SugarBot is an active research and learning project, not a production trading system.

License:
ALL RIGHTS RESERVED

Copyright ©
All rights to the source code, architecture, and associated materials are reserved.
No use, redistribution, or modification is permitted without explicit authorization.

---

###Introduction:

## What SugarBot does (high level)

SugarBot takes historical time-series data (e.g. OHLCV candles) and:

1. Transforms that data into **signals** using modular indicators
2. Combines signals into **decision rules** (strategies)
3. Executes those rules through a unified runtime engine
4. Scores outcomes using explicit performance metrics
5. Repeats the process across large parameter spaces via **sweeps**

The goal is not prediction — it is **structured exploration** of decision logic.

Think of SugarBot as a **wind tunnel for strategies**.

---

## Core architectural ideas

SugarBot is intentionally built from clean, orthogonal layers.

### 1. Data model: `CandleSeries` and `SeriesView`

- `CandleSeries` owns raw OHLCV data loaded from CSV
- `SeriesView` provides a time-aligned, column-oriented view over the same data

This separation allows:
- safe indexing by time
- efficient indicator computation
- consistent alignment across all downstream components

---

### 2. Indicators: signals, not decisions

Indicators are **pure transformations** from one series to another.
They do not place trades or make decisions.

Included indicators span multiple categories:

- **Trend**: SMA, EMA
- **Momentum**: ROC
- **Volatility**: ATR, StdDev (SMA & EMA variants)
- **Oscillators**: RSI, Z-Score
- **Channels**: Donchian (upper, lower, mid, width)

All indicators:
- respect explicit warm-up contracts
- propagate NaNs until valid
- are composable by design

---

### 3. Indicator composition (graphs)

Indicators can be built *from other indicators*.

Examples:
- ROC of an EMA
- Z-Score of a rolling standard deviation
- Boolean signals derived from numeric series

This enables **indicator graphs**, not just flat pipelines,
without rewriting compute logic or coupling strategies to raw data.

---

### 4. StrategyPlan: declarative strategy wiring

A **StrategyPlan** defines *what a strategy is*, not *how it runs*.

A plan encapsulates:
- strategy identity and options
- indicator graph construction
- parameter definitions and defaults

Plans do **not** execute trades.
They describe the structure of a strategy in a declarative way.

This separation is what makes runtime selection, sweeping,
and reuse possible.

---

### 5. Strategy execution (runtime engine)

The **StrategyExecutor** is strategy-agnostic.

It:
- consumes a `StrategyPlan`
- evaluates indicators over a series
- applies entry/exit rules
- produces a trade stream

Because execution is decoupled from strategy definition:
- strategies can be swapped at runtime
- execution logic stays centralized and testable

---

### 6. Strategy templates

Concrete strategies are implemented as **templates** that provide:
- options
- plan construction
- execution rules
- sweep configuration

Included reference templates:

- **Gated Diff Cross**  
  The canonical example of how to build a strategy in SugarBot.
  Serves as the architectural reference for future strategies.

- **Donchian Breakout**  
  A channel-based strategy demonstrating rolling-window semantics
  and correct “previous-state” event logic.

---

### 7. Sweeps: structured parameter exploration

SugarBot treats parameter search as a first-class concept.

The sweep system:
- is strategy-agnostic
- explores parameter grids (coarse → fine)
- tracks Top-K results
- supports train/validate evaluation

Sweeps are not bolted on — they are part of the core design.

---

### 8. Metrics: explicit scoring

Strategies are evaluated using explicit metrics such as:
- PnL
- drawdown
- trade count
- other extensible performance measures

Metrics are separated from execution so scoring can evolve independently.

---

## Method (simplified)

CSV → CandleSeries
↓
SeriesView
↓
Indicators
↓
StrategyPlan
↓
StrategyExecutor
↓
Backtester
↓
Metrics / Sweeps

Each layer has a single responsibility.
No layer “reaches around” another.

---

## Design / Methodology

Most backtesting systems entangle:
- data
- indicators
- strategy logic
- execution
- optimization

SugarBot deliberately **pulls these apart**.

That separation makes it possible to:
- swap indicators without touching strategies
- swap strategies without touching execution
- explore large parameter spaces safely
- reason about *why* a result occurred

SugarBot is designed to make decision logic **explicit and inspectable**.

---

## What SugarBot is *not*

- Not a trading bot
- Not a prediction engine
- Not a machine-learning model
- Not a black box

It does not guess the future.

It rigorously explores the past so you can understand
which decisions mattered — and which didn’t.

---

## Implementation

SugarBot is suitable for:
- quantitative strategy research
- decision-rule experimentation
- educational exploration of time-series systems
- generating structured datasets for downstream ML

The architecture is intentionally general:
anything that looks like *“if-this-then-that over time”* can fit.

---

## Disclaimer

SugarBot is a research and experimentation tool.
Backtests do not guarantee future performance.
Overfitting is real.

---

## System Overview & Repo Structure

A strategy-centric, object-oriented C++ framework for quantitative analysis, backtesting, and parameter optimization.
SugarBot reads OHLCV candle .csv data, constructs composable indicator graphs (numeric + boolean), executes declarative
StrategyPlans via a runtime StrategyExecutor (FSM), and performs strategy-agnostic parameter sweeps (Top-K, coarse→fine,
optional train/validate) to discover strong configurations.

Quick Start:
1) Build with a C++17+ compiler (CMake-based build recommended).
2) Run:
   ./sugar_bot path/to/FILE.csv
3) Use the runtime menu to select a strategy template and run a backtest or sweep.

Tip: main.cpp also contains a debug/validation harness used to verify indicator alignment, warm-up behavior, and NaN
propagation on real CSV data.


Overview:
| Layer                 | Role                                                         | Example / Notes                                                |
| :-------------------- | :----------------------------------------------------------- | :------------------------------------------------------------- |
| **Data Layer**        | Loads and owns historical market candles.                    | `Candle`, `CandleSeries`, `load_candles_csv()`                 |
| **Execution View**    | Read-only projection used for indicator execution.           | `SeriesView`                                                   |
| **Indicators**        | Numeric & boolean signal graphs (composable).                | `Indicator`, composite numeric/bool builders                   |
| **Strategy Plan**     | Declarative strategy identity + indicator wiring + params.   | `StrategyPlan`                                                 |
| **Strategy Executor** | Runtime FSM that converts plan + signals into trades.        | `StrategyExecutor`                                             |
| **Backtester**        | Orchestrates execution over historical data.                 | `Backtester::run()`                                            |
| **Sweeper / Optimizer** | Parameter search and ranking (strategy-agnostic).          | `sweep.*`, Top-K, coarse→fine, train/validate                  |
| **Metrics**           | Reporting boundary for results and summaries.                | `BacktestResult`, `metrics.*`                                  |


Repo Structure (logical grouping):

app/
 └─ main.cpp

core/
 ├─ candle.h
 ├─ series.h / series.cpp
 ├─ series_view.h
 ├─ csv.h
 └─ utils.h / utils.cpp

indicators/
 ├─ indicator.h
 ├─ indicators_sma.h
 ├─ indicators_ema.h
 ├─ indicators_roc.h
 ├─ indicators_stddev.h
 ├─ indicators_zscore.h
 ├─ indicators_atr.h
 ├─ indicators_rsi.h
 ├─ indicators_donchian.h
 ├─ indicators_composite_numeric.h / indicators_composite_numeric.cpp
 └─ indicators_composite_bool.h / indicators_composite_bool.cpp

strategy/
 ├─ strategy_plan.h
 ├─ strategy_executor.h / strategy_executor.cpp
 ├─ strategy_menu.h / strategy_menu.cpp
 ├─ strategy_gated_diff_cross.h / strategy_gated_diff_cross.cpp
 └─ strategy_donchian_breakout.h

engine/
 ├─ backtester.h / backtester.cpp
 ├─ sweep.h / sweep.cpp
 └─ metrics.h


Architecture:
main.cpp
 ├─ StrategyMenu (runtime selection)
 │   └─ StrategyPlan (declarative wiring)
 │        └─ StrategyExecutor (runtime FSM)
 │             ├─ Indicators (numeric + boolean composition graphs)
 │             │    └─ SeriesView (read-only access over CandleSeries)
 │             └─ Entry/Exit events → trades
 ├─ Backtester
 │    └─ BacktestResult + Metrics
 └─ Sweep Engine
      └─ Top-K ranking, coarse→fine search, optional train/validate split


Implemented Strategies (templates):
| Strategy Template            | Description                                                                 |
| :-------------------------- | :-------------------------------------------------------------------------- |
| `GatedDiffCross`            | Canonical reference template: gating + thresholds + composition + clean separation. |
| `DonchianBreakout`          | Concrete example: single-indicator breakout implemented through Plan/Executor.      |


Implemented Indicators:
| Indicator / Component              | Description                                                          |
| :-------------------------------- | :------------------------------------------------------------------- |
| `SMA`                             | Simple Moving Average                                                |
| `EMA`                             | Exponential Moving Average                                           |
| `ROC`                             | Rate of Change                                                       |
| `StdDev`                          | Rolling standard deviation (SMA & EMA variants)                      |
| `ZScore`                          | Normalized deviation from rolling mean (SMA & EMA variants)          |
| `ATR`                             | Average True Range                                                   |
| `RSI`                             | Relative Strength Index                                              |
| `Donchian`                        | Upper / Lower / Mid / Width channels                                 |
| `Composite Numeric`               | Functional composition tools for numeric indicator graphs            |
| `Composite Bool`                  | Comparators/gates for boolean signal graphs                          |

Notes:
- Indicators are designed to be composed (graphs) rather than extended via deep subclass trees.
- Warm-up behavior and NaN propagation are explicit to prevent silent alignment bugs common in TA stacks.


License:
ALL RIGHTS RESERVED

Copyright ©
All rights to the source code, architecture, and associated materials are reserved.
No use, redistribution, or modification is permitted without explicit authorization.
