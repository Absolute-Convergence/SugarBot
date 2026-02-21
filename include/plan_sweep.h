#pragma once

#include <iosfwd>

#include "plan_spec.h"
#include "series.h"
#include "strategy_plan.h"

namespace sugar {

    struct PlanSweepRuntimeOptions {
        bool override_mode{ false };
        TradeMode custom_mode{ TradeMode::LongShort };
        bool override_eval_mode{ false };
        BarEvalMode eval_mode{ BarEvalMode::SameBar };
    };

    // Runs a custom PlanSpec strategy:
    // - sweep mode when spec.sweep is enabled/populated
    // - single-run otherwise
    bool run_custom_plan_spec(
        const CandleSeries& series,
        const PlanSpec& spec,
        const PlanSweepRuntimeOptions& opt,
        std::ostream& out);

} // namespace sugar
