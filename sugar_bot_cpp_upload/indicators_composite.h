#pragma once
#include <functional>
#include <utility>
#include <vector>
#include "indicator.h"
#include "indicators_roc.h"
#include "indicators_sma.h"
#include "indicators_ema.h"

namespace sugar {

    // Generic adapter that applies a vector->vector transform to the output of a base indicator
    // "composition layer"

    class MapIndicator final : public Indicator {
    public:
        using Fn = std::function<std::vector<double>(const std::vector<double>&)>;

        MapIndicator(IndicatorPtr base, Fn fn)
            : base_(std::move(base)), fn_(std::move(fn)) {
        }

        std::vector<double> compute(const CandleSeries& series) const override;

    private:
        IndicatorPtr base_;
        Fn fn_;
    };

    // ---- Convenience factories  ----
    inline IndicatorPtr map_roc(IndicatorPtr base, std::size_t k) {
        return std::make_shared<MapIndicator>(
            std::move(base),
            [k](auto const& v) { return roc_over_series(v, k); }
        );
    }

    inline IndicatorPtr map_sma(IndicatorPtr base, std::size_t n) {
        return std::make_shared<MapIndicator>(
            std::move(base),
            [n](auto const& v) { return sma_over_series(v, n); }
        );
    }

    inline IndicatorPtr map_ema(IndicatorPtr base, std::size_t n) {
        return std::make_shared<MapIndicator>(
            std::move(base),
            [n](auto const& v) { return ema_over_series(v, n); }
        );
    }

    // ---- Backward-compatible wrapper ------------------------------------

    class ROCOfIndicator final : public Indicator {
    public:
        ROCOfIndicator(IndicatorPtr base, std::size_t k)
            : inner_(map_roc(std::move(base), k)) {
        }

        std::vector<double> compute(const CandleSeries& series) const override {
            return inner_->compute(series);
        }

    private:
        IndicatorPtr inner_;
    };

} // namespace sugar
