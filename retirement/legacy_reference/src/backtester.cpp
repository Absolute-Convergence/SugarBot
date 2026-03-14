#include "backtester.h"
#include "strategy_executor.h"

namespace sugar {

    BacktestResult Backtester::run(const StrategyPlan& plan,
        const CandleSeries& data,
        const ExecSettings& settings) {
        StrategyExecutor exec;
        std::shared_ptr<align::AlignedBundle> bundle;
        ColumnRef primary{};
        if (!make_single_series_dataview_from_candles(data, bundle, primary)) {
            return BacktestResult{};
        }
        const DataView dv(*bundle);
        return exec.run(dv, primary, plan, settings);
    }

} // namespace sugar
