#include "backtester.h"
#include "strategy_executor.h"

namespace sugar {

    BacktestResult Backtester::run(const StrategyPlan& plan,
        const CandleSeries& data,
        const ExecSettings& settings) {
        StrategyExecutor exec;
        return exec.run(SeriesView::from_series(data), plan, settings);
    }

} // namespace sugar