#pragma once

#include "data_view.h"
#include "metrics.h"
#include "strategy_plan.h"

namespace sugar {

    // Universal FSM executor. DataView+ColumnRef is canonical runtime input.
    class StrategyExecutor {
    public:
        BacktestResult run(const DataView& view,
            const ColumnRef& primary,
            const StrategyPlan& plan,
            const ExecSettings& settings) const;
    };

} // namespace sugar
