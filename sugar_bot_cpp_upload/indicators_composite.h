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

    // Existing helper factories (unchanged declarations)
    IndicatorPtr map_sma(IndicatorPtr base, std::size_t n);
    IndicatorPtr map_ema(IndicatorPtr base, std::size_t n);
    IndicatorPtr map_roc(IndicatorPtr base, std::size_t n);

    // =============================================================
    // NEW: CloseIndicator (primitive leaf node)
    // =============================================================

    class CloseIndicator final : public Indicator {
    public:
        CloseIndicator() = default;

        std::vector<double> compute(const CandleSeries& series) const override {
            const auto& c = series.closes();
            return std::vector<double>(c.begin(), c.end());
        }
    };

    // =============================================================
    // NEW: BinaryMapIndicator (A,B → f(A,B))
    // =============================================================

    class BinaryMapIndicator final : public Indicator {
    public:
        using Fn2 = std::function<
            std::vector<double>(
                const std::vector<double>&,
                const std::vector<double>&
            )
        >;

        BinaryMapIndicator(IndicatorPtr lhs, IndicatorPtr rhs, Fn2 fn)
            : lhs_(std::move(lhs)), rhs_(std::move(rhs)), fn_(std::move(fn)) {
        }

        std::vector<double> compute(const CandleSeries& series) const override;

    private:
        IndicatorPtr lhs_;
        IndicatorPtr rhs_;
        Fn2 fn_;
    };

    // =============================================================
    // NEW: Helper factories (pure additions)
    // =============================================================

    inline IndicatorPtr map_diff(IndicatorPtr a, IndicatorPtr b) {
        return std::make_shared<BinaryMapIndicator>(
            std::move(a), std::move(b),
            [](const auto& x, const auto& y) {
                const std::size_t n = std::min(x.size(), y.size());
                std::vector<double> out(n);
                for (std::size_t i = 0; i < n; ++i)
                    out[i] = x[i] - y[i];
                return out;
            }
        );
    }

    inline IndicatorPtr map_ratio(IndicatorPtr a, IndicatorPtr b) {
        return std::make_shared<BinaryMapIndicator>(
            std::move(a), std::move(b),
            [](const auto& x, const auto& y) {
                const std::size_t n = std::min(x.size(), y.size());
                const double nan = std::numeric_limits<double>::quiet_NaN();
                std::vector<double> out(n);
                for (std::size_t i = 0; i < n; ++i) {
                    out[i] = (!std::isfinite(y[i]) || y[i] == 0.0)
                        ? nan
                        : x[i] / y[i];
                }
                return out;
            }
        );
    }

    inline IndicatorPtr map_product(IndicatorPtr a, IndicatorPtr b) {
        return std::make_shared<BinaryMapIndicator>(
            std::move(a), std::move(b),
            [](const auto& x, const auto& y) {
                const std::size_t n = std::min(x.size(), y.size());
                std::vector<double> out(n);
                for (std::size_t i = 0; i < n; ++i)
                    out[i] = x[i] * y[i];
                return out;
            }
        );
    }

    // Percent-from-MA convenience
    inline IndicatorPtr map_percent_from_ma(IndicatorPtr ma) {
        auto price = std::make_shared<CloseIndicator>();
        return map_ratio(std::move(price), std::move(ma));
    }

} // namespace sugar
