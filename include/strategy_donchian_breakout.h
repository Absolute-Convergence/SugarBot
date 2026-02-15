#pragma once

#include <cstddef>
#include <iosfwd>
#include <string>

#include "series.h"
#include "metrics.h"
#include "indicators_donchian.h"   // DonchianOutput

// New spine
#include "strategy_plan.h"         // StrategyPlan, TradeMode, ExecSettings

// Generalized sweep utilities
#include "sweep.h"                // ScorePolicy, TopK

namespace sugar {

    // =============================================================
    // Donchian Breakout (Executor-native template)
    //
    // IMPORTANT: uses lagged (shifted) channel bands to avoid lookahead:
    //   upper_prev = shift(upper, 1)
    //   lower_prev = shift(lower, 1)
    //   exit_prev  = shift(exit_band, 1)
    //
    // Long entry : cross_up(close, upper_prev)
    // Long exit  : cross_down(close, exit_prev)
    // Short entry: cross_down(close, lower_prev)
    // Short exit : cross_up(close, exit_prev)
    // =============================================================

    struct DonchianBreakoutOptions {
        std::size_t period{ 20 };
        DonchianOutput exit_band{ DonchianOutput::Middle };
        TradeMode mode{ TradeMode::LongShort };
        ExecSettings exec{};
    };

    // Build the universal plug (plan).
    StrategyPlan build_donchian_breakout_plan(const DonchianBreakoutOptions& opt);

    // Run the plan through the executor.
    BacktestResult run_donchian_breakout(const CandleSeries& series,
        const DonchianBreakoutOptions& opt);

    // =============================================================
    // Template-native sweep support
    // =============================================================

    struct DonchianSweepRanges {
        // Primary knob
        std::size_t period_min{ 10 };
        std::size_t period_max{ 120 };
        std::size_t period_step{ 1 };

        // Trade mode set
        bool include_long_only{ true };
        bool include_long_short{ true };

        // Keep fixed for v1 (easy to expand later)
        DonchianOutput exit_band{ DonchianOutput::Middle };

        // Generic sweep knobs
        std::size_t top_k{ 10 };
        ScorePolicy score_policy{};
    };

    struct DonchianParams {
        std::size_t period{};
        TradeMode mode{};
        DonchianOutput exit_band{ DonchianOutput::Middle };
    };

    // Prompt sweep ranges (lives with this template, not the menu).
    void prompt_donchian_sweep_ranges(std::istream& in,
        std::ostream& out,
        DonchianSweepRanges& ranges);

    // Run sweep and return Top-K.
    TopK<DonchianParams> sweep_donchian_breakout_topk(const CandleSeries& data,
        const DonchianSweepRanges& ranges,
        const ExecSettings& exec);

    // Convenience: sweep + print + rerun best.
    bool run_donchian_breakout_sweep_best(const CandleSeries& data,
        const DonchianSweepRanges& ranges,
        TradeMode run_mode,           // used only when a range disables one mode
        const ExecSettings& exec,
        std::ostream& out);

    std::string donchian_params_to_string(const DonchianParams& p);

} // namespace sugar