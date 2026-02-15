#pragma once
#include "indicator.h"

namespace sugar {

    // ROC over k steps on raw closes
    class ROCIndicator final : public Indicator {
    public:
        explicit ROCIndicator(std::size_t k) : k_(k) {}

        // CandleSeries contract (required): forwards into the SeriesView kernel.
        std::vector<double> compute(const CandleSeries& series) const override;

        // SeriesView-first implementation (single-kernel truth). Supports close-only views.
        std::vector<double> compute(const SeriesView& view) const override;

        std::size_t lookback() const { return k_; }

    private:
        std::size_t k_{};
    };

    // Utility: ROC over an arbitrary vector<double> (exposed for composites)
    // Warm-up: first k entries are NaN; index k is first potentially defined value.
    std::vector<double> roc_over_series(const std::vector<double>& v, std::size_t k);

} // namespace sugar