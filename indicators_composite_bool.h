#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "indicators_composite_numeric.h" // IndicatorPtr + constant(...) + SeriesView

namespace sugar {

    // =============================================================
    // Boolean Signal Graph Layer
    //
    // Mirrors indicators_composite (numeric) but produces a boolean series (0/1).
    //
    // Conventions (bridge-crossed, composable):
    // - SeriesView-first: compute(view) is supported everywhere.
    // - CandleSeries remains supported and forwards where appropriate.
    // - Size alignment: outputs are aligned; binary ops pad rather than truncate.
    // - NaN semantics: comparisons/crosses treat non-finite numeric inputs as false.
    // =============================================================

    using BoolSeries = std::vector<std::uint8_t>; // 0=false, 1=true

    inline std::uint8_t b0() { return 0u; }
    inline std::uint8_t b1() { return 1u; }

    template <typename Op>
    inline BoolSeries bool_zip_pad(const BoolSeries& x, const BoolSeries& y, Op op) {
        const std::size_t n = std::max(x.size(), y.size());
        BoolSeries out(n, b0());
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint8_t a = (i < x.size()) ? x[i] : b0();
            const std::uint8_t b = (i < y.size()) ? y[i] : b0();
            out[i] = op(a, b);
        }
        return out;
    }

    class BoolSignal {
    public:
        virtual ~BoolSignal() = default;

        // CandleSeries contract (legacy/compat)
        virtual BoolSeries compute(const CandleSeries& series) const = 0;

        // SeriesView-first (required for vector-only composition)
        virtual BoolSeries compute(const SeriesView& view) const = 0;
    };

    using BoolSignalPtr = std::shared_ptr<BoolSignal>;

    // -------------------------------------------------------------
    // Unary boolean map: B -> f(B)
    // -------------------------------------------------------------
    class BoolMap final : public BoolSignal {
    public:
        using Fn = std::function<BoolSeries(const BoolSeries&)>;

        BoolMap(BoolSignalPtr base, Fn fn)
            : base_(std::move(base)), fn_(std::move(fn)) {
        }

        BoolSeries compute(const CandleSeries& series) const override;
        BoolSeries compute(const SeriesView& view) const override;

    private:
        BoolSignalPtr base_;
        Fn fn_;
    };

    // -------------------------------------------------------------
    // Binary boolean map: (A,B) -> f(A,B)
    // -------------------------------------------------------------
    class BoolBinaryMap final : public BoolSignal {
    public:
        using Fn2 = std::function<BoolSeries(const BoolSeries&, const BoolSeries&)>;

        BoolBinaryMap(BoolSignalPtr lhs, BoolSignalPtr rhs, Fn2 fn)
            : lhs_(std::move(lhs)), rhs_(std::move(rhs)), fn_(std::move(fn)) {
        }

        BoolSeries compute(const CandleSeries& series) const override;
        BoolSeries compute(const SeriesView& view) const override;

    private:
        BoolSignalPtr lhs_;
        BoolSignalPtr rhs_;
        Fn2 fn_;
    };

    // -------------------------------------------------------------
    // Comparator node: numeric (A,B) -> bool
    // -------------------------------------------------------------
    enum class CmpOp { LT, LE, GT, GE, EQ, NE };

    class CompareSignal final : public BoolSignal {
    public:
        CompareSignal(IndicatorPtr lhs, IndicatorPtr rhs, CmpOp op)
            : lhs_(std::move(lhs)), rhs_(std::move(rhs)), op_(op) {
        }

        BoolSeries compute(const CandleSeries& series) const override;
        BoolSeries compute(const SeriesView& view) const override;

    private:
        IndicatorPtr lhs_;
        IndicatorPtr rhs_;
        CmpOp op_{};
    };

    // -------------------------------------------------------------
    // Cross nodes: numeric (A,B) -> bool
    // -------------------------------------------------------------
    enum class CrossDir { Up, Down };

    class CrossSignal final : public BoolSignal {
    public:
        CrossSignal(IndicatorPtr lhs, IndicatorPtr rhs, CrossDir dir)
            : lhs_(std::move(lhs)), rhs_(std::move(rhs)), dir_(dir) {
        }

        BoolSeries compute(const CandleSeries& series) const override;
        BoolSeries compute(const SeriesView& view) const override;

    private:
        IndicatorPtr lhs_;
        IndicatorPtr rhs_;
        CrossDir dir_{};
    };

    // -------------------------------------------------------------
    // Boolean leaves
    // -------------------------------------------------------------
    class ConstBoolSignal final : public BoolSignal {
    public:
        explicit ConstBoolSignal(bool value) : value_(value ? 1u : 0u) {}

        BoolSeries compute(const CandleSeries& series) const override {
            return BoolSeries(series.size(), value_);
        }

        BoolSeries compute(const SeriesView& view) const override {
            return BoolSeries(view.size(), value_);
        }

    private:
        std::uint8_t value_{};
    };

    inline BoolSignalPtr bconst(bool v) {
        return std::make_shared<ConstBoolSignal>(v);
    }

    // =============================================================
    // Factory helpers: logic ops (NOT, AND, OR, NAND, NOR)
    // =============================================================

    // NOT
    inline BoolSignalPtr bnot(BoolSignalPtr a) {
        return std::make_shared<BoolMap>(
            std::move(a),
            [](const BoolSeries& x) {
                BoolSeries out(x.size(), b0());
                for (std::size_t i = 0; i < x.size(); ++i) out[i] = x[i] ? b0() : b1();
                return out;
            }
        );
    }

    // AND
    inline BoolSignalPtr band(BoolSignalPtr a, BoolSignalPtr b) {
        return std::make_shared<BoolBinaryMap>(
            std::move(a), std::move(b),
            [](const BoolSeries& x, const BoolSeries& y) {
                return bool_zip_pad(x, y, [](std::uint8_t a, std::uint8_t b) { return (a && b) ? b1() : b0(); });
            }
        );
    }

    // OR
    inline BoolSignalPtr bor(BoolSignalPtr a, BoolSignalPtr b) {
        return std::make_shared<BoolBinaryMap>(
            std::move(a), std::move(b),
            [](const BoolSeries& x, const BoolSeries& y) {
                return bool_zip_pad(x, y, [](std::uint8_t a, std::uint8_t b) { return (a || b) ? b1() : b0(); });
            }
        );
    }

    // NAND
    inline BoolSignalPtr bnand(BoolSignalPtr a, BoolSignalPtr b) {
        return bnot(band(std::move(a), std::move(b)));
    }

    // NOR
    inline BoolSignalPtr bnor(BoolSignalPtr a, BoolSignalPtr b) {
        return bnot(bor(std::move(a), std::move(b)));
    }

    // =============================================================
    // Factory helpers: comparators
    // =============================================================
    inline BoolSignalPtr blt(IndicatorPtr a, IndicatorPtr b) { return std::make_shared<CompareSignal>(std::move(a), std::move(b), CmpOp::LT); } // less than
    inline BoolSignalPtr ble(IndicatorPtr a, IndicatorPtr b) { return std::make_shared<CompareSignal>(std::move(a), std::move(b), CmpOp::LE); } // less or equal
    inline BoolSignalPtr bgt(IndicatorPtr a, IndicatorPtr b) { return std::make_shared<CompareSignal>(std::move(a), std::move(b), CmpOp::GT); } // greater
    inline BoolSignalPtr bge(IndicatorPtr a, IndicatorPtr b) { return std::make_shared<CompareSignal>(std::move(a), std::move(b), CmpOp::GE); } // greater or equal
    inline BoolSignalPtr beq(IndicatorPtr a, IndicatorPtr b) { return std::make_shared<CompareSignal>(std::move(a), std::move(b), CmpOp::EQ); } // equal
    inline BoolSignalPtr bne(IndicatorPtr a, IndicatorPtr b) { return std::make_shared<CompareSignal>(std::move(a), std::move(b), CmpOp::NE); } // not equal

    // Comparator-to-constant convenience
    inline BoolSignalPtr blt(IndicatorPtr a, double c) { return blt(std::move(a), constant(c)); }
    inline BoolSignalPtr ble(IndicatorPtr a, double c) { return ble(std::move(a), constant(c)); }
    inline BoolSignalPtr bgt(IndicatorPtr a, double c) { return bgt(std::move(a), constant(c)); }
    inline BoolSignalPtr bge(IndicatorPtr a, double c) { return bge(std::move(a), constant(c)); }
    inline BoolSignalPtr beq(IndicatorPtr a, double c) { return beq(std::move(a), constant(c)); }
    inline BoolSignalPtr bne(IndicatorPtr a, double c) { return bne(std::move(a), constant(c)); }

    // =============================================================
    // Factory helpers: crosses
    // =============================================================
    inline BoolSignalPtr cross_up(IndicatorPtr a, IndicatorPtr b) {
        return std::make_shared<CrossSignal>(std::move(a), std::move(b), CrossDir::Up);
    }
    inline BoolSignalPtr cross_down(IndicatorPtr a, IndicatorPtr b) {
        return std::make_shared<CrossSignal>(std::move(a), std::move(b), CrossDir::Down);
    }
    inline BoolSignalPtr cross_up(IndicatorPtr a, double c) { return cross_up(std::move(a), constant(c)); }
    inline BoolSignalPtr cross_down(IndicatorPtr a, double c) { return cross_down(std::move(a), constant(c)); }

} // namespace sugar