#pragma once

#include <cstddef>
#include <vector>

namespace sugar {

    // Wilder RSI over a numeric vector.
    std::vector<double> rsi_over_series(const std::vector<double>& v, std::size_t n);

} // namespace sugar
