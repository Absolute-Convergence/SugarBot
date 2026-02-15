#include "indicators_atr.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>
#include <vector>

namespace sugar {

    namespace {

        inline double qnan() { return std::numeric_limits<double>::quiet_NaN(); }

        // Single-kernel truth: True Range over spans.
        // - Output aligned to input size.
        // - Best-effort: if any required inputs for an index are non-finite, TR[i] is NaN.
        // - Requires close span to be sized correctly; high/low may be the same span (fallback to close).
        std::vector<double> tr_kernel(std::span<const double> high,
            std::span<const double> low,
            std::span<const double> close) {
            std::vector<double> tr(close.size(), qnan());
            if (close.empty()) return tr;

            // i = 0: no prev close, use high-low (fallback works: close-close = 0)
            {
                const double h0 = high[0];
                const double l0 = low[0];
                if (std::isfinite(h0) && std::isfinite(l0)) {
                    tr[0] = h0 - l0;
                }
                else {
                    tr[0] = qnan();
                }
            }

            for (std::size_t i = 1; i < close.size(); ++i) {
                const double h = high[i];
                const double l = low[i];
                const double pc = close[i - 1];

                if (!std::isfinite(h) || !std::isfinite(l) || !std::isfinite(pc)) {
                    tr[i] = qnan();
                    continue;
                }

                const double hl = h - l;
                const double hc = std::fabs(h - pc);
                const double lc = std::fabs(l - pc);

                tr[i] = std::max({ hl, hc, lc });
            }

            return tr;
        }

        // Single-kernel truth: Wilder ATR over TR with best-effort reset + reseed.
        // - Output aligned to tr.size().
        // - Warm-up: first ATR value appears at the end of the first contiguous run of n finite TR values.
        // - If TR becomes non-finite later, ATR resets and reseeds after the next run of n finite TR values.
        std::vector<double> atr_kernel(std::span<const double> tr, std::size_t n) {
            std::vector<double> out(tr.size(), qnan());
            if (n == 0 || tr.size() < n) return out;

            std::size_t run = 0;
            double seed_sum = 0.0;
            bool seeded = false;
            double atr = qnan();

            for (std::size_t i = 0; i < tr.size(); ++i) {
                const double v = tr[i];

                if (!std::isfinite(v)) {
                    // reset state
                    run = 0;
                    seed_sum = 0.0;
                    seeded = false;
                    atr = qnan();
                    continue;
                }

                if (!seeded) {
                    seed_sum += v;
                    ++run;

                    if (run == n) {
                        atr = seed_sum / static_cast<double>(n);
                        out[i] = atr;
                        seeded = true;
                    }
                }
                else {
                    atr = (atr * (static_cast<double>(n) - 1.0) + v) / static_cast<double>(n);
                    out[i] = atr;
                }
            }

            return out;
        }

    } // namespace

    std::vector<double> true_range_over_series(const CandleSeries& series) {
        // Preserve public API but route through the same SeriesView-first kernel.
        const auto v = SeriesView::from_series(series);
        return tr_kernel(v.high(), v.low(), v.close());
    }

    std::vector<double> atr_over_tr(const std::vector<double>& tr, std::size_t n) {
        return atr_kernel(std::span<const double>(tr.data(), tr.size()), n);
    }

    std::vector<double> true_range_over_series(const std::vector<double>& x) {
        std::vector<double> tr(x.size(), qnan());
        if (x.empty()) return tr;

        tr[0] = std::isfinite(x[0]) ? 0.0 : qnan();

        for (std::size_t i = 1; i < x.size(); ++i) {
            if (!std::isfinite(x[i]) || !std::isfinite(x[i - 1])) {
                tr[i] = qnan();
            }
            else {
                tr[i] = std::fabs(x[i] - x[i - 1]);
            }
        }
        return tr;
    }

    std::vector<double> atr_over_series(const std::vector<double>& x, std::size_t n) {
        return atr_over_tr(true_range_over_series(x), n);
    }

    std::vector<double> ATRIndicator::compute(const CandleSeries& series) const {
        // CandleSeries path forwards into the SeriesView kernel.
        return compute(SeriesView::from_series(series));
    }

    std::vector<double> ATRIndicator::compute(const SeriesView& view) const {
        // Best-effort projection:
        // - If high/low are missing (vector-only mode), treat high=low=close.
        const auto close = view.close();
        const auto high = view.has_high() ? view.high() : close;
        const auto low = view.has_low() ? view.low() : close;

        const auto tr = tr_kernel(high, low, close);
        return atr_over_tr(tr, n_);
    }

} // namespace sugar
