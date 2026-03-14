#pragma once
#include "indicator.h"
#include <cstddef>
#include <vector>

namespace sugar {

    // Wilder RSI over closes.
    // Output is aligned to series length.
    // Warm-up: indices < period are NaN; first defined RSI at index = period.
    class RSIIndicator final : public Indicator {
    public:
        explicit RSIIndicator(std::size_t period) : n_(period) {}

        // CandleSeries contract (required): forwards into the SeriesView kernel.
        std::vector<double> compute(const CandleSeries& series) const override;

        // SeriesView-first implementation (single-kernel truth). Supports close-only views.
        std::vector<double> compute(const SeriesView& view) const override;

        std::size_t period() const { return n_; }

    private:
        std::size_t n_{};
    };

    // Utility kernel: RSI over arbitrary vector<double> (closes or any derived series).
    // Warm-up: indices < period are NaN; first potentially defined RSI at index = period.
    // NaNs/infs in the input reset the Wilder state; output remains NaN until reseeded.
    std::vector<double> rsi_over_series(const std::vector<double>& v, std::size_t n);

} // namespace sugar
