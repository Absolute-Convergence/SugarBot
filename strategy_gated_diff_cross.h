#pragma once

/*
===============================================================================
 GATED DIFF CROSS STRATEGY — REFERENCE TEMPLATE

 This file intentionally serves TWO purposes:

   1) A concrete, working strategy (Gated Diff Cross)
   2) A *reference model* for future strategy templates

 The structure and ordering here is intentional. Future strategies should
 follow the same overall shape so signatures stay consistent and predictable.

 HIGH-LEVEL SECTIONS:
   1) Options (single-run configuration)
   2) Plan builder (strategy identity)
   3) Runner (thin execution wrapper)
   4) Sweep support types (Params + Ranges)
   5) Sweep orchestration (Top-K + reporting)

 NOTE:
   - Trading semantics belong in the PLAN BUILDER
   - Search semantics belong in the SWEEP section
   - Execution mechanics belong in StrategyExecutor
===============================================================================
*/

#include <cstdint>
#include <cstddef>
#include <string>
#include <iosfwd>

#include "strategy_plan.h"
#include "series.h"
#include "sweep.h"
#include "metrics.h"


// ------------------------------------------------------------
// Gated Diff Cross
//
// Core:
//   diff = MA_fast - MA_slow
//   cross(diff, 0)
//
// Gates:
//   1) abs(diff) >= diff_min
//   2) ATR / Close >= atr_frac_min
// ------------------------------------------------------------

namespace sugar {

    // ============================================================================
    // 1) OPTIONS — Single-run configuration
    //
    // These parameters define ONE execution of the strategy.
    // If it affects *behavior*, it belongs here.
    // ============================================================================
    struct GatedDiffCrossOptions {
        std::size_t fast_n{ 20 };
        std::size_t slow_n{ 30 };
        double diff_min{ 0.10 };

        std::size_t atr_n{ 5 };
        double atr_frac_min{ 0.01 };

        bool use_ema{ true };

        TradeMode mode{ TradeMode::LongShort };
        ExecSettings exec{};
    };


    // ============================================================================
    // 2) PLAN BUILDER — Strategy identity
    //
    // This function defines WHAT the strategy *is*:
    //   - indicators
    //   - signals
    //   - entry/exit logic
    //   - gating logic
    //
    // If changing a line here changes *how trades happen*, it belongs here.
    StrategyPlan build_gated_diff_cross_plan(const GatedDiffCrossOptions& opt);


    // ============================================================================
    // 3) RUNNER — Thin execution wrapper
    //
    // This function is intentionally boring:
    //   - builds the plan
    //   - executes it
    //   - returns the BacktestResult
    //
    // NO strategy logic should live here.
    // ============================================================================
    BacktestResult run_gated_diff_cross(const CandleSeries& series,
        const GatedDiffCrossOptions& opt);


    // ============================================================================
    // 4) SWEEP SUPPORT — Parameterization
    //
    // These types exist ONLY to support search / optimization.
    // They must not leak trading semantics.
    // ============================================================================

    // Represents ONE candidate parameter set during a sweep
    struct GatedDiffCrossParams {
        std::size_t fast_n;
        std::size_t slow_n;
        double diff_min;
        std::size_t atr_n;
        double atr_frac_min;
        TradeMode mode;
        bool use_ema;
    };

    // Small helper for UX
    std::string gated_diff_cross_params_to_string(const GatedDiffCrossParams& p);

    // Defines the SEARCH SPACE over parameters
    struct GatedDiffCrossSweepRanges {

        // SMA/EMA selection
        bool use_ema{ true };

        // Sweep dimensions
        std::size_t fast_min{ 30 }, fast_max{ 40 }, fast_step{ 1 };
        std::size_t slow_min{ 40 }, slow_max{ 50 }, slow_step{ 1 };

        double diff_min_min{ 0.00 }, diff_min_max{ 0.50 }, diff_min_step{ 0.05 };

        std::size_t atr_n_min{ 0 }, atr_n_max{ 10 }, atr_n_step{ 1 };
        double atr_frac_min_min{ 0.00 }, atr_frac_min_max{ 0.05 }, atr_frac_min_step{ 0.01 };

        // Modes included/allowed in sweep
        bool include_long_only{ true };
        bool include_long_short{ true };

        // ------------------------------------------------------------------------
        // Search-policy controls (optimizer knobs, NOT strategy identity)
        //
        // coarse_mesh_count:
        //   - 0 means "legacy stepping behavior"
        //   - otherwise ~N samples per dimension in coarse stage
        //
        // fine_radius:
        //   - controls local neighborhood size in fine stage
        //   - radius=1 => {-1,0,1} (legacy behavior)
        //
        // IMPORTANT:
        //   These influence *search topology*, not trading semantics.
        // ------------------------------------------------------------------------
        std::size_t coarse_mesh_count{ 0 };
        int fine_radius{ 1 };

        std::size_t top_k{ 10 };

        ScorePolicy score_policy{};
    };

    // Prompt user to define sweep ranges interactively
    void prompt_gated_diff_cross_sweep_ranges(std::istream& in,
        std::ostream& out,
        GatedDiffCrossSweepRanges& r);

    // ============================================================================
    // 5) SWEEP ORCHESTRATION — Optimization + reporting
    //
    // This is where:
    //   - parameters are enumerated
    //   - evaluated
    //   - ranked
    //   - deduplicated
    //
    // NOTE:
    //   This section is intentionally verbose and explicit.
    //   It is the *optimizer policy* for this strategy.
    // ============================================================================
    bool run_gated_diff_cross_sweep_best(const CandleSeries& series,
        const GatedDiffCrossSweepRanges& ranges,
        TradeMode run_mode,
        const ExecSettings& exec,
        std::ostream& out);

} // namespace sugar