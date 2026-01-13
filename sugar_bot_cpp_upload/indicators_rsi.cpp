#include "indicators_rsi.h"
#include <limits>
#include <cmath>

namespace sugar {

    static inline double qnan() { return std::numeric_limits<double>::quiet_NaN(); }

    std::vector<double> rsi_over_series(const std::vector<double>& v, std::size_t n) {
        std::vector<double> out(v.size(), qnan());
        if (n == 0 || v.size() <= n) return out; // need at least n+1 points for n diffs

        // Seed using first n differences (1..n)
        double gain_sum = 0.0;
        double loss_sum = 0.0;

        for (std::size_t i = 1; i <= n; ++i) {
            const double cur = v[i];
            const double prev = v[i - 1];
            if (!std::isfinite(cur) || !std::isfinite(prev)) return out;

            const double d = cur - prev;
            if (d >= 0.0) gain_sum += d;
            else          loss_sum += -d;
        }

        double avg_gain = gain_sum / static_cast<double>(n);
        double avg_loss = loss_sum / static_cast<double>(n);

        auto rsi_from_avgs = [](double g, double l) {
            if (l == 0.0) return 100.0;          // no losses
            if (g == 0.0) return 0.0;            // no gains
            const double rs = g / l;
            return 100.0 - (100.0 / (1.0 + rs));
            };

        // First defined RSI at index n
        out[n] = rsi_from_avgs(avg_gain, avg_loss);

        // Wilder smoothing forward
        for (std::size_t i = n + 1; i < v.size(); ++i) {
            const double cur = v[i];
            const double prev = v[i - 1];
            if (!std::isfinite(cur) || !std::isfinite(prev)) {
                out[i] = qnan();
                continue;
            }

            const double d = cur - prev;
            const double gain = (d > 0.0) ? d : 0.0;
            const double loss = (d < 0.0) ? -d : 0.0;

            avg_gain = (avg_gain * (static_cast<double>(n) - 1.0) + gain) / static_cast<double>(n);
            avg_loss = (avg_loss * (static_cast<double>(n) - 1.0) + loss) / static_cast<double>(n);

            out[i] = rsi_from_avgs(avg_gain, avg_loss);
        }

        return out;
    }

    std::vector<double> RSIIndicator::compute(const CandleSeries& series) const {
        return rsi_over_series(series.closes(), n_);
    }

} // namespace sugar