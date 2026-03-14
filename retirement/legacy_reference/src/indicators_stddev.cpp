#include "indicators_stddev.h"
#include "vector_kernels.h"

#include <span>

namespace sugar {

std::vector<double> stddev_over_series(const std::vector<double>& v, std::size_t n) {
    return vector_kernels::stddev(std::span<const double>(v.data(), v.size()), n);
}

std::vector<double> stddev_ema_over_series(const std::vector<double>& v, std::size_t n) {
    return vector_kernels::stddev_ema(std::span<const double>(v.data(), v.size()), n);
}

std::vector<double> StdDevIndicator::compute(const CandleSeries& series) const {
    return compute(SeriesView::from_series(series));
}

std::vector<double> StdDevIndicator::compute(const SeriesView& view) const {
    return vector_kernels::stddev(view.close(), n_);
}

std::vector<double> StdDevEMAIndicator::compute(const CandleSeries& series) const {
    return compute(SeriesView::from_series(series));
}

std::vector<double> StdDevEMAIndicator::compute(const SeriesView& view) const {
    return vector_kernels::stddev_ema(view.close(), n_);
}

} // namespace sugar
