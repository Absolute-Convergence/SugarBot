#pragma once

#include <cstddef>
#include <vector>

#include "series.h"

namespace sugar {

    // True Range (TR) aligned to candle series.
    std::vector<double> true_range_over_series(const CandleSeries& series);

    // Wilder ATR over a TR vector.
    std::vector<double> atr_over_tr(const std::vector<double>& tr, std::size_t n);

    // Generic true range over arbitrary numeric vector (absolute delta).
    std::vector<double> true_range_over_series(const std::vector<double>& x);

    // Convenience ATR over arbitrary vector.
    std::vector<double> atr_over_series(const std::vector<double>& x, std::size_t n);

} // namespace sugar
