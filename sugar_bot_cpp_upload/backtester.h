#pragma once

#include "strategy_plan.h"   // new StrategyPlan + ExecSettings
#include "series.h"
#include "metrics.h"

namespace sugar {

    class Backtester {
    public:

        // New universal-plug path: StrategyPlan + FSM executor.
        BacktestResult run(const StrategyPlan& plan,
                           const CandleSeries& data,
                           const ExecSettings& settings = {});
    };

} // namespace sugar