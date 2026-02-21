#include "strategy_menu.h"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "utils.h"
#include "plan_sweep.h"

namespace sugar {

    StrategyTemplate prompt_strategy_template(std::istream& in, std::ostream& out) {
        out << "\nChoose strategy template:\n";
#if SUGAR_HAS_DONCHIAN
        out << "  1) Donchian breakout\n";
#else
        out << "  1) Donchian breakout (not in this build)\n";
#endif
        out << "  2) Gated diff cross\n"; // Should be in all builds
#if SUGAR_HAS_ZSCORE
        out << "  3) ZScore mean reversion\n";
#else
        out << "  3) ZScore mean reversion (not in this build)\n";
#endif
#if SUGAR_HAS_SWING
        out << "  4) Swing breakout validation\n";
#else
        out << "  4) Swing breakout validation (not in this build)\n";
#endif
        out << "  99) Custom User Strategy (PlanSpec JSON)\n";
        out << "Selection [default: 2]: ";

        std::string line;
        std::getline(in, line);

        int choice = 2;
        if (!line.empty()) {
            std::istringstream iss(line);
            iss >> choice;
        }
        if (choice != 99 && (choice < 1 || choice > 4)) choice = 2;
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
#if SUGAR_HAS_DONCHIAN
        sugar::prompt_donchian_sweep_ranges(in, out, opt.donchian_ranges);
        opt.donchian_exec.eval_mode = prompt_bar_eval_mode(in, out, opt.donchian_exec.eval_mode);
#else
        (void)in; (void)out; (void)opt;
#endif
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
#if SUGAR_HAS_ZSCORE
        sugar::prompt_zscore_mean_reversion_sweep_ranges(in, out, opt.zscore_ranges);
        opt.zscore_exec.eval_mode = prompt_bar_eval_mode(in, out, opt.zscore_exec.eval_mode);
#else
        (void)in; (void)out; (void)opt;
#endif
    }

    void prompt_swing_breakout_validation_sweep(std::istream& in,
        std::ostream& out,
        StrategyRunOptions& opt)
    {
#if SUGAR_HAS_SWING
        sugar::prompt_swing_breakout_validation_sweep_ranges(in, out, opt.swing_ranges);
        opt.swing_exec.eval_mode = prompt_bar_eval_mode(in, out, opt.swing_exec.eval_mode);
#else
        (void)in; (void)out; (void)opt;
#endif
    }

    void prompt_custom_user_planspec(std::istream& in,
        std::ostream& out,
        StrategyRunOptions& opt)
    {
        out << "\nCustom User Strategy (PlanSpec JSON)\n";
        out << "Enter path to PlanSpec JSON: ";
        {
            std::string line;
            std::getline(in, line);
            opt.plan_spec_path = sugar::normalize_path_for_runtime(std::move(line));
        }

        // If no file provided, offer to print the operator registry and cancel cleanly.
        if (opt.plan_spec_path.empty()) {
            out << "No PlanSpec file provided.\n";
            out << "Print operator registry (same as --dump-ops)? (y/N): ";
            std::string line;
            std::getline(in, line);
            if (!line.empty() && (line[0] == 'y' || line[0] == 'Y')) {
                const auto reg = make_default_registry();
                out << "\n" << reg.describe_text(true /*include_modules_header*/) << "\n";
            }
            // Leave plan_spec_path empty to signal cancellation.
            return;
        }

        out << "Override trade mode? (y/N): ";
        std::string line;
        std::getline(in, line);
        opt.override_mode = (!line.empty() && (line[0] == 'y' || line[0] == 'Y'));
        if (opt.override_mode) {
            opt.custom_mode = prompt_trade_mode(in, out, opt.custom_mode);
        }

        out << "Override eval mode from PlanSpec file? (y/N): ";
        std::getline(in, line);
        opt.override_custom_eval_mode = (!line.empty() && (line[0] == 'y' || line[0] == 'Y'));
        if (opt.override_custom_eval_mode) {
            opt.custom_exec.eval_mode = prompt_bar_eval_mode(in, out, opt.custom_exec.eval_mode);
        }
    }

    bool run_strategy_template(const CandleSeries& series,
        const StrategyRunOptions& opt,
        std::ostream& out) {
        switch (opt.templ) {

        case StrategyTemplate::Donchian_Breakout:
#if SUGAR_HAS_DONCHIAN
            return run_donchian_breakout_sweep_best(
                series,
                opt.donchian_ranges,
                opt.mode,
                opt.donchian_exec,
                out
            );
#else
            out << "Donchian breakout is not available in this build.\n";
            return false;
#endif

        case StrategyTemplate::Gated_Diff_Cross:
            return run_gated_diff_cross_sweep_best(
                series,
                opt.gated_diff_ranges,
                opt.mode,
                opt.gated_diff_exec,
                out
            );

        case StrategyTemplate::ZScore_MeanReversion:
#if SUGAR_HAS_ZSCORE
            return run_zscore_mean_reversion_sweep_best(
                series,
                opt.zscore_ranges,
                opt.mode,
                opt.zscore_exec,
                out
            );
#else
            out << "ZScore mean reversion is not available in this build.\n";
            return false;
#endif

        case StrategyTemplate::Swing_Breakout_Validation:
#if SUGAR_HAS_SWING
            return run_swing_breakout_validation_sweep_best(
                series,
                opt.swing_ranges,
                opt.swing_exec,
                out
            );
#else
            out << "Swing breakout validation is not available in this build.\n";
            return false;
#endif

        case StrategyTemplate::Custom_User_PlanSpec: {
            if (opt.plan_spec_path.empty()) {
                out << "[Info] Custom strategy cancelled (no PlanSpec JSON provided).\n";
                return true;
            }

            PlanSpec spec;
            std::string e;
            if (!load_plan_spec_file(opt.plan_spec_path, spec, e)) {
                out << "Failed to load PlanSpec: " << e << "\n";
                return false;
            }
            PlanSweepRuntimeOptions rt{};
            rt.override_mode = opt.override_mode;
            rt.custom_mode = opt.custom_mode;
            rt.override_eval_mode = opt.override_custom_eval_mode;
            rt.eval_mode = opt.custom_exec.eval_mode;
            return run_custom_plan_spec(series, spec, rt, out);
        }

        default:
            out << "Unknown strategy template.\n";
            return false;
        }
    }

} // namespace sugar
