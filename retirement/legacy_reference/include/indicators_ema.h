#pragma once
#include "indicator.h"

namespace sugar {

    class EMAIndicator final : public Indicator {
    public:
        explicit EMAIndicator(std::size_t period) : n_(period) {}

        // CandleSeries contract (required): forwards into the SeriesView kernel.
        std::vector<double> compute(const CandleSeries& series) const override;

        // SeriesView-first implementation (single-kernel truth). Supports close-only views.
        std::vector<double> compute(const SeriesView& view) const override;

        std::size_t period() const { return n_; }

    private:
        std::size_t n_{};
    };

    // Compute an Exponential Moving Average over an arbitrary vector (aligned to v.size()).
    // Warm-up: first (n-1) entries are NaN; index (n-1) is SMA seed; EMA continues from there.
    std::vector<double> ema_over_series(const std::vector<double>& v, std::size_t n);

} // namespace sugar