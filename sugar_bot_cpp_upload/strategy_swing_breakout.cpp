#include "strategy_swing_breakout.h"
#include "indicators_ema.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace sugar {

    namespace {
        inline double qnan() {
            return std::numeric_limits<double>::quiet_NaN();
        }
    }

    SwingBreakoutStrategy::SwingBreakoutStrategy(std::size_t left_bars,
        std::size_t right_bars,
        bool use_ema10_stop,
        int days_above_10_required,
        double pct_gain_threshold,
        int days_for_gain,
        double max_loss_pct)
        : left_(left_bars),
        right_(right_bars),
        use_ema10_stop_(use_ema10_stop),
        days_above_10_required_(days_above_10_required),
        pct_gain_threshold_(pct_gain_threshold),
        days_for_gain_(days_for_gain),
        max_loss_pct_(max_loss_pct) {
    }

    BacktestResult SwingBreakoutStrategy::run(const CandleSeries& data) {
        BacktestResult r{};
        const std::size_t n = data.size();
        if (n == 0) return r;

        // Precompute OHLC vectors
        const auto& closes = data.closes();
        std::vector<double> highs(n), lows(n);
        for (std::size_t i = 0; i < n; ++i) {
            const auto& c = data[i];
            highs[i] = c.high;
            lows[i] = c.low;
        }

        // 10-day EMA on closes
        EMAIndicator ema10_ind(10);
        const auto ema10 = ema10_ind.compute(data);

        // Strategy state
        bool long_on = false;
        double entry = 0.0;

        double equity = 0.0;                                                                // cumulative % return
        double peak = 0.0;                                                                  // peak equity for drawdown
        int first_signal_date = 0;

        // trend & breakout state
        bool trend_up = false;                                                              // trendState == 1
        bool bo_flagged = false;                                                            // boFlagged

        double last_swing_high = qnan();
        int    last_swing_high_bar = -1;

        double breakout_low = qnan();
        double breakout_price = qnan();
        int    breakout_bar = -1;
        int    days_above_10 = 0;
        bool   validation_passed = false;
        double entry_price = qnan();

        for (std::size_t i = 0; i < n; ++i) {
            const double close = closes[i];
            const double high = highs[i];
            const double low = lows[i];

            bool is_breakout = false;
            bool is_swing_failure = false;

            // --- STRICT SWING HIGH DETECTION (pivot-based) ---
            // Mimic ta.pivothigh(high, leftBars, rightBars):
            // A pivot at bar p is confirmed at p + right_ (our current i).
            if (right_ > 0 && i >= right_) {
                const int p = static_cast<int>(i) - static_cast<int>(right_);
                if (p >= 0 && p >= static_cast<int>(left_) &&
                    p + static_cast<int>(right_) < static_cast<int>(n)) {

                    const double ph = highs[static_cast<std::size_t>(p)];
                    bool is_pivot_high = true;

                    const int start = p - static_cast<int>(left_);
                    const int end = p + static_cast<int>(right_);

                    for (int j = start; j <= end; ++j) {
                        if (j == p) continue;
                        if (highs[static_cast<std::size_t>(j)] >= ph) {
                            is_pivot_high = false;
                            break;
                        }
                    }

                    if (is_pivot_high) {
                        last_swing_high = ph;
                        last_swing_high_bar = p;

                        // In Pine: if isStrictSwingHigh and trendState != 1 -> boFlagged := false
                        if (!trend_up) {
                            bo_flagged = false;
                        }
                    }
                }
            }

            // --- 8% STOP LOSS: loss from entryPrice ---
            if (trend_up && !std::isnan(entry_price)) {
                const double current_loss_pct =
                    (entry_price - close) / entry_price * 100.0;
                if (current_loss_pct >= max_loss_pct_) {
                    is_swing_failure = true;
                }
            }

            // --- VALIDATION PHASE: breakout low violation ---
            if (trend_up && !validation_passed && !std::isnan(breakout_low)) {
                if (low < breakout_low) {
                    is_swing_failure = true;
                }
            }

            // --- VALIDATION PHASE: track days above EMA10 ---
            if (trend_up && !validation_passed && breakout_bar >= 0) {
                const double e = ema10[i];
                if (!std::isnan(e) && close > e) {
                    ++days_above_10;
                }
                else {
                    days_above_10 = 0;
                }
            }

            // --- VALIDATION PHASE: check criteria ---
            if (trend_up && !validation_passed && breakout_bar >= 0) {
                const int bars_since_breakout =
                    static_cast<int>(i) - breakout_bar;
                const double pct_gain =
                    (close - breakout_price) / breakout_price * 100.0;

                if (days_above_10 >= days_above_10_required_ &&
                    pct_gain >= pct_gain_threshold_ &&
                    bars_since_breakout <= days_for_gain_) {

                    validation_passed = true;
                }
            }

            // --- 10-DAY EMA STOP ---
            if (trend_up && use_ema10_stop_) {
                const double e = ema10[i];
                if (!std::isnan(e) && close < e) {
                    is_swing_failure = true;
                }
            }

            // --- BREAKOUT DETECTION (not in uptrend) ---
            if (!std::isnan(last_swing_high) &&
                high > last_swing_high &&
                !trend_up &&
                !bo_flagged) {

                // If we fail to close above last swing high -> swing failure
                if (close < last_swing_high) {
                    is_swing_failure = true;
                }
                else {
                    // Valid breakout
                    is_breakout = true;
                    trend_up = true;
                    bo_flagged = true;

                    entry_price = close;
                    breakout_price = close;
                    breakout_low = low;
                    breakout_bar = static_cast<int>(i);

                    const double e = ema10[i];
                    days_above_10 = (!std::isnan(e) && close > e) ? 1 : 0;
                    validation_passed = false;

                    if (first_signal_date == 0) {
                        first_signal_date = data[i].date;
                    }
                }
            }

            // --- EXECUTE TRADES  ---

            // Entry on breakout
            if (is_breakout && !long_on) {
                long_on = true;
                entry = close;
            }

            // Exit on any swing failure condition while long
            if (is_swing_failure && long_on) {
                const double trade_ret = (close / entry - 1.0) * 100.0;
                equity += trade_ret;
                ++r.trades;

                peak = std::max(peak, equity);
                r.max_drawdown = std::max(r.max_drawdown, peak - equity);

                long_on = false;
                trend_up = false;

                // Reset breakout validation state
                bo_flagged = false;
                breakout_low = qnan();
                breakout_price = qnan();
                breakout_bar = -1;
                days_above_10 = 0;
                validation_passed = false;
                entry_price = qnan();
            }
        }

        // Close any open position at the last bar
        if (long_on && n > 0) {
            const double trade_ret = (closes.back() / entry - 1.0) * 100.0;
            equity += trade_ret;
            ++r.trades;

            peak = std::max(peak, equity);
            r.max_drawdown = std::max(r.max_drawdown, peak - equity);
        }

        r.pnl = equity;
        r.best_start_date = first_signal_date;
        return r;
    }

} // namespace sugar
