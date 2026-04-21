#include "op_kernels_ms.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "ops/kahan_accumulator.h"
#include "ops/op_kernels_math.h"

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

std::vector<double> ms_correlation(std::span<const double> a, std::span<const double> b, std::size_t n) {
    const double qnan = std::numeric_limits<double>::quiet_NaN();
    const std::size_t size = std::max(a.size(), b.size());
    std::vector<double> out(size, qnan);
    if (n == 0 || size < n) return out;

    for (std::size_t i = n - 1; i < size; ++i) {
        numeric::kahan_accumulator sum_a{};
        numeric::kahan_accumulator sum_b{};
        bool finite_window = true;
        for (std::size_t j = i + 1 - n; j <= i; ++j) {
            const double av = (j < a.size()) ? a[j] : qnan;
            const double bv = (j < b.size()) ? b[j] : qnan;
            if (!std::isfinite(av) || !std::isfinite(bv)) {
                finite_window = false;
                break;
            }
            sum_a.add(av);
            sum_b.add(bv);
        }
        if (!finite_window) continue;

        const double mean_a = sum_a.value() / static_cast<double>(n);
        const double mean_b = sum_b.value() / static_cast<double>(n);
        numeric::kahan_accumulator cov{};
        numeric::kahan_accumulator var_a{};
        numeric::kahan_accumulator var_b{};
        for (std::size_t j = i + 1 - n; j <= i; ++j) {
            const double da = ((j < a.size()) ? a[j] : qnan) - mean_a;
            const double db = ((j < b.size()) ? b[j] : qnan) - mean_b;
            cov.add(da * db);
            var_a.add(da * da);
            var_b.add(db * db);
        }

        const double denom = std::sqrt(std::max(0.0, var_a.value()) * std::max(0.0, var_b.value()));
        if (!std::isfinite(denom) || denom == 0.0) continue;
        out[i] = cov.value() / denom;
    }

    return out;
}

} // namespace sugar::op_kernels
