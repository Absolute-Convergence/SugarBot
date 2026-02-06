#include "indicators_composite_numeric.h"

namespace sugar {

    std::vector<double> MapIndicator::compute(const CandleSeries& series) const {
        const auto base_vals = base_->compute(series);
        return fn_(base_vals);
    }

    std::vector<double> MapIndicator::compute(const SeriesView& view) const {
        const auto base_vals = base_->compute(view);
        return fn_(base_vals);
    }

    std::vector<double> BinaryMapIndicator::compute(const CandleSeries& series) const {
        const auto lhs_vals = lhs_->compute(series);
        const auto rhs_vals = rhs_->compute(series);
        return fn_(lhs_vals, rhs_vals);
    }

    std::vector<double> BinaryMapIndicator::compute(const SeriesView& view) const {
        const auto lhs_vals = lhs_->compute(view);
        const auto rhs_vals = rhs_->compute(view);
        return fn_(lhs_vals, rhs_vals);
    }

} // namespace sugar