#include "indicators_roc.h"
#include "vector_kernels.h"

#include <span>

namespace sugar {

std::vector<double> roc_over_series(const std::vector<double>& v, std::size_t k) {
    return vector_kernels::roc(std::span<const double>(v.data(), v.size()), k);
}

std::vector<double> ROCIndicator::compute(const CandleSeries& series) const {
    return compute(SeriesView::from_series(series));
}

std::vector<double> ROCIndicator::compute(const SeriesView& view) const {
    return vector_kernels::roc(view.close(), k_);
}

} // namespace sugar
