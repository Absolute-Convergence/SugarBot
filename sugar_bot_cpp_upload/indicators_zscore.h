#pragma once
#include "indicator.h"
#include <cstddef>
#include <vector>

namespace sugar {

    // ZScore using SMA mean + rolling stddev.
    // z = (x - SMA(x,n)) / StdDev(x,n)
    // Warm-up: first defined at index (n-1) (unless sd==0).
    class ZScoreSMAIndicator final : public Indicator {
    public:
        explicit ZScoreSMAIndicator(std::size_t period) : n_(period) {}

        // CandleSeries contract (required): forwards into the SeriesView kernel.
        std::vector<double> compute(const CandleSeries& series) const override;

        // SeriesView-first implementation (single-kernel truth). Supports close-only views.
        std::vector<double> compute(const SeriesView& view) const override;

        std::size_t period() const { return n_; }

    private:
        std::size_t n_{};
    };

    // ZScore using EMA mean + EMA-stddev.
    // Warm-up: first defined at index (2n-2) (unless sd==0).
    class ZScoreEMAIndicator final : public Indicator {
    public:
        explicit ZScoreEMAIndicator(std::size_t period) : n_(period) {}

        // CandleSeries contract (required): forwards into the SeriesView kernel.
        std::vector<double> compute(const CandleSeries& series) const override;

        // SeriesView-first implementation (single-kernel truth). Supports close-only views.
        std::vector<double> compute(const SeriesView& view) const override;

        std::size_t period() const { return n_; }

    private:
        std::size_t n_{};
    };

    // Utilities over arbitrary vectors (aligned to v.size()).
    std::vector<double> zscore_sma_over_series(const std::vector<double>& v, std::size_t n);
    std::vector<double> zscore_ema_over_series(const std::vector<double>& v, std::size_t n);

} // namespace sugar
