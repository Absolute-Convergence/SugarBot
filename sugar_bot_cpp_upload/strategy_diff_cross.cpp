#include "strategy_diff_cross.h"
#include <algorithm>
#include <cmath>

namespace sugar {

    BacktestResult DiffCrossStrategy::run(const CandleSeries& data) {
        BacktestResult r{};
        if (data.size() == 0 || !a_ || !b_) {
            return r;
        }

        const auto av = a_->compute(data);
        const auto bv = b_->compute(data);
        const auto closes = data.closes();

        const std::size_t n = std::min({ av.size(), bv.size(), closes.size() });
        if (n == 0) return r;

        auto is_nan = [](double x) { return std::isnan(x); };

        // Find first index where both indicators are usable (not NaN)
        std::size_t i0 = 0;
        for (; i0 < n; ++i0) {
            if (!is_nan(av[i0]) && !is_nan(bv[i0])) {
                break;
            }
        }
        if (i0 >= n) {
            return r;
        }

        r.best_start_date = data[i0].date;

        double equity = 0.0;  // cumulative % return
        double peak = 0.0;  // peak equity for drawdown

        auto update_dd = [&](double eq) {
            peak = std::max(peak, eq);
            r.max_drawdown = std::max(r.max_drawdown, peak - eq);
            };

        auto close_long = [&](std::size_t i, double entry) {
            const double trade_ret = (closes[i] / entry - 1.0) * 100.0;
            equity += trade_ret;

            ++r.trades;
            ++r.long_trades;
            r.pnl_long += trade_ret;

            update_dd(equity);
            };

        auto close_short = [&](std::size_t i, double entry) {
            // For a short, PnL% = entry/close - 1
            const double trade_ret = (entry / closes[i] - 1.0) * 100.0;
            equity += trade_ret;

            ++r.trades;
            ++r.short_trades;
            r.pnl_short += trade_ret;

            update_dd(equity);
            };

        auto diff_at = [&](std::size_t i) {
            return av[i] - bv[i];
            };

        if (mode_ == Mode::LongOnly) {
            // ----- Long-only behavior -----
            bool   long_on = false;
            double entry = 0.0;

            for (std::size_t i = i0; i < n; ++i) {
                const double d = diff_at(i);

                // Enter long when diff >= +thresh_
                if (!long_on && d >= thresh_) {
                    long_on = true;
                    entry = closes[i];
                }
                // Exit long when diff <= -thresh_
                else if (long_on && d <= -thresh_) {
                    close_long(i, entry);
                    long_on = false;
                }
            }

            // Close any open position at the last bar
            if (long_on) {
                close_long(n - 1, entry);
            }
        }
        else {
            // ----- LongShort mode: stop-and-reverse -----
            // pos: +1 (long), 0 (flat), -1 (short)
            int    pos = 0;
            double entry = 0.0;

            for (std::size_t i = i0; i < n; ++i) {
                const double d = diff_at(i);

                if (pos == 0) {
                    // flat -> open long or short
                    if (d >= thresh_) {
                        pos = +1;
                        entry = closes[i];
                    }
                    else if (d <= -thresh_) {
                        pos = -1;
                        entry = closes[i];
                    }
                }
                else if (pos == +1) {
                    // currently long, flip to short when diff <= -thresh_
                    if (d <= -thresh_) {
                        close_long(i, entry);         // close long
                        pos = -1;                   // open short
                        entry = closes[i];
                    }
                }
                else if (pos == -1) {
                    // currently short, flip to long when diff >= +thresh_
                    if (d >= thresh_) {
                        close_short(i, entry);        // close short
                        pos = +1;                   // open long
                        entry = closes[i];
                    }
                }
            }

            // Close any open position at the last bar
            if (pos == +1) {
                close_long(n - 1, entry);
            }
            else if (pos == -1) {
                close_short(n - 1, entry);
            }
        }

        r.pnl = equity;
        return r;
    }

} // namespace sugar
