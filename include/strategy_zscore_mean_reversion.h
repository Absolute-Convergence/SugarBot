#pragma once

#include <cstddef>
#include <iosfwd>
#include <string>

#include "strategy_plan.h"
#include "series.h"
#include "sweep.h"
#include "metrics.h"

namespace sugar {

    // ============================================================================
    // 1) OPTIONS — Single-run configuration
    // ============================================================================
    struct ZScoreMeanReversionOptions {
        std::size_t z_n{ 20 };     // zscore period
        double z_enter{ 2.0 };     // entry threshold (>=0): enter when |z| >= z_enter
        double z_exit{ 0.5 };      // exit threshold  (>=0): exit when |z| <= z_exit-ish (symmetric rules below)

        bool use_ema{ true };      // true => ZScoreEMAIndicator, false => ZScoreSMAIndicator

        TradeMode mode{ TradeMode::LongShort };
        ExecSettings exec{};
    };

    // ============================================================================
    // 2) PLAN BUILDER — Strategy identity
    // ============================================================================
    StrategyPlan build_zscore_mean_reversion_plan(const ZScoreMeanReversionOptions& opt);

    // ============================================================================
    // 3) RUNNER — Thin execution wrapper
    // ============================================================================
    BacktestResult run_zscore_mean_reversion(const CandleSeries& series,
        const ZScoreMeanReversionOptions& opt);

    // ============================================================================
    // 4) SWEEP SUPPORT — Parameterization
    // ============================================================================
    struct ZScoreMeanReversionParams {
        std::size_t z_n{};
        double z_enter{};
        double z_exit{};
        TradeMode mode{};
        bool use_ema{};
    };

    std::string zscore_mean_reversion_params_to_string(const ZScoreMeanReversionParams& p);

    struct ZScoreMeanReversionSweepRanges {
        bool use_ema{ true };

        std::size_t z_n_min{ 10 }, z_n_max{ 60 }, z_n_step{ 2 };
        double z_enter_min{ 1.0 }, z_enter_max{ 4.0 }, z_enter_step{ 0.25 };
        double z_exit_min{ 0.0 }, z_exit_max{ 2.0 }, z_exit_step{ 0.25 };

        bool include_long_only{ true };
        bool include_long_short{ true };

        // Search-policy controls (optimizer knobs, NOT strategy identity)
        std::size_t coarse_mesh_count{ 0 };
        int fine_radius{ 1 };

        std::size_t top_k{ 10 };
        ScorePolicy score_policy{};
    };

    void prompt_zscore_mean_reversion_sweep_ranges(std::istream& in,
        std::ostream& out,
        ZScoreMeanReversionSweepRanges& r);

    // ============================================================================
    // 5) SWEEP ORCHESTRATION — Optimization + reporting
    // ============================================================================
    bool run_zscore_mean_reversion_sweep_best(const CandleSeries& series,
        const ZScoreMeanReversionSweepRanges& ranges,
        TradeMode run_mode,
        const ExecSettings& exec,
        std::ostream& out);

} // namespace sugar
