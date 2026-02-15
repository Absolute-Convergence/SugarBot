#include "indicators_sma.h"

#include <limits>
#include <numeric>
#include <cstddef>
#include <span>
#include <vector>
#include <cmath>

namespace sugar {

    namespace {

        inline double qnan() {
            return std::numeric_limits<double>::quiet_NaN();
        }

        // Single-kernel truth: SMA over a close series.
        // - Output is aligned to input (same size).
        // - Warm-up: first (n-1) values are NaN; first defined SMA is at index (n-1).
        std::vector<double> sma_kernel(std::span<const double> close, std::size_t n) {
            std::vector<double> out(close.size(), qnan());
            if (n == 0 || close.size() < n) {
                return out;
            }

            double sum = 0.0;
            std::size_t bad = 0; // count non-finite in current window

            for (std::size_t i = 0; i < n; ++i) {
                const double v = close[i];
                if (!std::isfinite(v)) {
                    ++bad;
                }
                else {
                    sum += v;
                }
            }

            auto emit = [&](std::size_t i) {
                out[i] = (bad == 0) ? (sum / static_cast<double>(n)) : qnan();
                };

            emit(n - 1);

            for (std::size_t i = n; i < close.size(); ++i) {
                const double v_new = close[i];
                const double v_old = close[i - n];

                if (!std::isfinite(v_old)) {
                    --bad;
                }
                else {
                    sum -= v_old;
                }

                if (!std::isfinite(v_new)) {
                    ++bad;
                }
                else {
                    sum += v_new;
                }

                emit(i);
            }

            return out;
        } // namespace
    }

    std::vector<double> SMAIndicator::compute(const CandleSeries& series) const {
        // CandleSeries path forwards into the SeriesView kernel.
        return compute(SeriesView::from_series(series));
    }

    std::vector<double> SMAIndicator::compute(const SeriesView& view) const {
        // SMA only depends on close, so this works for both full OHLCV views and close-only views.
        return sma_kernel(view.close(), n_);
    }

    std::vector<double> sma_over_series(const std::vector<double>& v, std::size_t n) {
        return sma_kernel(std::span<const double>(v.data(), v.size()), n);
    }

} // namespace sugar