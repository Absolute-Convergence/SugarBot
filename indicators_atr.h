#pragma once
#include "indicator.h"
#include <cstddef>
#include <vector>

namespace sugar {

    // True Range (TR) aligned to series.
    // TR[0] uses (high-low); TR[i] for i>0 uses max of:
    //   high-low, |high-prevClose|, |low-prevClose|
    std::vector<double> true_range_over_series(const CandleSeries& series);

    // Wilder ATR over a TR vector.
    // Warm-up: indices < (n-1) are NaN; first ATR at (n-1) for the first contiguous run of n finite TR values.
    // NaN policy (composable): if TR contains non-finite values, ATR resets and reseeds after the next run of n finite TR values.
    std::vector<double> atr_over_tr(const std::vector<double>& tr, std::size_t n);

    // Generic "true range" for an arbitrary numeric vector.
    // Defined as absolute change: TR[0] = 0, TR[i] = |x[i] - x[i-1]|.
    // If x[i] or x[i-1] is not finite, TR[i] is NaN.
    std::vector<double> true_range_over_series(const std::vector<double>& x);

    // Convenience: ATR over an arbitrary numeric series (via generic TR then Wilder smoothing).
    std::vector<double> atr_over_series(const std::vector<double>& x, std::size_t n);

    // Leaf indicator: ATR(period).
    // SeriesView-first: if high/low are missing (vector-only mode), falls back to high=low=close (best-effort),
    // which makes TR[i] = |close[i] - close[i-1]| for i>0 and TR[0]=0 when finite.
    class ATRIndicator final : public Indicator {
    public:
        explicit ATRIndicator(std::size_t period) : n_(period) {}

        // CandleSeries contract (required): forwards into the SeriesView kernel.
        std::vector<double> compute(const CandleSeries& series) const override;

        // SeriesView-first implementation (single-kernel truth). Supports close-only views (best-effort).
        std::vector<double> compute(const SeriesView& view) const override;

        std::size_t period() const { return n_; }

    private:
        std::size_t n_{};
    };

} // namespace sugar
