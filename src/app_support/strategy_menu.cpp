#include "sweep/strategy_menu.h"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <sstream>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ingest/utils.h"
#include "sweep/plan_sweep.h"

namespace sugar {
    namespace {
        namespace fs = std::filesystem;

        std::string template_planspec_relpath(StrategyTemplate templ) {
            switch (templ) {
            case StrategyTemplate::Donchian_Breakout:
                return "graphs/templates/donchian_breakout.json";
            case StrategyTemplate::Gated_Diff_Cross:
                return "graphs/templates/gated_diff_cross.json";
            case StrategyTemplate::ZScore_MeanReversion:
                return "graphs/templates/zscore_mean_reversion.json";
            case StrategyTemplate::Swing_Breakout_Validation:
                return "graphs/templates/swing_breakout_validation.json";
            default:
                return {};
            }
        }

        std::string resolve_template_planspec_path(StrategyTemplate templ) {
            const std::string rel = template_planspec_relpath(templ);
            if (rel.empty()) {
                return {};
            }

            std::error_code ec;
            const fs::path rel_path(rel);
            if (fs::exists(rel_path, ec)) {
                return sugar::normalize_path_for_runtime(rel_path.string());
            }

#ifdef SUGARBOT_SOURCE_DIR
            const fs::path configured_path = fs::path(SUGARBOT_SOURCE_DIR) / rel_path;
            ec.clear();
            if (fs::exists(configured_path, ec)) {
                return sugar::normalize_path_for_runtime(configured_path.string());
            }
#endif

            return sugar::normalize_path_for_runtime(rel);
        }
    } // namespace

    StrategySource prompt_strategy_source(std::istream& in, std::ostream& out) {
        out << "\nChoose strategy source:\n";
        out << "  1) Strategy template list\n";
        out << "  2) Custom User Strategy (PlanSpec JSON)\n";
        out << "Selection [default: 1]: ";

        std::string line;
        std::getline(in, line);

        int source = 1;
        if (!line.empty()) {
            std::istringstream iss(line);
            iss >> source;
        }
        if (source == 2 || source == 99) {
            return StrategySource::CustomPlanSpec;
        }
        return StrategySource::TemplateList;
    }

    StrategyTemplate prompt_strategy_template(std::istream& in, std::ostream& out) {
        out << "\nChoose strategy template:\n";
        out << "  1) Donchian breakout\n";
        out << "  2) Gated diff cross\n";
        out << "  3) ZScore mean reversion\n";
        out << "  4) Swing breakout validation\n";
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
        opt.donchian_exec.eval_mode = prompt_bar_eval_mode(in, out, opt.donchian_exec.eval_mode);
    }

    void prompt_gated_diff_cross_sweep(std::istream& in,
        std::ostream& out,
        StrategyRunOptions& opt)
    {
        opt.gated_diff_exec.eval_mode = prompt_bar_eval_mode(in, out, opt.gated_diff_exec.eval_mode);
    }

    void prompt_zscore_mean_reversion_sweep_ranges(std::istream& in,
        std::ostream& out,
        StrategyRunOptions& opt)
    {
        opt.zscore_exec.eval_mode = prompt_bar_eval_mode(in, out, opt.zscore_exec.eval_mode);
    }

    void prompt_swing_breakout_validation_sweep(std::istream& in,
        std::ostream& out,
        StrategyRunOptions& opt)
    {
        opt.swing_exec.eval_mode = prompt_bar_eval_mode(in, out, opt.swing_exec.eval_mode);
    }

    void prompt_custom_user_planspec(std::istream& in,
        std::ostream& out,
        CustomPlanSpecRunOptions& opt)
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
            opt.mode = prompt_trade_mode(in, out, opt.mode);
        }

        out << "Override eval mode from PlanSpec file? (y/N): ";
        std::getline(in, line);
        opt.override_eval_mode = (!line.empty() && (line[0] == 'y' || line[0] == 'Y'));
        if (opt.override_eval_mode) {
            opt.exec.eval_mode = prompt_bar_eval_mode(in, out, opt.exec.eval_mode);
        }
    }

    bool load_custom_planspec_runtime(
        const CustomPlanSpecRunOptions& opt,
        PlanSpec& out_spec,
        PlanSweepRuntimeOptions& out_rt,
        std::string& err) {
        out_spec = PlanSpec{};
        out_rt = PlanSweepRuntimeOptions{};

        if (opt.plan_spec_path.empty()) {
            err = "no PlanSpec JSON provided";
            return false;
        }
        if (!load_plan_spec_file(opt.plan_spec_path, out_spec, err)) {
            return false;
        }

        out_rt.override_mode = opt.override_mode;
        out_rt.custom_mode = opt.mode;
        out_rt.override_eval_mode = opt.override_eval_mode;
        out_rt.eval_mode = opt.exec.eval_mode;
        return true;
    }

    bool load_template_planspec(StrategyTemplate templ,
        PlanSpec& out_spec,
        std::string& err) {
        const std::string path = resolve_template_planspec_path(templ);
        if (path.empty()) {
            err = "template has no default PlanSpec mapping";
            return false;
        }

        if (!load_plan_spec_file(path, out_spec, err)) {
            err = "failed to load template PlanSpec '" + path + "': " + err;
            return false;
        }
        return true;
    }

    bool run_strategy_template(const DataView& dv,
        const ColumnRef& primary,
        StrategyTemplate templ,
        const StrategyRunOptions& opt,
        std::ostream& out) {
        if (!dv.has_col(primary)) {
            out << "Runtime data error: primary series/column is missing from DataView.\n";
            return false;
        }

        const auto& b = dv.bundle();
        const std::size_t si = b.find_series_index(primary.series);
        if (si == static_cast<std::size_t>(-1) || si >= b.frames.size()) {
            out << "Runtime data error: primary series '" << primary.series << "' is missing.\n";
            return false;
        }

        switch (templ) {
        case StrategyTemplate::Donchian_Breakout:
        case StrategyTemplate::Gated_Diff_Cross:
        case StrategyTemplate::ZScore_MeanReversion:
        case StrategyTemplate::Swing_Breakout_Validation:
        {
            PlanSpec spec{};
            std::string e;
            if (!load_template_planspec(templ, spec, e)) {
                out << "Template PlanSpec error: " << e << "\n";
                return false;
            }

            PlanSweepRuntimeOptions rt{};
            rt.override_mode = true;
            rt.custom_mode = opt.mode;

            // Keep per-template eval-mode prompts behavior while routing execution through PlanSpec.
            switch (templ) {
            case StrategyTemplate::Donchian_Breakout:
                rt.override_eval_mode = true;
                rt.eval_mode = opt.donchian_exec.eval_mode;
                break;
            case StrategyTemplate::Gated_Diff_Cross:
                rt.override_eval_mode = true;
                rt.eval_mode = opt.gated_diff_exec.eval_mode;
                break;
            case StrategyTemplate::ZScore_MeanReversion:
                rt.override_eval_mode = true;
                rt.eval_mode = opt.zscore_exec.eval_mode;
                break;
            case StrategyTemplate::Swing_Breakout_Validation:
                rt.override_eval_mode = true;
                rt.eval_mode = opt.swing_exec.eval_mode;
                break;
            default:
                break;
            }

            return run_custom_plan_spec(dv, primary, spec, rt, out, nullptr);
        }

        default:
            out << "Unknown strategy template.\n";
            return false;
        }
    }

} // namespace sugar
