#include "indicators_zscore.h"
#include "vector_kernels.h"

#include <span>

namespace sugar {

std::vector<double> zscore_sma_over_series(const std::vector<double>& v, std::size_t n) {
    return vector_kernels::zscore_sma(std::span<const double>(v.data(), v.size()), n);
}

std::vector<double> zscore_ema_over_series(const std::vector<double>& v, std::size_t n) {
    return vector_kernels::zscore_ema(std::span<const double>(v.data(), v.size()), n);
}

std::vector<double> ZScoreSMAIndicator::compute(const CandleSeries& series) const {
    return compute(SeriesView::from_series(series));
}

std::vector<double> ZScoreSMAIndicator::compute(const SeriesView& view) const {
    return vector_kernels::zscore_sma(view.close(), n_);
}

std::vector<double> ZScoreEMAIndicator::compute(const CandleSeries& series) const {
    return compute(SeriesView::from_series(series));
}

std::vector<double> ZScoreEMAIndicator::compute(const SeriesView& view) const {
    return vector_kernels::zscore_ema(view.close(), n_);
}

} // namespace sugar
