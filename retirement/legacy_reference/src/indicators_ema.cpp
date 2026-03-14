#include "indicators_ema.h"
#include "vector_kernels.h"

#include <span>

namespace sugar {

std::vector<double> EMAIndicator::compute(const CandleSeries& series) const {
    return compute(SeriesView::from_series(series));
}

std::vector<double> EMAIndicator::compute(const SeriesView& view) const {
    return vector_kernels::ema(view.close(), n_);
}

std::vector<double> ema_over_series(const std::vector<double>& v, std::size_t n) {
    return vector_kernels::ema(std::span<const double>(v.data(), v.size()), n);
}

} // namespace sugar
