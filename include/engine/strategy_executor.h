#pragma once

#include <functional>
#include <string>

#include "ingest/data_view.h"
#include "sweep/metrics.h"
#include "sweep/strategy_plan.h"

namespace sugar {

    using ExecutionCancelCallback = std::function<bool()>;

    // Universal FSM executor. DataView+ColumnRef is canonical runtime input.
    class StrategyExecutor {
    public:
        bool run(
            const DataView& view,
            const ColumnRef& primary,
            const StrategyPlan& plan,
            const ExecSettings& settings,
            BacktestResult& out,
            std::string& err,
            ExecutionCancelCallback cancel_requested = nullptr) const;

        BacktestResult run(const DataView& view,
            const ColumnRef& primary,
            const StrategyPlan& plan,
            const ExecSettings& settings) const;
    };

} // namespace sugar
