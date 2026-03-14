#include "op_kernels_ms.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "op_kernels_math.h"

namespace sugar::op_kernels {

std::vector<double> ms_diff(std::span<const double> a, std::span<const double> b) {
    const double qnan = std::numeric_limits<double>::quiet_NaN();
    const std::size_t n = std::max(a.size(), b.size());
    std::vector<double> out(n, qnan);
    for (std::size_t i = 0; i < n; ++i) {
        const double x = (i < a.size()) ? a[i] : qnan;
        const double y = (i < b.size()) ? b[i] : qnan;
        if (!std::isfinite(x) || !std::isfinite(y)) continue;
        out[i] = x - y;
    }
    return out;
}

std::vector<double> ms_ratio(std::span<const double> a, std::span<const double> b) {
    const double qnan = std::numeric_limits<double>::quiet_NaN();
    const std::size_t n = std::max(a.size(), b.size());
    std::vector<double> out(n, qnan);
    for (std::size_t i = 0; i < n; ++i) {
        const double x = (i < a.size()) ? a[i] : qnan;
        const double y = (i < b.size()) ? b[i] : qnan;
        if (!std::isfinite(x) || !std::isfinite(y) || y == 0.0) continue;
        out[i] = x / y;
    }
    return out;
}

std::vector<double> ms_zscore(std::span<const double> x, std::size_t n) {
    const double qnan = std::numeric_limits<double>::quiet_NaN();
    auto out = op_kernels_math::zscore_sma(x, n);
    if (out.empty()) out.assign(x.size(), qnan);
    return out;
}

} // namespace sugar::op_kernels
