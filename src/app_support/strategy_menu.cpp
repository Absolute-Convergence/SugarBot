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

        bool parse_numbered_choice(const std::string& line, int min_choice, int max_choice, int& choice) {
            std::istringstream iss(line);
            int parsed = 0;
            if (!(iss >> parsed)) {
                return false;
            }
            std::string trailing;
            if (iss >> trailing) {
                return false;
            }
            if (parsed < min_choice || parsed > max_choice) {
                return false;
            }
            choice = parsed;
            return true;
        }

        SweepParamRange make_int_range(std::int64_t min_value,
            std::int64_t max_value,
            std::int64_t step)
        {
            SweepParamRange range{};
            range.min = static_cast<double>(min_value);
            range.max = static_cast<double>(max_value);
            range.step = static_cast<double>(step);
            range.is_int = true;
            return range;
        }

        SweepParamRange make_float_values(std::initializer_list<double> values) {
            SweepParamRange range{};
            range.values.assign(values.begin(), values.end());
            range.is_int = false;
            return range;
        }

        RankingObjectiveSpec make_default_template_objective(const PlanSweepSpec& sweep) {
            RankingObjectiveSpec objective{};
            objective.primary.source = ObjectiveValueSource::ReportedMetric;
            objective.primary.name = "pnl";
            objective.direction = ObjectiveDirection::Maximize;
            if (sweep.objective == "pnl_dd") {
                ObjectivePenaltySpec penalty{};
                penalty.value.source = ObjectiveValueSource::ReportedMetric;
                penalty.value.name = "max_drawdown";
                penalty.weight = sweep.dd_weight;
                objective.penalties.push_back(std::move(penalty));
            }
            return objective;
        }

        PlanSweepSpec make_template_sweep_spec(
            StrategyTemplate templ,
            TemplateSweepPreset preset)
        {
            PlanSweepSpec sweep{};
            sweep.enabled = true;
            sweep.top_k = 5;
            sweep.objective = "pnl_dd";
            sweep.dd_weight = 0.25;
            sweep.engine = PlanSweepEngineKind::Exhaustive;

            switch (preset) {
            case TemplateSweepPreset::Focused:
                sweep.max_evals = 1000;
                break;
            case TemplateSweepPreset::Broad:
                sweep.max_evals = 25000;
                break;
            case TemplateSweepPreset::Standard:
            default:
                sweep.max_evals = 10000;
                break;
            }

            switch (templ) {
            case StrategyTemplate::Donchian_Breakout:
                switch (preset) {
                case TemplateSweepPreset::Focused:
                    sweep.params["n"] = make_int_range(5, 500, 1);
                    break;
                case TemplateSweepPreset::Broad:
                    sweep.params["n"] = make_int_range(5, 20000, 1);
                    break;
                case TemplateSweepPreset::Standard:
                default:
                    sweep.params["n"] = make_int_range(5, 3500, 1);
                    break;
                }
                break;

            case StrategyTemplate::Gated_Diff_Cross:
                switch (preset) {
                case TemplateSweepPreset::Focused:
                    sweep.params["n_fast"] = make_int_range(3, 15, 1);
                    sweep.params["n_slow"] = make_int_range(20, 90, 2);
                    break;
                case TemplateSweepPreset::Broad:
                    sweep.params["n_fast"] = make_int_range(2, 60, 1);
                    sweep.params["n_slow"] = make_int_range(10, 400, 1);
                    break;
                case TemplateSweepPreset::Standard:
                default:
                    sweep.params["n_fast"] = make_int_range(3, 40, 1);
                    sweep.params["n_slow"] = make_int_range(20, 200, 1);
                    break;
                }
                break;

            case StrategyTemplate::ZScore_MeanReversion:
                switch (preset) {
                case TemplateSweepPreset::Focused:
                    sweep.params["n"] = make_int_range(5, 80, 5);
                    sweep.params["entry_z_long"] = make_float_values({ -0.75, -1.0, -1.25, -1.5, -1.75 });
                    sweep.params["entry_z_short"] = make_float_values({ 0.75, 1.0, 1.25, 1.5, 1.75 });
                    break;
                case TemplateSweepPreset::Broad:
                    sweep.params["n"] = make_int_range(5, 250, 1);
                    sweep.params["entry_z_long"] = make_float_values({ -0.25, -0.5, -0.75, -1.0, -1.25, -1.5, -1.75, -2.0, -2.25, -2.5 });
                    sweep.params["entry_z_short"] = make_float_values({ 0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0, 2.25, 2.5 });
                    break;
                case TemplateSweepPreset::Standard:
                default:
                    sweep.params["n"] = make_int_range(5, 150, 1);
                    sweep.params["entry_z_long"] = make_float_values({ -0.5, -0.75, -1.0, -1.25, -1.5, -1.75, -2.0, -2.25 });
                    sweep.params["entry_z_short"] = make_float_values({ 0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0, 2.25 });
                    break;
                }
                break;

            case StrategyTemplate::Swing_Breakout_Validation:
                switch (preset) {
                case TemplateSweepPreset::Focused:
                    sweep.params["n_break"] = make_int_range(5, 35, 2);
                    sweep.params["n_trend"] = make_int_range(20, 100, 5);
                    break;
                case TemplateSweepPreset::Broad:
                    sweep.params["n_break"] = make_int_range(5, 150, 1);
                    sweep.params["n_trend"] = make_int_range(60, 400, 2);
                    break;
                case TemplateSweepPreset::Standard:
                default:
                    sweep.params["n_break"] = make_int_range(5, 80, 1);
                    sweep.params["n_trend"] = make_int_range(40, 240, 5);
                    break;
                }
                break;

            default:
                break;
            }

            return sweep;
        }

        std::string format_template_sweep_param_names(const PlanSpecSweepDims& dims) {
            std::ostringstream oss;
            for (std::size_t i = 0; i < dims.names.size(); ++i) {
                if (i) {
                    oss << ", ";
                }
                oss << dims.names[i];
            }
            return oss.str();
        }

        std::size_t estimate_template_candidate_count(
            StrategyTemplate templ,
            TemplateSweepPreset preset)
        {
            const PlanSweepSpec sweep = make_template_sweep_spec(templ, preset);
            PlanSpecSweepDims dims{};
            std::string err;
            if (!build_planspec_sweep_dims(sweep, dims, err)) {
                return 0;
            }
            return build_planspec_sweep_points(dims, sweep.max_evals).size();
        }

        TemplateSweepPreset prompt_template_sweep_preset(
            std::istream& in,
            std::ostream& out,
            StrategyTemplate templ,
            TemplateSweepPreset def)
        {
            const auto focused_count = estimate_template_candidate_count(templ, TemplateSweepPreset::Focused);
            const auto standard_count = estimate_template_candidate_count(templ, TemplateSweepPreset::Standard);
            const auto broad_count = estimate_template_candidate_count(templ, TemplateSweepPreset::Broad);

            out << "\nSweep search profile:\n";
            out << "  1) Focused  (" << focused_count << " candidates; fast sanity sweep)\n";
            out << "  2) Standard (" << standard_count << " candidates; recommended)\n";
            out << "  3) Broad    (" << broad_count << " candidates; wider search)\n";
            out << "Selection [default: " << static_cast<int>(def) << "]: ";

            std::string line;
            std::getline(in, line);

            int choice = static_cast<int>(def);
            if (!line.empty()) {
                std::istringstream iss(line);
                iss >> choice;
            }

            if (choice == 1) {
                return TemplateSweepPreset::Focused;
            }
            if (choice == 3) {
                return TemplateSweepPreset::Broad;
            }
            return TemplateSweepPreset::Standard;
        }

        bool configure_template_sweep(
            StrategyTemplate templ,
            TemplateSweepPreset preset,
            PlanSpec& spec,
            std::ostream& out)
        {
            spec.sweep = make_template_sweep_spec(templ, preset);
            if (!spec.sweep.has_value()) {
                out << "Template sweep error: no sweep preset is available for this template.\n";
                return false;
            }

            PlanSpecSweepDims dims{};
            std::string err;
            if (!build_planspec_sweep_dims(*spec.sweep, dims, err)) {
                out << "Template sweep error: " << err << "\n";
                return false;
            }

            const auto points = build_planspec_sweep_points(dims, spec.sweep->max_evals);
            const auto total_combos = dims.total_combinations();
            const auto objective = make_default_template_objective(*spec.sweep);

            out << "[Info] template sweep configured: engine=exhaustive"
                << " objective=" << describe_ranking_objective(objective)
                << " top_k=" << spec.sweep->top_k
                << " candidate_space=" << points.size();
            if (total_combos != points.size()) {
                out << " of " << total_combos;
            }
            out << "\n";
            out << "[Info] sweep params: " << format_template_sweep_param_names(dims) << "\n";
            return true;
        }
    } // namespace

    StrategySource prompt_strategy_source(std::istream& in, std::ostream& out, bool include_artifact_run) {
        const int default_choice = include_artifact_run ? 3 : 1;
        const int max_choice = include_artifact_run ? 3 : 2;

        for (;;) {
            out << "\nChoose strategy source:\n";
            out << "  1) Strategy template list\n";
            out << "  2) Custom User Strategy (PlanSpec JSON)\n";
            if (include_artifact_run) {
                out << "  3) Run layout.json + logic.json + data.csv\n";
            }
            out << "Selection [default: " << default_choice << ", q to quit]: ";

            std::string line;
            std::getline(in, line);
            if (is_cancel_input(line)) {
                return StrategySource::Canceled;
            }

            int source = default_choice;
            if (!line.empty() && !parse_numbered_choice(line, 1, max_choice, source)) {
                out << "[Info] Invalid selection. Enter 1-" << max_choice << " or q to cancel.\n";
                continue;
            }
            if (source == 2) {
                return StrategySource::CustomPlanSpec;
            }
            if (include_artifact_run && source == 3) {
                return StrategySource::ArtifactRun;
            }
            return StrategySource::TemplateList;
        }
    }

    StrategyTemplate prompt_strategy_template(std::istream& in, std::ostream& out) {
        for (;;) {
            out << "\nChoose strategy template:\n";
            out << "  1) Donchian breakout\n";
            out << "  2) Gated diff cross\n";
            out << "  3) ZScore mean reversion\n";
            out << "  4) Swing breakout validation\n";
            out << "Selection [default: 2, q to quit]: ";

            std::string line;
            std::getline(in, line);
            if (is_cancel_input(line)) {
                return StrategyTemplate::Canceled;
            }

            int choice = 2;
            if (!line.empty() && !parse_numbered_choice(line, 1, 4, choice)) {
                out << "[Info] Invalid selection. Enter 1-4 or q to cancel.\n";
                continue;
            }
            return static_cast<StrategyTemplate>(choice);
        }
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
        opt.sweep_preset = prompt_template_sweep_preset(
            in,
            out,
            StrategyTemplate::Donchian_Breakout,
            opt.sweep_preset);
    }

    void prompt_gated_diff_cross_sweep(std::istream& in,
        std::ostream& out,
        StrategyRunOptions& opt)
    {
        opt.gated_diff_exec.eval_mode = prompt_bar_eval_mode(in, out, opt.gated_diff_exec.eval_mode);
        opt.sweep_preset = prompt_template_sweep_preset(
            in,
            out,
            StrategyTemplate::Gated_Diff_Cross,
            opt.sweep_preset);
    }

    void prompt_zscore_mean_reversion_sweep_ranges(std::istream& in,
        std::ostream& out,
        StrategyRunOptions& opt)
    {
        opt.zscore_exec.eval_mode = prompt_bar_eval_mode(in, out, opt.zscore_exec.eval_mode);
        opt.sweep_preset = prompt_template_sweep_preset(
            in,
            out,
            StrategyTemplate::ZScore_MeanReversion,
            opt.sweep_preset);
    }

    void prompt_swing_breakout_validation_sweep(std::istream& in,
        std::ostream& out,
        StrategyRunOptions& opt)
    {
        opt.swing_exec.eval_mode = prompt_bar_eval_mode(in, out, opt.swing_exec.eval_mode);
        opt.sweep_preset = prompt_template_sweep_preset(
            in,
            out,
            StrategyTemplate::Swing_Breakout_Validation,
            opt.sweep_preset);
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
            if (is_cancel_input(line)) {
                opt.plan_spec_path.clear();
                return;
            }
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
            if (!configure_template_sweep(templ, opt.sweep_preset, spec, out)) {
                return false;
            }

            PlanSweepRuntimeOptions rt{};
            rt.override_mode = true;
            rt.custom_mode = opt.mode;
            rt.ostream_progress_throttle_ms = 0;

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
