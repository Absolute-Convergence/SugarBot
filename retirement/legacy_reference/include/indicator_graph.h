#pragma once

#include <functional>
#include <memory>
#include <utility>
#include <vector>
#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "compiled_program.h"
#include "indicator.h"

namespace sugar {

    class MapIndicator final : public Indicator {
    public:
        using Fn = std::function<std::vector<double>(const std::vector<double>&)>;

        MapIndicator(IndicatorPtr base, Fn fn)
            : base_(std::move(base)), fn_(std::move(fn)) {
        }

        std::vector<double> compute(const CandleSeries& series) const override;
        std::vector<double> compute(const SeriesView& view) const override;

    private:
        IndicatorPtr base_;
        Fn fn_;
    };

    class CloseIndicator final : public Indicator {
    public:
        CloseIndicator() = default;

        std::vector<double> compute(const CandleSeries& series) const override {
            return std::vector<double>(series.closes().begin(), series.closes().end());
        }

        std::vector<double> compute(const SeriesView& view) const override {
            return std::vector<double>(view.close().begin(), view.close().end());
        }
    };

    class ConstIndicator final : public Indicator {
    public:
        explicit ConstIndicator(double value) : value_(value) {}

        std::vector<double> compute(const CandleSeries& series) const override {
            return std::vector<double>(series.size(), value_);
        }

        std::vector<double> compute(const SeriesView& view) const override {
            return std::vector<double>(view.size(), value_);
        }

    private:
        double value_{};
    };

    inline IndicatorPtr constant(double value) {
        return std::make_shared<ConstIndicator>(value);
    }

    class BinaryMapIndicator final : public Indicator {
    public:
        using Fn2 = std::function<std::vector<double>(
            const std::vector<double>&,
            const std::vector<double>&
        )>;

        BinaryMapIndicator(IndicatorPtr lhs, IndicatorPtr rhs, Fn2 fn)
            : lhs_(std::move(lhs)), rhs_(std::move(rhs)), fn_(std::move(fn)) {
        }

        std::vector<double> compute(const CandleSeries& series) const override;
        std::vector<double> compute(const SeriesView& view) const override;

    private:
        IndicatorPtr lhs_;
        IndicatorPtr rhs_;
        Fn2 fn_;
    };

    // Legacy compatibility graph layer:
    // - This header is no longer part of the stable engine contract.
    // - Canonical runtime bool-buffer ownership now lives in compiled_program.h/program_evaluator.h.
    using BoolSeries = BoolBuffer;

    inline std::uint8_t b0() { return kBoolFalse; }
    inline std::uint8_t b1() { return kBoolTrue; }

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
        virtual BoolSeries compute(const SeriesView& view) const = 0;
        virtual BoolSeries compute(const CandleSeries& series) const {
            return compute(SeriesView::from_series(series));
        }
    };

    using BoolSignalPtr = std::shared_ptr<BoolSignal>;

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

    inline BoolSignalPtr bnot(BoolSignalPtr a) {
        return std::make_shared<BoolMap>(
            std::move(a),
            [](const BoolSeries& x) {
                BoolSeries out(x.size(), b0());
                for (std::size_t i = 0; i < x.size(); ++i) out[i] = x[i] ? b0() : b1();
                return out;
            });
    }

    inline BoolSignalPtr band(BoolSignalPtr a, BoolSignalPtr b) {
        return std::make_shared<BoolBinaryMap>(
            std::move(a), std::move(b),
            [](const BoolSeries& x, const BoolSeries& y) {
                return bool_zip_pad(x, y, [](std::uint8_t av, std::uint8_t bv) { return (av && bv) ? b1() : b0(); });
            });
    }

    inline BoolSignalPtr bor(BoolSignalPtr a, BoolSignalPtr b) {
        return std::make_shared<BoolBinaryMap>(
            std::move(a), std::move(b),
            [](const BoolSeries& x, const BoolSeries& y) {
                return bool_zip_pad(x, y, [](std::uint8_t av, std::uint8_t bv) { return (av || bv) ? b1() : b0(); });
            });
    }

    inline BoolSignalPtr bnand(BoolSignalPtr a, BoolSignalPtr b) {
        return bnot(band(std::move(a), std::move(b)));
    }

    inline BoolSignalPtr bnor(BoolSignalPtr a, BoolSignalPtr b) {
        return bnot(bor(std::move(a), std::move(b)));
    }

    inline BoolSignalPtr blt(IndicatorPtr a, IndicatorPtr b) { return std::make_shared<CompareSignal>(std::move(a), std::move(b), CmpOp::LT); }
    inline BoolSignalPtr ble(IndicatorPtr a, IndicatorPtr b) { return std::make_shared<CompareSignal>(std::move(a), std::move(b), CmpOp::LE); }
    inline BoolSignalPtr bgt(IndicatorPtr a, IndicatorPtr b) { return std::make_shared<CompareSignal>(std::move(a), std::move(b), CmpOp::GT); }
    inline BoolSignalPtr bge(IndicatorPtr a, IndicatorPtr b) { return std::make_shared<CompareSignal>(std::move(a), std::move(b), CmpOp::GE); }
    inline BoolSignalPtr beq(IndicatorPtr a, IndicatorPtr b) { return std::make_shared<CompareSignal>(std::move(a), std::move(b), CmpOp::EQ); }
    inline BoolSignalPtr bne(IndicatorPtr a, IndicatorPtr b) { return std::make_shared<CompareSignal>(std::move(a), std::move(b), CmpOp::NE); }

    inline BoolSignalPtr blt(IndicatorPtr a, double c) { return blt(std::move(a), constant(c)); }
    inline BoolSignalPtr ble(IndicatorPtr a, double c) { return ble(std::move(a), constant(c)); }
    inline BoolSignalPtr bgt(IndicatorPtr a, double c) { return bgt(std::move(a), constant(c)); }
    inline BoolSignalPtr bge(IndicatorPtr a, double c) { return bge(std::move(a), constant(c)); }
    inline BoolSignalPtr beq(IndicatorPtr a, double c) { return beq(std::move(a), constant(c)); }
    inline BoolSignalPtr bne(IndicatorPtr a, double c) { return bne(std::move(a), constant(c)); }

    inline BoolSignalPtr cross_up(IndicatorPtr a, IndicatorPtr b) {
        return std::make_shared<CrossSignal>(std::move(a), std::move(b), CrossDir::Up);
    }
    inline BoolSignalPtr cross_down(IndicatorPtr a, IndicatorPtr b) {
        return std::make_shared<CrossSignal>(std::move(a), std::move(b), CrossDir::Down);
    }
    inline BoolSignalPtr cross_up(IndicatorPtr a, double c) { return cross_up(std::move(a), constant(c)); }
    inline BoolSignalPtr cross_down(IndicatorPtr a, double c) { return cross_down(std::move(a), constant(c)); }

} // namespace sugar
