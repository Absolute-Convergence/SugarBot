#include "indicators_composite.h"

namespace sugar {

    std::vector<double> MapIndicator::compute(const CandleSeries& series) const {
        const auto base_vals = base_->compute(series);
        return fn_(base_vals);
    }

    // BinaryMapIndicator implementation
    std::vector<double>
        BinaryMapIndicator::compute(const CandleSeries& series) const {
        auto lhs_vals = lhs_->compute(series);
        auto rhs_vals = rhs_->compute(series);
        return fn_(lhs_vals, rhs_vals);
    }
} // namespace sugar
