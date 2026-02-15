#pragma once

#include "metrics.h"
#include "series.h"
#include "series_view.h"
#include "strategy_plan.h"

namespace sugar {

    // Universal FSM executor that runs a StrategyPlan on a SeriesView.
    class StrategyExecutor {
    public:
        BacktestResult run(const SeriesView& view,
            const StrategyPlan& plan,
            const ExecSettings& settings) const;

        BacktestResult run(const CandleSeries& series,
            const StrategyPlan& plan,
            const ExecSettings& settings) const;
    };

} // namespace sugar