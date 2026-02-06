#include "strategy_donchian_breakout.h"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <vector>

#include "indicators_composite_bool.h"
#include "indicators_composite_numeric.h"
#include "strategy_executor.h"
#include "utils.h"

namespace sugar {

    namespace {

        // Rolling max/min warmup is (n-1). We shift bands by 1 to avoid lookahead.
        // CrossSignal uses i-1, so a safe first valid index is period (=(period-1)+1).
        inline std::size_t warmup_index_for_plan(const DonchianBreakoutOptions& opt) {
            return opt.period;
        }

        inline IndicatorPtr make_exit_band(IndicatorPtr upper, IndicatorPtr lower, DonchianOutput exit_band) {
            // IMPORTANT: callers pass shared_ptr copies, so moving is fine.
            switch (exit_band) {
            case DonchianOutput::Upper:
                return upper;
            case DonchianOutput::Lower:
                return lower;
            case DonchianOutput::Middle:
            default: {
                // middle = (upper + lower) / 2
                auto mid = map_mul_const(map_add(std::move(upper), std::move(lower)), 0.5);
                return mid;
            }
            }
        }

    } // namespace

    // ------------------------------------------------------------
    // Plan builder
    // ------------------------------------------------------------

    StrategyPlan build_donchian_breakout_plan(const DonchianBreakoutOptions& opt) {
        // Base price series
        IndicatorPtr close = std::make_shared<CloseIndicator>();

        // Donchian bands (rolling extremes on close)
        // NOTE: map_rolling_max/min include the current bar, so we SHIFT by 1 before using
        // them as event boundaries.
        IndicatorPtr upper_raw = map_rolling_max(std::make_shared<CloseIndicator>(), opt.period);
        IndicatorPtr lower_raw = map_rolling_min(std::make_shared<CloseIndicator>(), opt.period);

        IndicatorPtr upper_prev = map_shift(IndicatorPtr(upper_raw), 1);
        IndicatorPtr lower_prev = map_shift(IndicatorPtr(lower_raw), 1);

        IndicatorPtr exit_raw = make_exit_band(IndicatorPtr(upper_raw), IndicatorPtr(lower_raw), opt.exit_band);
        IndicatorPtr exit_prev = map_shift(std::move(exit_raw), 1);

        // Events (using lagged boundaries)
        BoolSignalPtr long_enter = cross_up(IndicatorPtr(close), IndicatorPtr(upper_prev));
        BoolSignalPtr long_exit = cross_down(IndicatorPtr(close), IndicatorPtr(exit_prev));
        BoolSignalPtr short_enter = cross_down(IndicatorPtr(close), IndicatorPtr(lower_prev));
        BoolSignalPtr short_exit = cross_up(IndicatorPtr(close), IndicatorPtr(exit_prev));

        StrategyPlan plan{};
        plan.name = "DonchianBreakout";
        plan.mode = opt.mode;

        plan.signals.entry_long = std::move(long_enter);
        plan.signals.exit_long = std::move(long_exit);
        plan.signals.entry_short = std::move(short_enter);
        plan.signals.exit_short = std::move(short_exit);

        plan.first_valid_index = warmup_index_for_plan(opt);
        return plan;
    }

    BacktestResult run_donchian_breakout(const CandleSeries& series,
        const DonchianBreakoutOptions& opt)
    {
        StrategyPlan plan = build_donchian_breakout_plan(opt);
        StrategyExecutor exec;
        return exec.run(series, plan, opt.exec);
    }

    // ------------------------------------------------------------
    // Sweep support
    // ------------------------------------------------------------

    std::string donchian_params_to_string(const DonchianParams& p) {
        std::ostringstream oss;
        oss << "period=" << p.period
            << " mode=" << ((p.mode == TradeMode::LongOnly) ? "Long" : "LongShort")
            << " exit=";

        switch (p.exit_band) {
        case DonchianOutput::Upper: oss << "Upper"; break;
        case DonchianOutput::Lower: oss << "Lower"; break;
        case DonchianOutput::Middle:
        default: oss << "Middle"; break;
        }
        return oss.str();
    }

    void prompt_donchian_sweep_ranges(std::istream& in,
        std::ostream& out,
        DonchianSweepRanges& ranges)
    {
        out << "\n--- Donchian Breakout sweep ranges ---\n";
        prompt_range(in, out, "Period (min max step)", ranges.period_min, ranges.period_max, ranges.period_step);
        // Trade-mode selection is done at the menu level. These booleans can still be
        // toggled programmatically (e.g., by the executor) if you want a mode-filtered sweep.
    }

    TopK<DonchianParams> sweep_donchian_breakout_topk(const CandleSeries& data,
        const DonchianSweepRanges& ranges,
        const ExecSettings& exec)
    {
        auto eval = [&](const CandleSeries& srs, const DonchianParams& p) -> BacktestResult {
            DonchianBreakoutOptions opt{};
            opt.period = p.period;
            opt.exit_band = p.exit_band;
            opt.mode = p.mode;
            opt.exec = exec;
            return run_donchian_breakout(srs, opt);
            };

        auto score_fn = [&](const BacktestResult& r) -> double {
            return default_sweep_score(r, ranges.score_policy);
            };

        const auto periods = make_range(ranges.period_min, ranges.period_max, ranges.period_step);

        std::vector<DonchianParams> grid;
        grid.reserve(periods.size() * 2);

        for (auto n : periods) {
            if (n == 0) continue;
            if (ranges.include_long_only) {
                grid.push_back(DonchianParams{ n, TradeMode::LongOnly, ranges.exit_band });
            }
            if (ranges.include_long_short) {
                grid.push_back(DonchianParams{ n, TradeMode::LongShort, ranges.exit_band });
            }
        }

        return sweep_topk<DonchianParams>(data, grid, eval, score_fn, ranges.top_k);
    }

    bool run_donchian_breakout_sweep_best(const CandleSeries& data,
        const DonchianSweepRanges& ranges,
        TradeMode run_mode,
        const ExecSettings& exec,
        std::ostream& out)
    {
        // Apply runtime mode filter (menu-selected mode).
        auto filtered = ranges;
        if (run_mode == TradeMode::LongOnly) {
            filtered.include_long_only = true;
            filtered.include_long_short = false;
        }
        else {
            filtered.include_long_only = false;
            filtered.include_long_short = true;
        }

        out << "\n[Donchian Breakout Sweep]\n";
        out << "  period: " << filtered.period_min << " -> " << filtered.period_max << " step " << filtered.period_step << "\n";
        out << "  modes: "
            << (filtered.include_long_only ? "LongOnly " : "")
            << (filtered.include_long_short ? "LongShort" : "")
            << "\n";
        out << "  score: pnl - " << filtered.score_policy.dd_penalty << "*dd"
            << " (min_trades=" << filtered.score_policy.min_trades << ")\n";
        out << "  top_k: " << filtered.top_k << "\n";

        auto topk = sweep_donchian_breakout_topk(data, filtered, exec);
        auto best = topk.sorted_best_first();

        if (best.empty()) {
            out << "\nNo sweep candidates produced a finite score.\n";
            return false;
        }

        out << "\nTop candidates:\n";
        const std::size_t show = std::min<std::size_t>(best.size(), filtered.top_k);
        for (std::size_t i = 0; i < show; ++i) {
            out << "\n  #" << (i + 1)
                << " score=" << best[i].score
                << " | " << donchian_params_to_string(best[i].params) << "\n";
            print_backtest_summary(best[i].result, out, "    ");
        }

        auto winner = best.front().params;
        winner.mode = run_mode;

        DonchianBreakoutOptions opt{};
        opt.period = winner.period;
        opt.exit_band = winner.exit_band;
        opt.mode = winner.mode;
        opt.exec = exec;

        out << "\nBest Donchian Breakout (re-run): " << donchian_params_to_string(winner) << "\n";
        const auto final_res = run_donchian_breakout(data, opt);
        print_backtest_summary(final_res, out, "  ");

        return true;
    }

} // namespace sugar
