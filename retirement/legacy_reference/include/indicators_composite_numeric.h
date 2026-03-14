#pragma once

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <utility>
#include <vector>
#include <deque>

#include "indicator.h"
#include "indicator_graph.h"

// -----------------------------------------------------------------------------
// Optional indicator includes (compile-time gated)
//
// Goal: indicators_composite_numeric.h can remain the "composition hub" without
// forcing *all* indicator headers to exist in every environment.
// Strategies that don't use a missing indicator header should still compile.
//
// Notes:
// - If a mapping helper (map_sma/map_ema/map_roc/etc.) depends on a header that
//   is missing, that helper is NOT defined.
// - This prevents silent runtime fallbacks and keeps failures localized to the
//   strategy that asked for the missing feature.
// -----------------------------------------------------------------------------

#ifndef __has_include
#define __has_include(x) 0
#endif

#if __has_include("indicators_atr.h")
#include "indicators_atr.h"
#endif

#if __has_include("indicators_donchian.h")
#include "indicators_donchian.h"
#endif

#if __has_include("indicators_rsi.h")
#include "indicators_rsi.h"
#endif

#if __has_include("indicators_stddev.h")
#include "indicators_stddev.h"
#endif

#if __has_include("indicators_zscore.h")
#include "indicators_zscore.h"
#endif

#if __has_include("indicators_sma.h")
#include "indicators_sma.h"
#endif

#if __has_include("indicators_ema.h")
#include "indicators_ema.h"
#endif

#if __has_include("indicators_roc.h")
#include "indicators_roc.h"
#endif


namespace sugar {

    // -----------------------------------------------------------------------------
    // Rolling kernels (used by Donchian, rolling bands, etc.)
    // -----------------------------------------------------------------------------

    // Compute rolling max over the last n samples (inclusive), aligned to v.
    // Warmup: indices < (n-1) are NaN.
    inline std::vector<double> rolling_max_over_series(const std::vector<double>& v, std::size_t n) {
        std::vector<double> out(v.size(), std::numeric_limits<double>::quiet_NaN());
        if (n == 0 || v.empty()) return out;

        std::deque<std::size_t> dq;
        dq.clear();

        for (std::size_t i = 0; i < v.size(); ++i) {
            // pop indices out of window
            while (!dq.empty() && dq.front() + n <= i) dq.pop_front();
            // maintain decreasing deque
            while (!dq.empty() && v[dq.back()] <= v[i]) dq.pop_back();
            dq.push_back(i);
            if (i + 1 >= n) {
                out[i] = v[dq.front()];
            }
        }
        return out;
    }

    // Compute rolling min over the last n samples (inclusive), aligned to v.
    // Warmup: indices < (n-1) are NaN.
    inline std::vector<double> rolling_min_over_series(const std::vector<double>& v, std::size_t n) {
        std::vector<double> out(v.size(), std::numeric_limits<double>::quiet_NaN());
        if (n == 0 || v.empty()) return out;

        std::deque<std::size_t> dq;
        dq.clear();

        for (std::size_t i = 0; i < v.size(); ++i) {
            while (!dq.empty() && dq.front() + n <= i) dq.pop_front();
            while (!dq.empty() && v[dq.back()] >= v[i]) dq.pop_back();
            dq.push_back(i);
            if (i + 1 >= n) {
                out[i] = v[dq.front()];
            }
        }
        return out;
    }

    // =============================================================
    // Small shared helpers (NaN policy + size alignment)
    // =============================================================

    inline double qnan() { return std::numeric_limits<double>::quiet_NaN(); }

    // Align output vectors safely when two sources disagree on size.
    // Policy:
    // - Output size = max(x.size(), y.size()).
    // - For indices beyond one side, treat missing value as NaN.
    // - For arithmetic ops, if either side is non-finite at i, output NaN.
    template <typename Op>
    inline std::vector<double> binary_zip_nanpad(const std::vector<double>& x,
        const std::vector<double>& y,
        Op op) {
        const std::size_t n = std::max(x.size(), y.size());
        std::vector<double> out(n, qnan());

        for (std::size_t i = 0; i < n; ++i) {
            const double a = (i < x.size()) ? x[i] : qnan();
            const double b = (i < y.size()) ? y[i] : qnan();

            if (!std::isfinite(a) || !std::isfinite(b)) {
                out[i] = qnan();
            }
            else {
                out[i] = op(a, b);
            }
        }
        return out;
    }

    // ---- Convenience factories (indicator-of-indicator) ----

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

        std::vector<double> compute(const SeriesView& view) const override {
            return inner_->compute(view);
        }

    private:
        IndicatorPtr inner_;
    };

    // =============================================================
    // Leaf nodes
    // =============================================================

    // =============================================================
    // Numeric signal graph helper factories
    // =============================================================

    inline IndicatorPtr map_diff(IndicatorPtr a, IndicatorPtr b) {
        return std::make_shared<BinaryMapIndicator>(
            std::move(a), std::move(b),
            [](const auto& x, const auto& y) {
                return binary_zip_nanpad(x, y, [](double a, double b) { return a - b; });
            }
        );
    }

    inline IndicatorPtr map_sub(IndicatorPtr a, IndicatorPtr b) { return map_diff(std::move(a), std::move(b)); }

    inline IndicatorPtr map_ratio(IndicatorPtr a, IndicatorPtr b) {
        return std::make_shared<BinaryMapIndicator>(
            std::move(a), std::move(b),
            [](const auto& x, const auto& y) {
                const std::size_t n = std::max(x.size(), y.size());
                std::vector<double> out(n, qnan());

                for (std::size_t i = 0; i < n; ++i) {
                    const double num = (i < x.size()) ? x[i] : qnan();
                    const double den = (i < y.size()) ? y[i] : qnan();

                    if (!std::isfinite(num) || !std::isfinite(den) || den == 0.0) out[i] = qnan();
                    else out[i] = num / den;
                }
                return out;
            }
        );
    }

    inline IndicatorPtr map_div(IndicatorPtr a, IndicatorPtr b) { return map_ratio(std::move(a), std::move(b)); }

    inline IndicatorPtr map_product(IndicatorPtr a, IndicatorPtr b) {
        return std::make_shared<BinaryMapIndicator>(
            std::move(a), std::move(b),
            [](const auto& x, const auto& y) {
                return binary_zip_nanpad(x, y, [](double a, double b) { return a * b; });
            }
        );
    }

    inline IndicatorPtr map_mul(IndicatorPtr a, IndicatorPtr b) { return map_product(std::move(a), std::move(b)); }

    inline IndicatorPtr map_add(IndicatorPtr a, IndicatorPtr b) {
        return std::make_shared<BinaryMapIndicator>(
            std::move(a), std::move(b),
            [](const auto& x, const auto& y) {
                return binary_zip_nanpad(x, y, [](double a, double b) { return a + b; });
            }
        );
    }

    // Unary numeric transforms ------------------------------------------------

    inline IndicatorPtr map_negate(IndicatorPtr a) {
        return std::make_shared<MapIndicator>(
            std::move(a),
            [](const auto& x) {
                std::vector<double> out(x.size(), qnan());
                for (std::size_t i = 0; i < x.size(); ++i) {
                    out[i] = std::isfinite(x[i]) ? -x[i] : qnan();
                }
                return out;
            }
        );
    }

    inline IndicatorPtr map_abs(IndicatorPtr a) {
        return std::make_shared<MapIndicator>(
            std::move(a),
            [](const auto& x) {
                std::vector<double> out(x.size(), qnan());
                for (std::size_t i = 0; i < x.size(); ++i) {
                    out[i] = std::isfinite(x[i]) ? std::abs(x[i]) : qnan();
                }
                return out;
            }
        );
    }

    // Common scalar ops -------------------------------------------------------

    inline IndicatorPtr map_add_const(IndicatorPtr a, double c) {
        return map_add(std::move(a), constant(c));
    }

    inline IndicatorPtr map_mul_const(IndicatorPtr a, double c) {
        return map_mul(std::move(a), constant(c));
    }

    inline IndicatorPtr map_div_const(IndicatorPtr a, double c) {
        return map_div(std::move(a), constant(c));
    }

    inline IndicatorPtr map_ratio_of_ma(IndicatorPtr ma) {
        auto price = std::make_shared<CloseIndicator>();
        return map_ratio(std::move(price), std::move(ma));
    }

    // NOTE (causality / self-reference):
        // Rolling extremes like rolling_max/min include the current bar by default.
        // If you use them as *boundaries* in an event detector (enter/exit/cross),
        // you typically must compare against the previous-state boundary (e.g. shift
        // the boundary by 1 bar) to avoid "impossible" events such as close[i] > max(..., i).
        // Execution timing (e.g. next-bar fills) does NOT fix a self-referential boundary.
    inline IndicatorPtr map_rolling_max(IndicatorPtr base, std::size_t n) {
        return std::make_shared<MapIndicator>(
            std::move(base),
            [n](auto const& v) { return rolling_max_over_series(v, n); }
        );
    }

    // NOTE (causality / self-reference):
        // Rolling extremes like rolling_max/min include the current bar by default.
        // If you use them as *boundaries* in an event detector (enter/exit/cross),
        // you typically must compare against the previous-state boundary (e.g. shift
        // the boundary by 1 bar) to avoid "impossible" events such as close[i] > max(..., i).
        // Execution timing (e.g. next-bar fills) does NOT fix a self-referential boundary.
    inline IndicatorPtr map_rolling_min(IndicatorPtr base, std::size_t n) {
        return std::make_shared<MapIndicator>(
            std::move(base),
            [n](auto const& v) { return rolling_min_over_series(v, n); }
        );
    }

    // --- Min / Max ------------------------------------------------------------

    inline IndicatorPtr map_min(IndicatorPtr a, IndicatorPtr b) {
        return std::make_shared<BinaryMapIndicator>(
            std::move(a), std::move(b),
            [](const auto& x, const auto& y) {
                return binary_zip_nanpad(x, y, [](double a, double b) { return std::min(a, b); });
            }
        );
    }

    inline IndicatorPtr map_max(IndicatorPtr a, IndicatorPtr b) {
        return std::make_shared<BinaryMapIndicator>(
            std::move(a), std::move(b),
            [](const auto& x, const auto& y) {
                return binary_zip_nanpad(x, y, [](double a, double b) { return std::max(a, b); });
            }
        );
    }

    // --- Clamp ---------------------------------------------------------------

    inline IndicatorPtr map_clamp_const(IndicatorPtr x, double lo, double hi) {
        return map_min(map_max(std::move(x), constant(lo)), constant(hi));
    }

    inline IndicatorPtr map_clamp(IndicatorPtr x, IndicatorPtr lo, IndicatorPtr hi) {
        return map_min(map_max(std::move(x), std::move(lo)), std::move(hi));
    }

    // --- Shift / Lag ----------------------------------------------------------

    inline IndicatorPtr map_shift(IndicatorPtr base, std::size_t k,
        double fill = std::numeric_limits<double>::quiet_NaN()) {
        return std::make_shared<MapIndicator>(
            std::move(base),
            [k, fill](const std::vector<double>& v) {
                if (k == 0) return v; // copy
                std::vector<double> out(v.size(), fill);
                for (std::size_t i = k; i < v.size(); ++i) out[i] = v[i - k];
                return out;
            }
        );
    }

    // --- Delta ---------------------------------------------------------------

    inline IndicatorPtr map_delta(IndicatorPtr base) {
        const double nan = qnan();
        return std::make_shared<MapIndicator>(
            std::move(base),
            [nan](const std::vector<double>& v) {
                std::vector<double> out(v.size(), nan);
                for (std::size_t i = 1; i < v.size(); ++i) {
                    if (!std::isfinite(v[i]) || !std::isfinite(v[i - 1])) out[i] = nan;
                    else out[i] = v[i] - v[i - 1];
                }
                return out;
            }
        );
    }

    // --- Log / Exp ------------------------------------------------------------

    inline IndicatorPtr map_log(IndicatorPtr base) {
        const double nan = qnan();
        return std::make_shared<MapIndicator>(
            std::move(base),
            [nan](const std::vector<double>& v) {
                std::vector<double> out(v.size(), nan);
                for (std::size_t i = 0; i < v.size(); ++i) {
                    if (!std::isfinite(v[i]) || v[i] <= 0.0) out[i] = nan;
                    else out[i] = std::log(v[i]);
                }
                return out;
            }
        );
    }

    inline IndicatorPtr map_exp(IndicatorPtr base) {
        const double nan = qnan();
        return std::make_shared<MapIndicator>(
            std::move(base),
            [nan](const std::vector<double>& v) {
                std::vector<double> out(v.size(), nan);
                for (std::size_t i = 0; i < v.size(); ++i) {
                    if (!std::isfinite(v[i])) { out[i] = nan; continue; }
                    const double e = std::exp(v[i]);
                    out[i] = std::isfinite(e) ? e : nan;
                }
                return out;
            }
        );
    }

    // --- Sqrt / Pow -----------------------------------------------------------

    inline IndicatorPtr map_sqrt(IndicatorPtr base) {
        const double nan = qnan();
        return std::make_shared<MapIndicator>(
            std::move(base),
            [nan](const std::vector<double>& v) {
                std::vector<double> out(v.size(), nan);
                for (std::size_t i = 0; i < v.size(); ++i) {
                    if (!std::isfinite(v[i]) || v[i] < 0.0) out[i] = nan;
                    else out[i] = std::sqrt(v[i]);
                }
                return out;
            }
        );
    }

    inline IndicatorPtr map_pow(IndicatorPtr base, double p) {
        const double nan = qnan();
        return std::make_shared<MapIndicator>(
            std::move(base),
            [nan, p](const std::vector<double>& v) {
                std::vector<double> out(v.size(), nan);
                for (std::size_t i = 0; i < v.size(); ++i) {
                    if (!std::isfinite(v[i])) { out[i] = nan; continue; }
                    const double r = std::pow(v[i], p);
                    out[i] = std::isfinite(r) ? r : nan;
                }
                return out;
            }
        );
    }

    // --- Normalize ------------------------------------------------------------

    inline IndicatorPtr map_normalize_minmax(IndicatorPtr base, std::size_t n) {
        const double nan = qnan();
        return std::make_shared<MapIndicator>(
            std::move(base),
            [n, nan](const std::vector<double>& v) {
                const auto rmin = rolling_min_over_series(v, n);
                const auto rmax = rolling_max_over_series(v, n);
                const std::size_t m = std::max({ v.size(), rmin.size(), rmax.size() });
                std::vector<double> out(m, nan);

                for (std::size_t i = 0; i < m; ++i) {
                    const double x = (i < v.size()) ? v[i] : nan;
                    const double mn = (i < rmin.size()) ? rmin[i] : nan;
                    const double mx = (i < rmax.size()) ? rmax[i] : nan;

                    if (!std::isfinite(x) || !std::isfinite(mn) || !std::isfinite(mx)) {
                        out[i] = nan;
                        continue;
                    }
                    const double denom = mx - mn;
                    if (!std::isfinite(denom)) out[i] = nan;
                    else if (denom == 0.0) out[i] = 0.0;
                    else out[i] = (x - mn) / denom;
                }
                return out;
            }
        );
    }

    inline IndicatorPtr map_normalize_minmax_global(IndicatorPtr base) {
        const double nan = qnan();
        return std::make_shared<MapIndicator>(
            std::move(base),
            [nan](const std::vector<double>& v) {
                double mn = std::numeric_limits<double>::infinity();
                double mx = -std::numeric_limits<double>::infinity();
                for (double x : v) {
                    if (!std::isfinite(x)) continue;
                    mn = std::min(mn, x);
                    mx = std::max(mx, x);
                }
                if (!std::isfinite(mn) || !std::isfinite(mx)) return std::vector<double>(v.size(), nan);
                const double denom = mx - mn;
                if (denom == 0.0 || !std::isfinite(denom)) return std::vector<double>(v.size(), nan);

                std::vector<double> out(v.size(), nan);
                for (std::size_t i = 0; i < v.size(); ++i) {
                    if (!std::isfinite(v[i])) out[i] = nan;
                    else out[i] = (v[i] - mn) / denom;
                }
                return out;
            }
        );
    }

    // Backward-compatible name (older code may still call this).
    inline IndicatorPtr map_percent_from_ma(IndicatorPtr ma) {
        return map_ratio_of_ma(std::move(ma));
    }

} // namespace sugar
