#include "indicators_atr.h"
#include <limits>
#include <cmath>
#include <algorithm>

namespace sugar {

    static inline double qnan() { return std::numeric_limits<double>::quiet_NaN(); }

    std::vector<double> true_range_over_series(const CandleSeries& series) {
        std::vector<double> tr(series.size(), qnan());
        if (series.size() == 0) return tr;

        // i = 0: no prev close, use high-low
        {
            const auto& c0 = series[0];
            tr[0] = c0.high - c0.low;
        }

        for (std::size_t i = 1; i < series.size(); ++i) {
            const auto& c = series[i];
            const double prev_close = series[i - 1].close;

            const double hl = c.high - c.low;
            const double hc = std::fabs(c.high - prev_close);
            const double lc = std::fabs(c.low - prev_close);

            tr[i] = std::max({ hl, hc, lc });
        }

        return tr;
    }

    std::vector<double> atr_over_tr(const std::vector<double>& tr, std::size_t n) {
        std::vector<double> out(tr.size(), qnan());
        if (n == 0 || tr.size() < n) return out;

        // Seed: simple average of first n TR values at index n-1
        double seed_sum = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            if (!std::isfinite(tr[i])) return out;
            seed_sum += tr[i];
        }
        double atr = seed_sum / static_cast<double>(n);
        out[n - 1] = atr;

        // Wilder smoothing forward
        for (std::size_t i = n; i < tr.size(); ++i) {
            if (!std::isfinite(tr[i]) || !std::isfinite(atr)) {
                out[i] = qnan();
                continue;
            }
            atr = (atr * (static_cast<double>(n) - 1.0) + tr[i]) / static_cast<double>(n);
            out[i] = atr;
        }

        return out;
    }

    std::vector<double> ATRIndicator::compute(const CandleSeries& series) const {
        const auto tr = true_range_over_series(series);
        return atr_over_tr(tr, n_);
    }

} // namespace sugar
