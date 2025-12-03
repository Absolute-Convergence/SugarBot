#pragma once
#include "strategy.h"

namespace sugar {

    // Minimal port of the Pine "Swing Breakout Strategy with Validation":
    // - Detect swing highs using left/right pivot bars
    // - Breakout = price breaks above last swing high and CLOSES above it
    // - Validation: must stay above 10-day EMA for X days AND reach Y% gain within Z bars
    // - Exits: 8% max loss, 10-day EMA break, or breakout-low violation during validation
    class SwingBreakoutStrategy final : public IStrategy {
    public:
        SwingBreakoutStrategy(std::size_t left_bars,
            std::size_t right_bars,
            bool use_ema10_stop,
            int days_above_10_required,
            double pct_gain_threshold,
            int days_for_gain,
            double max_loss_pct);

        BacktestResult run(const CandleSeries& data) override;

    private:
        std::size_t left_;
        std::size_t right_;
        bool use_ema10_stop_;
        int days_above_10_required_;
        double pct_gain_threshold_;
        int days_for_gain_;
        double max_loss_pct_;
    };

} // namespace sugar
