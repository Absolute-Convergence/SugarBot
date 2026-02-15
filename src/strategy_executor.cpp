#include "strategy_executor.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <cstdint>

#include "indicators_composite_bool.h" // BoolSignal + BoolSeries

namespace sugar {

    namespace {

        inline BoolSeries ensure_size(BoolSeries v, std::size_t n) {
            if (v.size() == n) return v;
            if (v.size() < n) {
                v.resize(n, 0u);
                return v;
            }
            v.resize(n);
            return v;
        }

        inline BoolSeries all_ones(std::size_t n) {
            return BoolSeries(n, 1u);
        }

        inline double apply_costs(double pct_return, const ExecCosts& costs) {
            // Costs applied as a simple % drag in basis points (bps).
            // This keeps v1 deterministic and easily upgradable later.
            const double drag_pct = (costs.fee_bps + costs.slippage_bps) / 100.0;
            return pct_return - drag_pct;
        }

        enum class Pos : int {
            Flat = 0,
            Long = 1,
            Short = -1
        };

        inline double long_return_pct(double entry, double exit) {
            if (!std::isfinite(entry) || !std::isfinite(exit) || entry == 0.0) return qnan();
            return (exit - entry) / entry * 100.0;
        }

        inline double short_return_pct(double entry, double exit) {
            if (!std::isfinite(entry) || !std::isfinite(exit) || entry == 0.0) return qnan();
            return (entry - exit) / entry * 100.0;
        }

        inline double fill_price(const SeriesView& view,
            std::size_t i,
            FillTiming timing,
            bool /*is_entry*/) {
            // v1: close fills are always available (close is required by SeriesView).
            // NextBarOpen uses open[i+1] when available; otherwise falls back to close.
            const auto close = view.close();
            const std::size_t n = close.size();
            if (n == 0) return qnan();

            if (timing == FillTiming::SameBarClose) {
                return close[i];
            }

            // NextBarOpen
            if (i + 1 >= n) {
                // No next bar; cannot fill realistically. Fallback to last close.
                return close[n - 1];
            }
            if (view.has_open()) {
                return view.open()[i + 1];
            }
            return close[i + 1];
        }

        inline double fill_price_on_bar(const SeriesView& view,
            std::size_t i,
            FillTiming timing,
            bool /*is_entry*/) {
            // Used by BarEvalMode::NextBar: i is the execution bar.
            // SameBarClose -> close[i]. NextBarOpen -> open[i] if available else close[i].
            const auto close = view.close();
            const std::size_t n = close.size();
            if (n == 0) return qnan();
            if (i >= n) return qnan();

            if (timing == FillTiming::SameBarClose) {
                return close[i];
            }

            // NextBarOpen (interpreted as execution-bar open)
            if (view.has_open()) {
                return view.open()[i];
            }
            return close[i];
        }

        inline std::uint64_t hash_combine(std::uint64_t h, std::uint64_t x) {
            // classic boost-style mix; deterministic and fast
            return h ^ (x + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2));
        }

    } // namespace

    BacktestResult StrategyExecutor::run(const CandleSeries& series,
        const StrategyPlan& plan,
        const ExecSettings& settings) const {
        return run(SeriesView::from_series(series), plan, settings);
    }

    BacktestResult StrategyExecutor::run(const SeriesView& view,
        const StrategyPlan& plan,
        const ExecSettings& settings) const {
        BacktestResult out{};
        std::uint64_t h = 0x84222325cbf29ce4ULL; 

        const std::size_t n = view.size();
        if (n == 0) return out;

        // Compute signals (aligned bool vectors).
        const BoolSeries entryL = ensure_size(plan.signals.entry_long
            ? plan.signals.entry_long->compute(view)
            : BoolSeries{},
            n);
        const BoolSeries exitL = ensure_size(plan.signals.exit_long
            ? plan.signals.exit_long->compute(view)
            : BoolSeries{},
            n);

        const BoolSeries entryS = ensure_size(plan.signals.entry_short
            ? plan.signals.entry_short->compute(view)
            : BoolSeries{},
            n);
        const BoolSeries exitS = ensure_size(plan.signals.exit_short
            ? plan.signals.exit_short->compute(view)
            : BoolSeries{},
            n);

        const BoolSeries allow = ensure_size(plan.signals.allow_trade
            ? plan.signals.allow_trade->compute(view)
            : all_ones(n),
            n);

        // Date reporting: prefer the plan's precomputed warm-up index if provided.
        if (plan.first_valid_index != StrategyPlan::npos &&
            plan.first_valid_index < n &&
            view.has_series()) {
            out.best_start_date = view.series().rows()[plan.first_valid_index].date;
        }

        // FSM state
        Pos pos = Pos::Flat;
        double entry_price = qnan();
        std::size_t entry_i = StrategyPlan::npos;

        double equity = settings.initial_equity;
        double peak_equity = equity;

        auto close_trade = [&](Pos side, std::size_t exit_i, double exit_price) {
            if (side == Pos::Flat) return;
            if (!std::isfinite(entry_price) || !std::isfinite(exit_price)) {
                // If prices are invalid, treat as no-op close to preserve determinism.
                pos = Pos::Flat;
                entry_price = qnan();
                entry_i = StrategyPlan::npos;
                return;
            }

            double r = qnan();
            if (side == Pos::Long) {
                r = long_return_pct(entry_price, exit_price);
                if (std::isfinite(r)) {
                    out.long_trades += 1;
                    out.pnl_long += r;
                }
            }
            else if (side == Pos::Short) {
                r = short_return_pct(entry_price, exit_price);
                if (std::isfinite(r)) {
                    out.short_trades += 1;
                    out.pnl_short += r;
                }
            }

            if (std::isfinite(r)) {
                r = apply_costs(r, settings.costs);
                out.trades += 1;
                out.pnl += r;
                // ---- NEW: mix trade signature into hash ----
                // hash only depends on behavior (side + entry/exit indices).
                // If two runs execute the same trades on the same bars, they will match.
                const std::uint64_t side_tag = (side == Pos::Long) ? 1ULL : 2ULL;
                h = hash_combine(h, side_tag);
                h = hash_combine(h, static_cast<std::uint64_t>(entry_i));
                h = hash_combine(h, static_cast<std::uint64_t>(exit_i));
                // -------------------------------------------
                equity *= (1.0 + r / 100.0);
                peak_equity = std::max(peak_equity, equity);
                const double dd = (peak_equity > 0.0) ? (peak_equity - equity) / peak_equity * 100.0 : 0.0;
                out.max_drawdown = std::max(out.max_drawdown, dd);
            }

            pos = Pos::Flat;
            entry_price = qnan();
            entry_i = StrategyPlan::npos;
            };

        auto open_trade = [&](Pos side, std::size_t i, double price) {
            if (side == Pos::Flat) return;
            if (!std::isfinite(price)) return;
            pos = side;
            entry_price = price;
            entry_i = i;

            // If we didn't have a precomputed start date, use the first entry.
            if (out.best_start_date == 0 && view.has_series()) {
                out.best_start_date = view.series().rows()[i].date;
            }
            };

        const bool allow_short = (plan.mode == TradeMode::LongShort);

        // =============================================================
// Execution timing models
//
// SameBar:
//   - Evaluate signals at bar i and execute on bar i.
//   - fill_timing selects the fill price (close[i] or next-bar open).
// NextBar:
//   - Evaluate signals at bar i ("decision bar") and execute on bar i+1 ("execution bar").
//   - fill_timing selects the fill price on the execution bar (close[j] or open[j]).
// =============================================================

        if (settings.eval_mode == BarEvalMode::NextBar) {
            // Decision on i, execute on j=i+1.
            // Stop at n-2 because signals on the final bar have no next bar to execute on.
            for (std::size_t i = 0; i + 1 < n; ++i) {
                const std::size_t j = i + 1;

                const bool gate = (allow[i] != 0u);

                const bool el = gate && (entryL[i] != 0u);
                const bool xl = (exitL[i] != 0u);

                const bool es = gate && allow_short && (entryS[i] != 0u);
                const bool xs = allow_short && (exitS[i] != 0u);

                if (pos == Pos::Long) {
                    const bool wants_reverse = allow_short && es;
                    if (xl || wants_reverse) {
                        const double px = fill_price_on_bar(view, j, settings.fill_timing, false);
                        close_trade(Pos::Long, j, px);

                        // Optional same-bar reversal (on the execution bar).
                        if (wants_reverse && settings.allow_same_bar_reversal) {
                            open_trade(Pos::Short, j, px);
                        }
                        continue;
                    }
                }
                else if (pos == Pos::Short) {
                    const bool wants_reverse = el;
                    if (xs || wants_reverse) {
                        const double px = fill_price_on_bar(view, j, settings.fill_timing, false);
                        close_trade(Pos::Short, j, px);

                        if (wants_reverse && settings.allow_same_bar_reversal) {
                            open_trade(Pos::Long, j, px);
                        }
                        continue;
                    }
                }

                if (pos == Pos::Flat) {
                    if (el) {
                        const double px = fill_price_on_bar(view, j, settings.fill_timing, true);
                        open_trade(Pos::Long, j, px);
                    }
                    else if (es) {
                        const double px = fill_price_on_bar(view, j, settings.fill_timing, true);
                        open_trade(Pos::Short, j, px);
                    }
                }
            }
        }
        else {
            // =============================================================
            // SameBar pipeline (original behavior)
            // =============================================================
            for (std::size_t i = 0; i < n; ++i) {
                const bool gate = (allow[i] != 0u);

                const bool el = gate && (entryL[i] != 0u);
                const bool xl = (exitL[i] != 0u);

                const bool es = gate && allow_short && (entryS[i] != 0u);
                const bool xs = allow_short && (exitS[i] != 0u);

                if (pos == Pos::Long) {
                    const bool wants_reverse = allow_short && es;
                    if (xl || wants_reverse) {
                        const double px = fill_price(view, i, settings.fill_timing, false);
                        close_trade(Pos::Long, i, px);

                        if (wants_reverse && settings.allow_same_bar_reversal) {
                            open_trade(Pos::Short, i, px);
                        }
                        continue;
                    }
                }
                else if (pos == Pos::Short) {
                    const bool wants_reverse = el;
                    if (xs || wants_reverse) {
                        const double px = fill_price(view, i, settings.fill_timing, false);
                        close_trade(Pos::Short, i, px);

                        if (wants_reverse && settings.allow_same_bar_reversal) {
                            open_trade(Pos::Long, i, px);
                        }
                        continue;
                    }
                }

                if (pos == Pos::Flat) {
                    if (el) {
                        const double px = fill_price(view, i, settings.fill_timing, true);
                        open_trade(Pos::Long, i, px);
                    }
                    else if (es) {
                        const double px = fill_price(view, i, settings.fill_timing, true);
                        open_trade(Pos::Short, i, px);
                    }
                }
            }
        }

        // Close out any open trade at end if requested.
        if (settings.close_out_at_end && pos != Pos::Flat) {
            const std::size_t last = n - 1;
            const double px = view.close()[last];
            if (pos == Pos::Long) close_trade(Pos::Long, last, px);
            else if (pos == Pos::Short) close_trade(Pos::Short, last, px);
        }

        out.trade_hash = (out.trades > 0) ? h : 0ULL; // 0 means "no trades"

        return out;
    }

} // namespace sugar