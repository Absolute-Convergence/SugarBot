#pragma once
#include <tuple>
#include <vector>
#include "series.h"
#include "metrics.h"
#include "strategy_diff_cross.h"   

namespace sugar {

    // fast, slow, roc, thresh
    using RocSmaParams = std::tuple<std::size_t, std::size_t, std::size_t, double>;

    struct SweepResult {
        BacktestResult best;
        RocSmaParams   params;
    };

    // add a mode parameter with default LongOnly
    SweepResult sweep_roc_sma(const CandleSeries& data,
        const std::vector<std::size_t>& fasts,
        const std::vector<std::size_t>& slows,
        const std::vector<std::size_t>& rocs,
        const std::vector<double>& threshes,
        DiffCrossStrategy::Mode mode = DiffCrossStrategy::Mode::LongOnly);

} // namespace sugar
