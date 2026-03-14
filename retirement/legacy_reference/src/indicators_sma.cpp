#include "indicators_sma.h"
#include "vector_kernels.h"

#include <span>

namespace sugar {

    std::vector<double> SMAIndicator::compute(const CandleSeries& series) const {
        // CandleSeries path forwards into the SeriesView kernel.
        return compute(SeriesView::from_series(series));
    }

    std::vector<double> SMAIndicator::compute(const SeriesView& view) const {
        // SMA only depends on close, so this works for both full OHLCV views and close-only views.
        return vector_kernels::sma(view.close(), n_);
    }

    std::vector<double> sma_over_series(const std::vector<double>& v, std::size_t n) {
        return vector_kernels::sma(std::span<const double>(v.data(), v.size()), n);
    }

} // namespace sugar
