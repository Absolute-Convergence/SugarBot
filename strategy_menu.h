#pragma once

#include <iosfwd>

#include "series.h"
#include "strategy_plan.h" // TradeMode, ExecSettings

#include "strategy_donchian_breakout.h" // DonchianSweepRanges + runner
#include "strategy_gated_diff_cross.h"
#include "strategy_zscore_mean_reversion.h"
#include "strategy_swing_breakout_validation.h"

namespace sugar {

    enum class StrategyTemplate {
        Donchian_Breakout = 1,
        Gated_Diff_Cross = 2,
        ZScore_MeanReversion = 3,
        Swing_Breakout_Validation = 4,
    };

    struct StrategyRunOptions {
        StrategyTemplate templ{ StrategyTemplate::Donchian_Breakout };

        TradeMode mode{ TradeMode::LongShort };

        // Donchian
        DonchianSweepRanges donchian_ranges{};
        ExecSettings donchian_exec{};

        // Gated Diff Cross
        GatedDiffCrossSweepRanges gated_diff_ranges{};
        ExecSettings gated_diff_exec{};

        // ZScore Mean Reversion
        ZScoreMeanReversionSweepRanges zscore_ranges{};
        ExecSettings zscore_exec{};

        // Swing Breakout Validation
        SwingBreakoutValidationSweepRanges swing_ranges{};
        ExecSettings swing_exec{};
    };

    StrategyTemplate prompt_strategy_template(std::istream& in, std::ostream& out);

    TradeMode prompt_trade_mode(std::istream& in,
        std::ostream& out,
        TradeMode def);

    void prompt_donchian_sweep(std::istream& in,
        std::ostream& out,
        StrategyRunOptions& opt);

    void prompt_gated_diff_cross_sweep(std::istream& in,
        std::ostream& out,
        StrategyRunOptions& opt);

    void prompt_zscore_mean_reversion_sweep_ranges(std::istream& in,
        std::ostream& out,
        StrategyRunOptions& opt);

    void prompt_swing_breakout_validation_sweep(std::istream& in,
        std::ostream& out,
        StrategyRunOptions& opt);

    bool run_strategy_template(const CandleSeries& series,
        const StrategyRunOptions& opt,
        std::ostream& out);

} // namespace sugar