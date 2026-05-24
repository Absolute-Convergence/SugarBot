#include "engine/strategy_executor.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

#include "engine/program_evaluator.h"
#include "program_evaluator_internal.h"

namespace sugar {

    namespace {

        double qnan() {
            return std::numeric_limits<double>::quiet_NaN();
        }

        double apply_costs(double pct_return, const ExecCosts& costs) {
            const double drag_pct = (costs.fee_bps + costs.slippage_bps) / 100.0;
            return pct_return - drag_pct;
        }

        enum class Pos : int {
            Flat = 0,
            Long = 1,
            Short = -1
        };

        double long_return_pct(double entry, double exit) {
            if (!std::isfinite(entry) || !std::isfinite(exit) || entry == 0.0) return qnan();
            return (exit - entry) / entry * 100.0;
        }

        double short_return_pct(double entry, double exit) {
            if (!std::isfinite(entry) || !std::isfinite(exit) || entry == 0.0) return qnan();
            return (entry - exit) / entry * 100.0;
        }

        std::span<const double> find_series_col(
            const DataView& dv,
            const std::string& series_key,
            const char* name) {
            ColumnId id{};
            if (!dv.bundle().schema || !dv.bundle().schema->columns().find(name, id)) {
                return {};
            }
            const ColumnRef ref{ series_key, id };
            if (!dv.has_col(ref)) {
                return {};
            }
            return dv.col(ref);
        }

        double fill_price(
            std::span<const double> close,
            std::span<const double> open,
            std::size_t i,
            FillTiming timing) {
            const std::size_t n = close.size();
            if (n == 0 || i >= n) return qnan();

            if (timing == FillTiming::SameBarClose) {
                return close[i];
            }

            if (i + 1 >= n) {
                return close[n - 1];
            }
            if (!open.empty()) {
                return open[i + 1];
            }
            return close[i + 1];
        }

        double fill_price_on_bar(
            std::span<const double> close,
            std::span<const double> open,
            std::size_t i,
            FillTiming timing) {
            const std::size_t n = close.size();
            if (n == 0 || i >= n) return qnan();

            if (timing == FillTiming::SameBarClose) {
                return close[i];
            }
            if (!open.empty()) {
                return open[i];
            }
            return close[i];
        }

        std::uint64_t hash_combine(std::uint64_t h, std::uint64_t x) {
            return h ^ (x + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2));
        }

        std::size_t count_active_signals(const BoolBuffer& buffer) {
            return static_cast<std::size_t>(std::count_if(
                buffer.begin(),
                buffer.end(),
                [](std::uint8_t value) {
                    return value != 0u;
                }));
        }

    } // namespace

    bool StrategyExecutor::run(
        const DataView& dv,
        const ColumnRef& primary,
        const StrategyPlan& plan,
        const ExecSettings& settings,
        BacktestResult& out,
        std::string& err,
        ExecutionCancelCallback cancel_requested) const {
        out = BacktestResult{};
        err.clear();
        if (!plan.has_storage()) {
            err = "strategy plan storage is missing";
            return false;
        }
        if (!dv.has_col(primary)) {
            err = "primary series/column is missing from DataView";
            return false;
        }
        if (cancel_requested && cancel_requested()) {
            err = "execution canceled";
            return false;
        }

        const auto close = dv.col(primary);
        const std::size_t n = close.size();
        if (n == 0) {
            return true;
        }

        ProgramEvaluationBuffers buffers{};
        if (!evaluate_strategy_plan_program_buffers(dv, primary, plan, buffers, err)) {
            if (err.empty()) {
                err = "plan program evaluation failed";
            }
            return false;
        }
        ProgramSignalOutputs signals = std::move(buffers.signals);
        out.dynamic_execution = std::move(buffers.dynamic_execution);
        if (cancel_requested && cancel_requested()) {
            err = "execution canceled";
            return false;
        }

        out.rows_evaluated = n;
        out.warmup_rows =
            (plan.first_valid_index != StrategyPlan::npos)
            ? std::min<std::size_t>(plan.first_valid_index, n)
            : 0;
        out.signal_counts.allow_trade = count_active_signals(signals.allow_trade);
        out.signal_counts.entry_long = count_active_signals(signals.entry_long);
        out.signal_counts.exit_long = count_active_signals(signals.exit_long);
        out.signal_counts.entry_short = count_active_signals(signals.entry_short);
        out.signal_counts.exit_short = count_active_signals(signals.exit_short);

        const auto open = find_series_col(dv, primary.series, "open");
        const auto time_axis = dv.time_axis();
        std::uint64_t h = 0x84222325cbf29ce4ULL;

        if (plan.first_valid_index != StrategyPlan::npos &&
            plan.first_valid_index < n &&
            !time_axis.empty()) {
            out.best_start_date = time_axis[plan.first_valid_index];
            out.has_best_start_date = true;
        }

        Pos pos = Pos::Flat;
        double entry_price = qnan();
        std::size_t entry_i = StrategyPlan::npos;

        double equity = settings.initial_equity;
        double peak_equity = equity;

        auto close_trade = [&](Pos side, std::size_t exit_i, double exit_price) {
            if (side == Pos::Flat) return;
            if (!std::isfinite(entry_price) || !std::isfinite(exit_price)) {
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
                const std::uint64_t side_tag = (side == Pos::Long) ? 1ULL : 2ULL;
                h = hash_combine(h, side_tag);
                h = hash_combine(h, static_cast<std::uint64_t>(entry_i));
                h = hash_combine(h, static_cast<std::uint64_t>(exit_i));
                equity *= (1.0 + r / 100.0);
                peak_equity = std::max(peak_equity, equity);
                const double dd = (peak_equity > 0.0)
                    ? (peak_equity - equity) / peak_equity * 100.0
                    : 0.0;
                out.max_drawdown = std::max(out.max_drawdown, dd);
            }

            pos = Pos::Flat;
            entry_price = qnan();
            entry_i = StrategyPlan::npos;
        };

        auto open_trade = [&](Pos side, std::size_t i, double price) {
            if (side == Pos::Flat || !std::isfinite(price)) return;
            pos = side;
            entry_price = price;
            entry_i = i;

            if (!out.has_best_start_date && !time_axis.empty() && i < time_axis.size()) {
                out.best_start_date = time_axis[i];
                out.has_best_start_date = true;
            }
        };

        const bool allow_short = (plan.mode == TradeMode::LongShort);

        if (settings.eval_mode == BarEvalMode::NextBar) {
            for (std::size_t i = 0; i + 1 < n; ++i) {
                if (cancel_requested && cancel_requested()) {
                    err = "execution canceled";
                    return false;
                }
                const std::size_t j = i + 1;

                const bool gate = (signals.allow_trade[i] != 0u);
                const bool el = gate && (signals.entry_long[i] != 0u);
                const bool xl = (signals.exit_long[i] != 0u);
                const bool es = gate && allow_short && (signals.entry_short[i] != 0u);
                const bool xs = allow_short && (signals.exit_short[i] != 0u);

                if (pos == Pos::Long) {
                    const bool wants_reverse = allow_short && es;
                    if (settings.exits_first) {
                        if (xl || wants_reverse) {
                            const double px = fill_price_on_bar(close, open, j, settings.fill_timing);
                            close_trade(Pos::Long, j, px);
                            if (wants_reverse && settings.allow_same_bar_reversal) {
                                open_trade(Pos::Short, j, px);
                            }
                            continue;
                        }
                    }
                    else {
                        if (wants_reverse) {
                            const double px = fill_price_on_bar(close, open, j, settings.fill_timing);
                            close_trade(Pos::Long, j, px);
                            open_trade(Pos::Short, j, px);
                            continue;
                        }
                        if (xl) {
                            const double px = fill_price_on_bar(close, open, j, settings.fill_timing);
                            close_trade(Pos::Long, j, px);
                            continue;
                        }
                    }
                }
                else if (pos == Pos::Short) {
                    const bool wants_reverse = el;
                    if (settings.exits_first) {
                        if (xs || wants_reverse) {
                            const double px = fill_price_on_bar(close, open, j, settings.fill_timing);
                            close_trade(Pos::Short, j, px);
                            if (wants_reverse && settings.allow_same_bar_reversal) {
                                open_trade(Pos::Long, j, px);
                            }
                            continue;
                        }
                    }
                    else {
                        if (wants_reverse) {
                            const double px = fill_price_on_bar(close, open, j, settings.fill_timing);
                            close_trade(Pos::Short, j, px);
                            open_trade(Pos::Long, j, px);
                            continue;
                        }
                        if (xs) {
                            const double px = fill_price_on_bar(close, open, j, settings.fill_timing);
                            close_trade(Pos::Short, j, px);
                            continue;
                        }
                    }
                }

                if (pos == Pos::Flat) {
                    if (el) {
                        open_trade(Pos::Long, j, fill_price_on_bar(close, open, j, settings.fill_timing));
                    }
                    else if (es) {
                        open_trade(Pos::Short, j, fill_price_on_bar(close, open, j, settings.fill_timing));
                    }
                }
            }
        }
        else {
            for (std::size_t i = 0; i < n; ++i) {
                if (cancel_requested && cancel_requested()) {
                    err = "execution canceled";
                    return false;
                }
                const bool gate = (signals.allow_trade[i] != 0u);
                const bool el = gate && (signals.entry_long[i] != 0u);
                const bool xl = (signals.exit_long[i] != 0u);
                const bool es = gate && allow_short && (signals.entry_short[i] != 0u);
                const bool xs = allow_short && (signals.exit_short[i] != 0u);

                if (pos == Pos::Long) {
                    const bool wants_reverse = allow_short && es;
                    if (settings.exits_first) {
                        if (xl || wants_reverse) {
                            const double px = fill_price(close, open, i, settings.fill_timing);
                            close_trade(Pos::Long, i, px);
                            if (wants_reverse && settings.allow_same_bar_reversal) {
                                open_trade(Pos::Short, i, px);
                            }
                            continue;
                        }
                    }
                    else {
                        if (wants_reverse) {
                            const double px = fill_price(close, open, i, settings.fill_timing);
                            close_trade(Pos::Long, i, px);
                            open_trade(Pos::Short, i, px);
                            continue;
                        }
                        if (xl) {
                            const double px = fill_price(close, open, i, settings.fill_timing);
                            close_trade(Pos::Long, i, px);
                            continue;
                        }
                    }
                }
                else if (pos == Pos::Short) {
                    const bool wants_reverse = el;
                    if (settings.exits_first) {
                        if (xs || wants_reverse) {
                            const double px = fill_price(close, open, i, settings.fill_timing);
                            close_trade(Pos::Short, i, px);
                            if (wants_reverse && settings.allow_same_bar_reversal) {
                                open_trade(Pos::Long, i, px);
                            }
                            continue;
                        }
                    }
                    else {
                        if (wants_reverse) {
                            const double px = fill_price(close, open, i, settings.fill_timing);
                            close_trade(Pos::Short, i, px);
                            open_trade(Pos::Long, i, px);
                            continue;
                        }
                        if (xs) {
                            const double px = fill_price(close, open, i, settings.fill_timing);
                            close_trade(Pos::Short, i, px);
                            continue;
                        }
                    }
                }

                if (pos == Pos::Flat) {
                    if (el) {
                        open_trade(Pos::Long, i, fill_price(close, open, i, settings.fill_timing));
                    }
                    else if (es) {
                        open_trade(Pos::Short, i, fill_price(close, open, i, settings.fill_timing));
                    }
                }
            }
        }

        if (settings.close_out_at_end && pos != Pos::Flat) {
            const std::size_t last = n - 1;
            const double px = close[last];
            if (pos == Pos::Long) {
                close_trade(Pos::Long, last, px);
            }
            else if (pos == Pos::Short) {
                close_trade(Pos::Short, last, px);
            }
        }

        out.trade_hash = (out.trades > 0) ? h : 0ULL;
        return true;
    }

    BacktestResult StrategyExecutor::run(
        const DataView& dv,
        const ColumnRef& primary,
        const StrategyPlan& plan,
        const ExecSettings& settings) const {
        BacktestResult out{};
        std::string err;
        run(dv, primary, plan, settings, out, err, nullptr);
        return out;
    }

} // namespace sugar
