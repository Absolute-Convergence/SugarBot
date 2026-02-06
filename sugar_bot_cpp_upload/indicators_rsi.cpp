#include "indicators_rsi.h"
#include <cmath>
#include <cstddef>
#include <limits>
#include <span>
#include <vector>

namespace sugar {

    static inline double qnan() { return std::numeric_limits<double>::quiet_NaN(); }

    namespace {

        // Single-kernel truth: Wilder RSI over a close series.
        // - Output is aligned to input (same size).
        // - Warm-up: indices < n are NaN; first potentially defined RSI at index = n.
        // - NaNs/infs reset the Wilder state; output remains NaN until reseeded.
        std::vector<double> rsi_kernel(std::span<const double> close, std::size_t n) {
            std::vector<double> out(close.size(), qnan());
            if (n == 0 || close.size() <= n) {
                return out; // need at least n+1 points for n diffs
            }

            auto rsi_from_avgs = [](double g, double l) {
                if (l == 0.0) return 100.0; // no losses
                if (g == 0.0) return 0.0;   // no gains
                const double rs = g / l;
                return 100.0 - (100.0 / (1.0 + rs));
                };

            // We seed when we have n consecutive valid diffs.
            std::size_t run = 0;
            double gain_sum = 0.0;
            double loss_sum = 0.0;
            double avg_gain = 0.0;
            double avg_loss = 0.0;
            bool seeded = false;

            for (std::size_t i = 1; i < close.size(); ++i) {
                const double cur = close[i];
                const double prev = close[i - 1];

                if (!std::isfinite(cur) || !std::isfinite(prev)) {
                    // Reset state; allow NaNs to propagate and delay valid outputs.
                    run = 0;
                    gain_sum = 0.0;
                    loss_sum = 0.0;
                    seeded = false;
                    continue;
                }

                const double d = cur - prev;
                const double gain = (d > 0.0) ? d : 0.0;
                const double loss = (d < 0.0) ? -d : 0.0;

                if (!seeded) {
                    // Build seed window of n diffs.
                    gain_sum += gain;
                    loss_sum += loss;
                    ++run;

                    if (run == n) {
                        avg_gain = gain_sum / static_cast<double>(n);
                        avg_loss = loss_sum / static_cast<double>(n);
                        out[i] = rsi_from_avgs(avg_gain, avg_loss);
                        seeded = true;
                    }
                }
                else {
                    // Wilder smoothing forward.
                    avg_gain = (avg_gain * (static_cast<double>(n) - 1.0) + gain) / static_cast<double>(n);
                    avg_loss = (avg_loss * (static_cast<double>(n) - 1.0) + loss) / static_cast<double>(n);
                    out[i] = rsi_from_avgs(avg_gain, avg_loss);
                }
            }

            return out;
        }

    } // namespace

    std::vector<double> rsi_over_series(const std::vector<double>& v, std::size_t n) {
        return rsi_kernel(std::span<const double>(v.data(), v.size()), n);
    }

    std::vector<double> RSIIndicator::compute(const CandleSeries& series) const {
        // CandleSeries path forwards into the SeriesView kernel.
        return compute(SeriesView::from_series(series));
    }

    std::vector<double> RSIIndicator::compute(const SeriesView& view) const {
        // RSI only depends on close, so this works for both full OHLCV views and close-only views.
        return rsi_kernel(view.close(), n_);
    }

} // namespace sugar