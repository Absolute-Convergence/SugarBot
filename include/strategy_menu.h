#pragma once

#include <iosfwd>

#include "series.h"
#include "strategy_plan.h" // TradeMode, ExecSettings

// Strategy templates are modular. Some repos/builds may not include all.
#if __has_include("strategy_donchian_breakout.h")
#include "strategy_donchian_breakout.h" 
#define SUGAR_HAS_DONCHIAN 1
#else
#define SUGAR_HAS_DONCHIAN 0
#endif

#include "strategy_gated_diff_cross.h" // Base Template For all Strategies

#if __has_include("strategy_zscore_mean_reversion.h")
#include "strategy_zscore_mean_reversion.h"
#define SUGAR_HAS_ZSCORE 1
#else
#define SUGAR_HAS_ZSCORE 0
#endif

#if __has_include("strategy_swing_breakout_validation.h")
#include "strategy_swing_breakout_validation.h"
#define SUGAR_HAS_SWING 1
#else
#define SUGAR_HAS_SWING 0
#endif

#include <string>

#include "plan_spec.h"
#include "plan_compiler.h"
#include "op_registry.h"
#include "strategy_executor.h"

namespace sugar {

    enum class StrategyTemplate {
        Donchian_Breakout = 1,
        Gated_Diff_Cross = 2,
        ZScore_MeanReversion = 3,
        Swing_Breakout_Validation = 4,
        Custom_User_PlanSpec = 99,
    };

    struct StrategyRunOptions {
        StrategyTemplate templ{ StrategyTemplate::Donchian_Breakout };
        TradeMode mode{ TradeMode::LongShort };

#if SUGAR_HAS_DONCHIAN
        DonchianSweepRanges donchian_ranges{};
        ExecSettings donchian_exec{};
#endif

        GatedDiffCrossSweepRanges gated_diff_ranges{};
        ExecSettings gated_diff_exec{};

#if SUGAR_HAS_ZSCORE
        ZScoreMeanReversionSweepRanges zscore_ranges{};
        ExecSettings zscore_exec{};
#endif

#if SUGAR_HAS_SWING
        SwingBreakoutValidationSweepRanges swing_ranges{};
        ExecSettings swing_exec{};
#endif

        // Custom User Strategy (PlanSpec JSON)
        std::string plan_spec_path{};
        bool override_mode{ false };
        TradeMode custom_mode{ TradeMode::LongShort };
        bool override_custom_eval_mode{ false };
        ExecSettings custom_exec{};
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

    void prompt_custom_user_planspec(std::istream& in,
        std::ostream& out,
        StrategyRunOptions& opt);

    bool run_strategy_template(const CandleSeries& series,
        const StrategyRunOptions& opt,
        std::ostream& out);

} // namespace sugar
