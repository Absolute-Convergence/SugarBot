#pragma once
#include <utility>
#include "strategy.h"
#include "indicator.h"

namespace sugar {

    // Go long and/or short when (A - B) crosses thresholds.
    // diff = A - B
    //
    // LongOnly mode:
    //   - Enter long when diff >= +thresh.
    //   - Exit long when diff <= -thresh.
    //
    // LongShort mode:
    //   - If flat:
    //       diff >= +thresh -> open long
    //       diff <= -thresh -> open short
    //   - If long and diff <= -thresh -> close long, open short (stop-and-reverse)
    //   - If short and diff >= +thresh -> close short, open long (stop-and-reverse)
    class DiffCrossStrategy final : public IStrategy {
    public:
        enum class Mode {
            LongOnly,
            LongShort
        };

        DiffCrossStrategy(IndicatorPtr a,
            IndicatorPtr b,
            double thresh_percent,
            Mode mode = Mode::LongOnly)
            : a_(std::move(a)),
            b_(std::move(b)),
            thresh_(thresh_percent),
            mode_(mode) {
        }

        BacktestResult run(const CandleSeries& data) override;

    private:
        IndicatorPtr a_;
        IndicatorPtr b_;
        double       thresh_{};
        Mode         mode_{ Mode::LongOnly };
    };

} // namespace sugar
