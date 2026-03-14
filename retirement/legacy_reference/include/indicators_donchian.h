#pragma once
#include "indicator.h"
#include <cstddef>
#include <vector>

namespace sugar {

    // Donchian Channel:
    // Upper[i] = max(high over last n bars ending at i)
    // Lower[i] = min(low  over last n bars ending at i)
    // Mid[i]   = (Upper[i] + Lower[i]) / 2
    // Width[i] = Upper[i] - Lower[i]
    //
    // Warm-up: indices < (n-1) are NaN; first defined at index (n-1).
    // Guard: if n==0 or series.size() < n => all NaN.
    enum class DonchianOutput {
        Upper,
        Lower,
        Middle,
        Width
    };

    // Utility kernels
    std::vector<double> donchian_upper_over_series(const CandleSeries& series, std::size_t n);
    std::vector<double> donchian_lower_over_series(const CandleSeries& series, std::size_t n);
    std::vector<double> donchian_middle_over_series(const CandleSeries& series, std::size_t n);
    std::vector<double> donchian_width_over_series(const CandleSeries& series, std::size_t n);

    // ---------------------------------------------------------------------
    // Generic rolling extrema (vector-generic)
    //
    // These kernels enable "Donchian-like" functionality on any numeric
    // signal, not only candle-derived highs/lows.
    //
    // Warm-up: indices < (n-1) are NaN; first defined at index (n-1).
    // Guard: if n==0 or v.size() < n => all NaN.
    // NaN policy: if v contains non-finite values, output is NaN at that index
    // and internal state is reset; outputs resume after observing n consecutive
    // finite values.
    inline std::vector<double> rolling_max_over_series(const std::vector<double>& v, std::size_t n);
    inline std::vector<double> rolling_min_over_series(const std::vector<double>& v, std::size_t n);

    // Leaf indicator: Donchian(period, output) computed from candles (high/low).
    class DonchianIndicator final : public Indicator {
    public:
        DonchianIndicator(std::size_t period, DonchianOutput out)
            : n_(period), out_(out) {
        }

        std::vector<double> compute(const SeriesView& view) const override;
        std::vector<double> compute(const CandleSeries& series) const override;

        std::size_t period() const { return n_; }
        DonchianOutput output() const { return out_; }

    private:
        std::size_t n_{};
        DonchianOutput out_{ DonchianOutput::Upper };
    };

} // namespace sugar
