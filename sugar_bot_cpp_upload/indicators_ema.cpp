#include "indicators_ema.h"

#include <cstddef>
#include <limits>
#include <numeric>
#include <span>
#include <vector>
#include <cmath>

namespace sugar {

    namespace {

        inline double qnan() {
            return std::numeric_limits<double>::quiet_NaN();
        }

        // Single-kernel truth: EMA over a close series.
        // - Output is aligned to input (same size).
        // - Warm-up: first (n-1) entries are NaN; index (n-1) is the SMA seed; EMA continues from there.
        std::vector<double> ema_kernel(std::span<const double> close, std::size_t n) {
            std::vector<double> out(close.size(), qnan());
            if (n == 0 || close.size() < n) {
                return out;
            }

            const double alpha = 2.0 / (static_cast<double>(n) + 1.0);

            // Best-effort: reset + reseed across non-finite gaps.
            std::size_t run = 0;
            long double sum = 0.0L;
            bool seeded = false;

            for (std::size_t i = 0; i < close.size(); ++i) {
                const double x = close[i];

                if (!std::isfinite(x)) {
                    run = 0;
                    sum = 0.0L;
                    seeded = false;
                    continue; // out[i] remains NaN
                }

                if (!seeded) {
                    sum += static_cast<long double>(x);
                    ++run;

                    if (run == n) {
                        out[i] = static_cast<double>(sum / static_cast<long double>(n)); // SMA seed
                        seeded = true;
                    }
                }
                else {
                    out[i] = alpha * x + (1.0 - alpha) * out[i - 1];
                }
            }
            return out;
        }   // namespace
    }

    std::vector<double> EMAIndicator::compute(const CandleSeries& series) const {
        // CandleSeries path forwards into the SeriesView kernel.
        return compute(SeriesView::from_series(series));
    }

    std::vector<double> EMAIndicator::compute(const SeriesView& view) const {
        // EMA only depends on close, so this works for both full OHLCV views and close-only views.
        return ema_kernel(view.close(), n_);
    }

    std::vector<double> ema_over_series(const std::vector<double>& v, std::size_t n) {
        return ema_kernel(std::span<const double>(v.data(), v.size()), n);
    }

} // namespace sugar