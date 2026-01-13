#pragma once
#include "indicator.h"
#include <cstddef>
#include <vector>

namespace sugar {

    // True Range (TR) aligned to series.
    // TR[0] uses (high-low); TR[i] for i>0 uses max of:
    //   high-low, |high-prevClose|, |low-prevClose|
    std::vector<double> true_range_over_series(const CandleSeries& series);

    // Wilder ATR over a TR vector. Warm-up: indices < (n-1) are NaN; first ATR at (n-1).
    std::vector<double> atr_over_tr(const std::vector<double>& tr, std::size_t n);

    // Leaf indicator: ATR(period) computed from candles (OHLC).
    class ATRIndicator final : public Indicator {
    public:
        explicit ATRIndicator(std::size_t period) : n_(period) {}

        std::vector<double> compute(const CandleSeries& series) const override;

        std::size_t period() const { return n_; }

    private:
        std::size_t n_{};
    };

} // namespace sugar
