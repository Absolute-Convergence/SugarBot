#pragma once

#include <iosfwd>

#include "data_view.h"
#include "metrics.h"
#include "plan_spec.h"
#include "strategy_plan.h"

namespace sugar {

    struct PlanSweepRuntimeOptions {
        bool override_mode{ false };
        TradeMode custom_mode{ TradeMode::LongShort };
        bool override_eval_mode{ false };
        BarEvalMode eval_mode{ BarEvalMode::SameBar };
    };

    // DataView-first custom PlanSpec runtime path.
    bool run_custom_plan_spec(
        const DataView& dv,
        const ColumnRef& primary,
        const PlanSpec& spec,
        const PlanSweepRuntimeOptions& opt,
        std::ostream& out,
        BacktestResult* result_out = nullptr);

} // namespace sugar
