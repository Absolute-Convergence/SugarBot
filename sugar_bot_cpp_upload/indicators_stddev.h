#pragma once
#include "indicator.h"
#include "indicators_ema.h"
#include "indicators_sma.h"
#include <cstddef>
#include <vector>

namespace sugar {

    // Rolling/windowed stddev over the last n values (population variance, divide by n).
    // Warm-up: first (n-1) entries are NaN; first defined at index (n-1).
    class StdDevIndicator final : public Indicator {
    public:
        explicit StdDevIndicator(std::size_t period) : n_(period) {}

        // CandleSeries contract (required): forwards into the SeriesView kernel.
        std::vector<double> compute(const CandleSeries& series) const override;

        // SeriesView-first implementation (single-kernel truth). Supports close-only views.
        std::vector<double> compute(const SeriesView& view) const override;

        std::size_t period() const { return n_; }
    private:
        std::size_t n_{};
    };

    // EMA-based stddev using the project's ema_over_series() for both mean and variance:
    //   mean = EMA(x, n)                      (seed at index n-1)
    //   var  = EMA((x-mean)^2, n)             (seed needs n defined dev2 samples)
    //   sd   = sqrt(var)
    //
    // Because mean is first defined at (n-1) and var EMA needs n defined dev2 samples,
    // the first defined sd is at index (2n-2). Before that, sd is NaN.
    class StdDevEMAIndicator final : public Indicator {
    public:
        explicit StdDevEMAIndicator(std::size_t period) : n_(period) {}

        // CandleSeries contract (required): forwards into the SeriesView kernel.
        std::vector<double> compute(const CandleSeries& series) const override;

        // SeriesView-first implementation (single-kernel truth). Supports close-only views.
        std::vector<double> compute(const SeriesView& view) const override;

        std::size_t period() const { return n_; }
    private:
        std::size_t n_{};
    };

    // Utilities operating on arbitrary vectors (aligned to v.size()).
    std::vector<double> stddev_over_series(const std::vector<double>& v, std::size_t n);
    std::vector<double> stddev_ema_over_series(const std::vector<double>& v, std::size_t n);

} // namespace sugar
