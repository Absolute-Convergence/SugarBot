#include "indicators_rsi.h"
#include "rsi_kernels.h"

#include <vector>

namespace sugar {

    std::vector<double> RSIIndicator::compute(const CandleSeries& series) const {
        return rsi_over_series(series.closes(), n_);
    }

    std::vector<double> RSIIndicator::compute(const SeriesView& view) const {
        const std::vector<double> close(view.close().begin(), view.close().end());
        return rsi_over_series(close, n_);
    }

} // namespace sugar
