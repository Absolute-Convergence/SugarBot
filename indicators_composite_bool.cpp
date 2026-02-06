#include "indicators_composite_bool.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>

namespace sugar {

    BoolSeries BoolMap::compute(const CandleSeries& series) const {
        const auto base_vals = base_->compute(series);
        return fn_(base_vals);
    }

    BoolSeries BoolMap::compute(const SeriesView& view) const {
        const auto base_vals = base_->compute(view);
        return fn_(base_vals);
    }

    BoolSeries BoolBinaryMap::compute(const CandleSeries& series) const {
        const auto lhs_vals = lhs_->compute(series);
        const auto rhs_vals = rhs_->compute(series);
        return fn_(lhs_vals, rhs_vals);
    }

    BoolSeries BoolBinaryMap::compute(const SeriesView& view) const {
        const auto lhs_vals = lhs_->compute(view);
        const auto rhs_vals = rhs_->compute(view);
        return fn_(lhs_vals, rhs_vals);
    }

    static inline std::uint8_t eval_cmp(double a, double b, CmpOp op) {
        switch (op) {
        case CmpOp::LT: return (a < b) ? 1u : 0u;
        case CmpOp::LE: return (a <= b) ? 1u : 0u;
        case CmpOp::GT: return (a > b) ? 1u : 0u;
        case CmpOp::GE: return (a >= b) ? 1u : 0u;
        case CmpOp::EQ: return (a == b) ? 1u : 0u;
        case CmpOp::NE: return (a != b) ? 1u : 0u;
        default:        return 0u;
        }
    }

    // Shared comparator implementation (SeriesView-first)
    static inline BoolSeries compute_compare_vec(const std::vector<double>& a,
        const std::vector<double>& b,
        CmpOp op) {
        const std::size_t n = std::max(a.size(), b.size());
        BoolSeries out(n, 0u);

        for (std::size_t i = 0; i < n; ++i) {
            const double x = (i < a.size()) ? a[i] : std::numeric_limits<double>::quiet_NaN();
            const double y = (i < b.size()) ? b[i] : std::numeric_limits<double>::quiet_NaN();

            if (!std::isfinite(x) || !std::isfinite(y)) out[i] = 0u;
            else out[i] = eval_cmp(x, y, op);
        }
        return out;
    }

    BoolSeries CompareSignal::compute(const CandleSeries& series) const {
        // keep CandleSeries compatibility but make SeriesView path the natural evaluation path
        return compute(SeriesView::from_series(series));
    }

    BoolSeries CompareSignal::compute(const SeriesView& view) const {
        const auto a = lhs_->compute(view);
        const auto b = rhs_->compute(view);
        return compute_compare_vec(a, b, op_);
    }

    // Shared cross implementation (SeriesView-first)
    static inline BoolSeries compute_cross_vec(const std::vector<double>& a,
        const std::vector<double>& b,
        CrossDir dir) {
        const std::size_t n = std::max(a.size(), b.size());
        BoolSeries out(n, 0u);
        if (n < 2) return out;

        // Guardrail:
        // Some comparisons are self-referential if RHS is a rolling extreme that includes the current bar
        // (e.g., Donchian upper from highs). In those cases, a[i] > b[i] (or a[i] < b[i]) can often be
        // structurally impossible, producing an all-zero signal and silently "no trades".
        // We can't infer provenance here, but we can warn when the relation is never satisfied.
        std::size_t finite_pairs = 0;
        std::size_t rhs_relation_true = 0; // a1 > b1 for Up; a1 < b1 for Down
        std::size_t rhs_relation_eq = 0;   // a1 == b1 (useful hint for rolling extremes)
        std::size_t crosses = 0;           // how many true cross events fired

        for (std::size_t i = 1; i < n; ++i) {
            const double a0 = (i - 1 < a.size()) ? a[i - 1] : std::numeric_limits<double>::quiet_NaN();
            const double a1 = (i < a.size()) ? a[i] : std::numeric_limits<double>::quiet_NaN();
            const double b0 = (i - 1 < b.size()) ? b[i - 1] : std::numeric_limits<double>::quiet_NaN();
            const double b1 = (i < b.size()) ? b[i] : std::numeric_limits<double>::quiet_NaN();

            if (!std::isfinite(a0) || !std::isfinite(a1) || !std::isfinite(b0) || !std::isfinite(b1)) {
                out[i] = 0u;
                continue;
            }

            ++finite_pairs;
            if (dir == CrossDir::Up) {
                if (a1 > b1) ++rhs_relation_true;
                if (a1 == b1) ++rhs_relation_eq;
            }
            else {
                if (a1 < b1) ++rhs_relation_true;
                if (a1 == b1) ++rhs_relation_eq;
            }

            if (dir == CrossDir::Up) {
                out[i] = (a0 <= b0 && a1 > b1) ? 1u : 0u;
            }
            else {
                out[i] = (a0 >= b0 && a1 < b1) ? 1u : 0u;
            }

            if (out[i]) ++crosses;
        }


        // If the RHS relation is *never* satisfied, it's likely the caller compared against a rolling
        // envelope that includes the current bar (self-referential). This is a common source of
        // "no trades" bugs (e.g., close vs Donchian upper without using prev/shift).
        // We warn once per process to avoid log spam.
        if (finite_pairs >= 50 && rhs_relation_true == 0 && crosses == 0) {
            static bool warned = false;
            if (!warned) {
                warned = true;
                std::cerr
                    << "[SugarBot][debug] cross_" << ((dir == CrossDir::Up) ? "up" : "down")
                    << ": LHS is never " << ((dir == CrossDir::Up) ? ">" : "<")
                    << " RHS on finite bars. \nIf RHS is a rolling extreme/envelope that includes the current bar "
                    "(e.g., Donchian upper/lower), this pattern is often structurally impossible. \n"
                    "Consider aligning RHS to map_shift(RHS, 1).\n";
            }
        }

        return out;
    }

    BoolSeries CrossSignal::compute(const CandleSeries& series) const {
        return compute(SeriesView::from_series(series));
    }

    BoolSeries CrossSignal::compute(const SeriesView& view) const {
        const auto a = lhs_->compute(view);
        const auto b = rhs_->compute(view);
        return compute_cross_vec(a, b, dir_);
    }

} // namespace sugar