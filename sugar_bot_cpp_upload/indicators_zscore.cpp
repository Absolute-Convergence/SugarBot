#include "indicators_zscore.h"
#include "indicators_ema.h"   
#include <limits>
#include <cmath>

#include "indicators_zscore.h"
#include "indicators_stddev.h"
#include "indicators_zscore.h"
#include <limits>
#include <cmath>

namespace sugar {

    static inline double qnan() { return std::numeric_limits<double>::quiet_NaN(); }

    std::vector<double> zscore_sma_over_series(const std::vector<double>& v, std::size_t n) {
        std::vector<double> out(v.size(), qnan());
        if (n == 0 || v.size() < n) return out;

        const auto mean = sma_over_series(v, n);
        const auto sd = stddev_over_series(v, n);

        for (std::size_t i = 0; i < v.size(); ++i) {
            if (!std::isfinite(mean[i]) || !std::isfinite(sd[i]) || sd[i] == 0.0) {
                out[i] = qnan();
            }
            else {
                out[i] = (v[i] - mean[i]) / sd[i];
            }
        }
        return out;
    }

    std::vector<double> zscore_ema_over_series(const std::vector<double>& v, std::size_t n) {
        std::vector<double> out(v.size(), qnan());
        if (n == 0 || v.size() < n) return out;

        const auto mean = ema_over_series(v, n);
        const auto sd = stddev_ema_over_series(v, n);

        for (std::size_t i = 0; i < v.size(); ++i) {
            if (!std::isfinite(mean[i]) || !std::isfinite(sd[i]) || sd[i] == 0.0) {
                out[i] = qnan();
            }
            else {
                out[i] = (v[i] - mean[i]) / sd[i];
            }
        }
        return out;
    }

    std::vector<double> ZScoreSMAIndicator::compute(const CandleSeries& series) const {
        const auto v = series.closes(); // closes() returns by value (allocates)
        return zscore_sma_over_series(v, n_);
    }

    std::vector<double> ZScoreEMAIndicator::compute(const CandleSeries& series) const {
        const auto v = series.closes(); // closes() returns by value (allocates)
        return zscore_ema_over_series(v, n_);
    }

} // namespace sugar