#include "indicator_graph.h"

#include <cmath>
#include <iostream>
#include <limits>

namespace sugar {

    std::vector<double> MapIndicator::compute(const CandleSeries& series) const {
        const auto base_vals = base_->compute(series);
        return fn_(base_vals);
    }

    std::vector<double> MapIndicator::compute(const SeriesView& view) const {
        const auto base_vals = base_->compute(view);
        return fn_(base_vals);
    }

    std::vector<double> BinaryMapIndicator::compute(const CandleSeries& series) const {
        const auto lhs_vals = lhs_->compute(series);
        const auto rhs_vals = rhs_->compute(series);
        return fn_(lhs_vals, rhs_vals);
    }

    std::vector<double> BinaryMapIndicator::compute(const SeriesView& view) const {
        const auto lhs_vals = lhs_->compute(view);
        const auto rhs_vals = rhs_->compute(view);
        return fn_(lhs_vals, rhs_vals);
    }

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
        return compute(SeriesView::from_series(series));
    }

    BoolSeries CompareSignal::compute(const SeriesView& view) const {
        const auto a = lhs_->compute(view);
        const auto b = rhs_->compute(view);
        return compute_compare_vec(a, b, op_);
    }

    static inline BoolSeries compute_cross_vec(const std::vector<double>& a,
        const std::vector<double>& b,
        CrossDir dir) {
        const std::size_t n = std::max(a.size(), b.size());
        BoolSeries out(n, 0u);
        if (n < 2) return out;

        std::size_t finite_pairs = 0;
        std::size_t rhs_relation_true = 0;
        std::size_t crosses = 0;

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
            }
            else {
                if (a1 < b1) ++rhs_relation_true;
            }

            if (dir == CrossDir::Up) {
                out[i] = (a0 <= b0 && a1 > b1) ? 1u : 0u;
            }
            else {
                out[i] = (a0 >= b0 && a1 < b1) ? 1u : 0u;
            }

            if (out[i]) ++crosses;
        }

        if (finite_pairs >= 50 && rhs_relation_true == 0 && crosses == 0) {
            static bool warned = false;
            if (!warned) {
                warned = true;
                std::cerr
                    << "[SugarBot][debug] cross_" << ((dir == CrossDir::Up) ? "up" : "down")
                    << ": LHS is never " << ((dir == CrossDir::Up) ? ">" : "<")
                    << " RHS on finite bars. \nIf RHS is a rolling extreme/envelope that includes the current bar "
                    << "(e.g., Donchian upper/lower), this pattern is often structurally impossible. \n"
                    << "Consider aligning RHS to map_shift(RHS, 1).\n";
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
