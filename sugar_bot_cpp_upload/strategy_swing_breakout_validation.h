#pragma once

#include <cstddef>
#include <iosfwd>
#include <string>

#include "strategy_plan.h"
#include "series.h"
#include "metrics.h"
#include "sweep.h"

namespace sugar {

    // Faithful port of:
    // "Swing Breakout Strategy with Validation" (Pine v6)
    struct SwingBreakoutValidationOptions {
        // Pivot inputs
        int leftBars{ 2 };
        int rightBars{ 2 };

        // Validation
        bool use10DayStop{ true };
        int daysAbove10Day{ 2 };
        double pctGainThreshold{ 4.0 };
        int daysForGain{ 3 };
        double maxLossPct{ 8.0 };

        // Volume filter
        bool useVolumeFilter{ false };
        int volumeAvgPeriod{ 50 };
        double volumeMultiplier{ 1.0 };

        // Volume climax
        bool useVolumeClimax{ false };
        double volumeClimaxThreshold{ 2.5 };
        int volumeClimaxMAPeriod{ 50 };
        int climaxBlackoutDays{ 60 };
        double minGainForClimax{ 20.0 };

        // Real breakout tracking
        int realBOResetDays{ 10 };

        TradeMode mode{ TradeMode::LongOnly }; // Pine is long-only
        ExecSettings exec{};
    };

    StrategyPlan build_swing_breakout_validation_plan(const SwingBreakoutValidationOptions& opt);

    BacktestResult run_swing_breakout_validation(const CandleSeries& series,
        const SwingBreakoutValidationOptions& opt);

    // --- Sweep (optional but same pattern as ZScore/GDC) ---
    struct SwingBreakoutValidationParams {
        int leftBars{};
        int rightBars{};

        bool use10DayStop{};
        int daysAbove10Day{};
        double pctGainThreshold{};
        int daysForGain{};
        double maxLossPct{};

        bool useVolumeFilter{};
        int volumeAvgPeriod{};
        double volumeMultiplier{};

        bool useVolumeClimax{};
        double volumeClimaxThreshold{};
        int volumeClimaxMAPeriod{};
        int climaxBlackoutDays{};
        double minGainForClimax{};

        int realBOResetDays{};
    };

    std::string swing_breakout_validation_params_to_string(const SwingBreakoutValidationParams& p);

    struct SwingBreakoutValidationSweepRanges {
        // Pivot
        int left_min{ 1 }, left_max{ 4 }, left_step{ 1 };
        int right_min{ 1 }, right_max{ 4 }, right_step{ 1 };

        // Validation
        bool use10DayStop{ true };
        int daysAbove10_min{ 2 }, daysAbove10_max{ 5 }, daysAbove10_step{ 1 };
        double pctGain_min{ 2.0 }, pctGain_max{ 6.0 }, pctGain_step{ 1.0 };
        int daysForGain_min{ 2 }, daysForGain_max{ 4 }, daysForGain_step{ 1 };
        double maxLoss_min{ 6.0 }, maxLoss_max{ 10.0 }, maxLoss_step{ 1.0 };

        // Volume filter
        bool useVolumeFilter{ false };
        int volAvg_min{ 20 }, volAvg_max{ 100 }, volAvg_step{ 10 };
        double volMult_min{ 0.5 }, volMult_max{ 2.0 }, volMult_step{ 0.25 };

        // Climax
        bool useVolumeClimax{ false };
        double climaxThresh_min{ 1.5 }, climaxThresh_max{ 5.0 }, climaxThresh_step{ 0.5 };
        int climaxMA_min{ 20 }, climaxMA_max{ 100 }, climaxMA_step{ 10 };
        int blackout_min{ 10 }, blackout_max{ 120 }, blackout_step{ 10 };
        double minGainClimax_min{ 5.0 }, minGainClimax_max{ 50.0 }, minGainClimax_step{ 5.0 };

        // Real breakout reset
        int realReset_min{ 7 }, realReset_max{ 14 }, realReset_step{ 1 };

        // Search knobs
        std::size_t top_k{ 10 };
        std::size_t coarse_mesh_count{ 0 };
        int fine_radius{ 1 };
        ScorePolicy score_policy{};
    };

    void prompt_swing_breakout_validation_sweep_ranges(std::istream& in,
        std::ostream& out,
        SwingBreakoutValidationSweepRanges& r);

    bool run_swing_breakout_validation_sweep_best(const CandleSeries& series,
        const SwingBreakoutValidationSweepRanges& ranges,
        const ExecSettings& exec,
        std::ostream& out);

} // namespace sugar
