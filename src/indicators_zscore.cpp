#include "indicators_zscore.h"
#include "indicators_ema.h"
#include "indicators_sma.h"
#include "indicators_stddev.h"

#include <cmath>
#include <limits>
#include <span>
#include <vector>

namespace sugar {

    namespace {

        inline double qnan() { return std::numeric_limits<double>::quiet_NaN(); }

        // Shared helper for SeriesView-backed evaluation.
        // NOTE: We intentionally convert span->vector so we can reuse the existing over_series APIs.
        // This keeps ZScore single-truth via SeriesView while staying mechanically consistent with
        // the rest of the bridge refactor.
        std::vector<double> span_to_vector(std::span<const double> s) {
            return std::vector<double>(s.begin(), s.end());
        }

    } // namespace

    std::vector<double> zscore_sma_over_series(const std::vector<double>& v, std::size_t n) {
        std::vector<double> out(v.size(), qnan());
        if (n == 0 || v.size() < n) {
            return out;
        }

        const auto mean = sma_over_series(v, n);
        const auto sd = stddev_over_series(v, n);

        for (std::size_t i = 0; i < v.size(); ++i) {
            if (!std::isfinite(v[i]) || !std::isfinite(mean[i]) || !std::isfinite(sd[i]) || sd[i] == 0.0) {
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
        if (n == 0 || v.size() < n) {
            return out;
        }

        const auto mean = ema_over_series(v, n);
        const auto sd = stddev_ema_over_series(v, n);

        for (std::size_t i = 0; i < v.size(); ++i) {
            if (!std::isfinite(v[i]) || !std::isfinite(mean[i]) || !std::isfinite(sd[i]) || sd[i] == 0.0) {
                out[i] = qnan();
            }
            else {
                out[i] = (v[i] - mean[i]) / sd[i];
            }
        }
        return out;
    }

    std::vector<double> ZScoreSMAIndicator::compute(const CandleSeries& series) const {
        // CandleSeries path forwards into the SeriesView kernel.
        return compute(SeriesView::from_series(series));
    }

    std::vector<double> ZScoreSMAIndicator::compute(const SeriesView& view) const {
        // ZScore depends on close only, so this supports both full OHLCV and close-only views.
        const auto v = span_to_vector(view.close());
        return zscore_sma_over_series(v, n_);
    }

    std::vector<double> ZScoreEMAIndicator::compute(const CandleSeries& series) const {
        // CandleSeries path forwards into the SeriesView kernel.
        return compute(SeriesView::from_series(series));
    }

    std::vector<double> ZScoreEMAIndicator::compute(const SeriesView& view) const {
        const auto v = span_to_vector(view.close());
        return zscore_ema_over_series(v, n_);
    }

} // namespace sugar