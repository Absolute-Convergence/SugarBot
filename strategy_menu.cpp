#include "strategy_menu.h"

#include <iostream>
#include <sstream>
#include <string>

namespace sugar {

    StrategyTemplate prompt_strategy_template(std::istream& in, std::ostream& out) {
        out << "\nChoose strategy template:\n";
        out << "  1) Donchian breakout: \n";
        out << "  2) Gated diff cross: \n";
        out << "  3) ZScore mean reversion:\n";
        out << "  4) Swing_breakout_validation:\n";
        out << "Selection [default: 2]: ";

        std::string line;
        std::getline(in, line);

        int choice = 2;
        if (!line.empty()) {
            std::istringstream iss(line);
            iss >> choice;
        }
        if (choice < 1 || choice > 4) choice = 2;
        return static_cast<StrategyTemplate>(choice);
    }

    static BarEvalMode prompt_bar_eval_mode(std::istream& in,
        std::ostream& out,
        BarEvalMode def)
    {
        out << "\nExecutor timing model:\n";
        out << "  1) SameBar  (evaluate & execute on the same bar)\n";
        out << "  2) NextBar  (evaluate on bar close, execute on next bar)\n";
        out << "Selection [default: "
            << ((def == BarEvalMode::SameBar) ? "1" : "2")
            << "]: ";

        std::string line;
        std::getline(in, line);

        int choice = (def == BarEvalMode::SameBar) ? 1 : 2;
        if (!line.empty()) {
            std::istringstream iss(line);
            iss >> choice;
        }
        return (choice == 2) ? BarEvalMode::NextBar : BarEvalMode::SameBar;
    }

    TradeMode prompt_trade_mode(std::istream& in,
        std::ostream& out,
        TradeMode def)
    {
        out << "\nPosition behavior:\n";
        out << "  1) LongOnly\n";
        out << "  2) LongShort (stop-and-reverse)\n";
        out << "Selection [default: " << ((def == TradeMode::LongOnly) ? "1" : "2") << "]: ";

        std::string line;
        std::getline(in, line);

        int choice = (def == TradeMode::LongOnly) ? 1 : 2;
        if (!line.empty()) {
            std::istringstream iss(line);
            iss >> choice;
        }

        return (choice == 1) ? TradeMode::LongOnly : TradeMode::LongShort;
    }

    void prompt_donchian_sweep(std::istream& in,
        std::ostream& out,
        StrategyRunOptions& opt)
    {
        sugar::prompt_donchian_sweep_ranges(in, out, opt.donchian_ranges);
        opt.donchian_exec.eval_mode = prompt_bar_eval_mode(in, out, opt.donchian_exec.eval_mode);
    }

    void prompt_gated_diff_cross_sweep(std::istream& in,
        std::ostream& out,
        StrategyRunOptions& opt)
    {
        sugar::prompt_gated_diff_cross_sweep_ranges(in, out, opt.gated_diff_ranges);
        opt.gated_diff_exec.eval_mode = prompt_bar_eval_mode(in, out, opt.gated_diff_exec.eval_mode);
    }

    void prompt_zscore_mean_reversion_sweep_ranges(std::istream& in,
        std::ostream& out, 
        StrategyRunOptions& opt)
    {
        sugar::prompt_zscore_mean_reversion_sweep_ranges(in, out, opt.zscore_ranges);
        opt.zscore_exec.eval_mode = prompt_bar_eval_mode(in, out, opt.zscore_exec.eval_mode);
    }

    void prompt_swing_breakout_validation_sweep(std::istream& in,
        std::ostream& out,
        StrategyRunOptions& opt)
    {
        sugar::prompt_swing_breakout_validation_sweep_ranges(in, out, opt.swing_ranges);
        opt.swing_exec.eval_mode = prompt_bar_eval_mode(in, out, opt.swing_exec.eval_mode);
    }

    bool run_strategy_template(const CandleSeries& series,
        const StrategyRunOptions& opt,
        std::ostream& out) {
        switch (opt.templ) {

        case StrategyTemplate::Donchian_Breakout:
            return run_donchian_breakout_sweep_best(
                series,
                opt.donchian_ranges,
                opt.mode,
                opt.donchian_exec,
                out
            );

        case StrategyTemplate::Gated_Diff_Cross:
            return run_gated_diff_cross_sweep_best(
                series,
                opt.gated_diff_ranges,
                opt.mode,
                opt.gated_diff_exec,
                out
            );

        case StrategyTemplate::ZScore_MeanReversion:
            return run_zscore_mean_reversion_sweep_best(
                series,
                opt.zscore_ranges,
                opt.mode,
                opt.zscore_exec,
                out
            );

        case StrategyTemplate::Swing_Breakout_Validation:
            return run_swing_breakout_validation_sweep_best(
                series,
                opt.swing_ranges,
                opt.swing_exec,
                out
            );

        default:
            out << "Unknown strategy template.\n";
            return false;
        }
    }

} // namespace sugar
