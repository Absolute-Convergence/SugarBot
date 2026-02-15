#include "indicators_roc.h"

#include <cmath>
#include <cstddef>
#include <limits>
#include <span>
#include <vector>

namespace sugar {

    namespace {

        inline double qnan() {
            return std::numeric_limits<double>::quiet_NaN();
        }

        // Single-kernel truth: ROC over k steps on a close series.
        // - Output aligned to input (same size).
        // - Warm-up: indices < k are NaN.
        // - Guards:
        //   * prev == 0 => NaN
        //   * non-finite prev or current => NaN
        std::vector<double> roc_kernel(std::span<const double> close, std::size_t k) {
            std::vector<double> out(close.size(), qnan());
            if (k == 0 || close.size() <= k) {
                return out;
            }

            for (std::size_t i = k; i < close.size(); ++i) {
                const double prev = close[i - k];
                const double cur = close[i];

                if (prev == 0.0) {
                    out[i] = qnan();
                    continue;
                }
                if (!std::isfinite(prev) || !std::isfinite(cur)) {
                    out[i] = qnan();
                    continue;
                }

                out[i] = (cur / prev - 1.0) * 100.0;
            }

            return out;
        }

    } // namespace

    std::vector<double> roc_over_series(const std::vector<double>& v, std::size_t k) {
        return roc_kernel(std::span<const double>(v.data(), v.size()), k);
    }

    std::vector<double> ROCIndicator::compute(const CandleSeries& series) const {
        // CandleSeries path forwards into the SeriesView kernel.
        return compute(SeriesView::from_series(series));
    }

    std::vector<double> ROCIndicator::compute(const SeriesView& view) const {
        // ROC only depends on close, so this works for both full OHLCV views and close-only views.
        return roc_kernel(view.close(), k_);
    }

} // namespace sugar