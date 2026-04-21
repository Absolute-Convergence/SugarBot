#include "op_kernels_core.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace sugar::op_kernels {

std::vector<double> core_select(std::span<const std::uint8_t> condition,
    std::span<const double> if_true,
    std::span<const double> if_false) {
    const double qnan = std::numeric_limits<double>::quiet_NaN();
    const std::size_t n = std::max({ condition.size(), if_true.size(), if_false.size() });
    std::vector<double> out(n, qnan);

    for (std::size_t i = 0; i < n; ++i) {
        const bool take_true = (i < condition.size()) && (condition[i] != 0u);
        const double value = take_true
            ? ((i < if_true.size()) ? if_true[i] : qnan)
            : ((i < if_false.size()) ? if_false[i] : qnan);
        if (!std::isfinite(value)) continue;
        out[i] = value;
    }

    return out;
}

} // namespace sugar::op_kernels
