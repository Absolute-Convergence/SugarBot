#include "interactive/app_interactive.h"

#include <filesystem>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <utility>

#include "commands/app_commands.h"
#include "reporting/app_reporters.h"
#include "runtime/app_runtime.h"

#include "sweep/runtime_graph_preview.h"
#include "sweep/strategy_menu.h"
#include "ingest/utils.h"

namespace {

    namespace fs = std::filesystem;

    enum class InteractiveWorkflow {
        Canceled = 0,
        RunArtifacts = 1,
        ObsReport = 2,
        PlanReport = 3,
        FeatureDump = 4,
        AlignDump = 5,
        StrategyTemplate = 6,
        CustomPlanSpec = 7,
    };

    void maybe_pause_before_exit(std::istream& in, std::ostream& out) {
#ifndef _DEBUG
        out << "\nPress Enter to exit...";
        in.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        in.get();
#else
        (void)in;
        (void)out;
#endif
    }

    bool is_existing_file_path(const std::string& path) {
        std::error_code ec;
        const fs::path p(path);
        if (!fs::exists(p, ec) || ec) {
            return false;
        }
        ec.clear();
        return fs::is_regular_file(p, ec) && !ec;
    }

    void print_missing_file_prompt(std::ostream& out, const std::string& path) {
        out << "[Info] File not found: " << path
            << ". Enter an existing file path or q to cancel.\n";
    }

    bool resolve_interactive_csv_path(const sugar::app::InteractiveCommand& cmd,
        std::istream& in,
        std::ostream& out,
        std::string& csv_path)
    {
        csv_path = cmd.csv_path.empty() ?
            sugar::app::default_csv_path_string() :
            cmd.csv_path;
        csv_path = sugar::normalize_path_for_runtime(std::move(csv_path));

        for (;;) {
            out << "CSV file path [default: " << csv_path << "] (q to cancel): ";

            std::string line;
            if (!std::getline(in, line)) {
                return false;
            }
            if (sugar::is_cancel_input(line)) {
                return false;
            }

            std::string candidate = csv_path;
            if (!line.empty()) {
                candidate = sugar::normalize_path_for_runtime(std::move(line));
            }
            if (is_existing_file_path(candidate)) {
                csv_path = std::move(candidate);
                return true;
            }

            print_missing_file_prompt(out, candidate);
        }
    }

    void print_interactive_runtime_summary(const sugar::app::InteractiveCommand& cmd,
        const sugar::app::RuntimeSession& session,
        std::ostream& out)
    {
        for (const auto& notice : session.notices) {
            out << notice << "\n";
        }
        if (cmd.has_runtime_data_spec) {
            out << "[Info] frame-first ingestion active (--data-spec)\n";
        }
        else {
            out << "[Info] canonical ingestion active (legacy preset DataSpec)\n";
        }

        const sugar::DataView dv(*session.runtime.aligned_bundle);
        const auto capability_report = sugar::diagnose_observation_capabilities(
            sugar::make_observation_world_facts(session.runtime));
        out << "[Info] selected primary entity: " << session.runtime.primary_entity_key << "\n";
        out << "[Info] aligned calendar rows: " << dv.size() << "\n";

        sugar::app::print_frame_summary(session.runtime.aligned_bundle->frames[session.runtime.primary_series_index], out, "  ");
        sugar::print_runtime_graph_preview(
            dv,
            session.runtime.primary_ref,
            session.runtime.run_manifest,
            session.runtime.primary_input_path,
            out,
            48,
            &capability_report);
    }

    bool parse_menu_choice(const std::string& line, int min_choice, int max_choice, int& out_choice)
    {
        std::istringstream iss(line);
        int choice = 0;
        if (!(iss >> choice)) {
            return false;
        }
        std::string trailing;
        if (iss >> trailing) {
            return false;
        }
        if (choice < min_choice || choice > max_choice) {
            return false;
        }
        out_choice = choice;
        return true;
    }

    InteractiveWorkflow prompt_interactive_workflow(std::istream& in, std::ostream& out) {
        for (;;) {
            out << "\nChoose interactive workflow:\n";
            out << "  1) Run layout.json + logic.json + data.csv\n";
            out << "  2) Observation report\n";
            out << "  3) Plan report\n";
            out << "  4) Feature dump\n";
            out << "  5) Alignment dump\n";
            out << "  6) Strategy template list\n";
            out << "  7) Custom User Strategy (PlanSpec JSON)\n";
            out << "Selection [default: 1, q to quit]: ";

            std::string line;
            std::getline(in, line);
            if (sugar::is_cancel_input(line)) {
                return InteractiveWorkflow::Canceled;
            }
            if (line.empty()) {
                return InteractiveWorkflow::RunArtifacts;
            }

            int choice = 0;
            if (parse_menu_choice(line, 1, 7, choice)) {
                return static_cast<InteractiveWorkflow>(choice);
            }

            out << "[Info] Invalid selection. Enter 1-7 or q to cancel.\n";
        }
    }

    void print_equivalent_cli(std::ostream& out, const std::string& command) {
        out << "[Info] equivalent CLI: sugarbot " << command << "\n";
    }

    bool prompt_path(std::istream& in,
        std::ostream& out,
        const char* label,
        const std::string& default_value,
        std::string& path)
    {
        for (;;) {
            out << label;
            if (!default_value.empty()) {
                out << " [default: " << default_value << "]";
            }
            out << " (q to cancel): ";

            std::string line;
            if (!std::getline(in, line)) {
                return false;
            }
            if (sugar::is_cancel_input(line)) {
                return false;
            }
            std::string candidate;
            if (line.empty()) {
                if (default_value.empty()) {
                    out << "[Info] Path is required. Enter a path or q to cancel.\n";
                    continue;
                }
                candidate = sugar::normalize_path_for_runtime(default_value);
            }
            else {
                candidate = sugar::normalize_path_for_runtime(std::move(line));
            }

            if (!is_existing_file_path(candidate)) {
                print_missing_file_prompt(out, candidate);
                continue;
            }

            path = std::move(candidate);
            return true;
        }
    }

    bool prompt_required_text(std::istream& in,
        std::ostream& out,
        const char* label,
        std::string& value)
    {
        for (;;) {
            out << label << " (q to cancel): ";

            std::string line;
            if (!std::getline(in, line)) {
                return false;
            }
            if (sugar::is_cancel_input(line)) {
                return false;
            }
            sugar::trim_in_place(line);
            if (line.empty()) {
                out << "[Info] Value is required. Enter a value or q to cancel.\n";
                continue;
            }
            value = std::move(line);
            return true;
        }
    }

    int run_interactive_run_command(const sugar::app::InteractiveCommand& interactive,
        std::istream& in,
        std::ostream& out,
        std::ostream& err)
    {
        sugar::app::AppCommand run_cmd{};
        run_cmd.kind = sugar::app::CommandKind::Run;

        if (!prompt_path(in, out, "Layout JSON path", interactive.runtime_data_spec_path, run_cmd.run.layout_path) ||
            !prompt_path(in, out, "Logic/PlanSpec JSON path", {}, run_cmd.run.logic_path) ||
            !prompt_path(in, out, "CSV data path", interactive.csv_path, run_cmd.run.csv_path))
        {
            out << "[Info] Interactive file run cancelled.\n";
            return 0;
        }

        out << "[Info] interactive file run using existing noninteractive pipeline.\n";
        print_equivalent_cli(out, "run " + run_cmd.run.layout_path + " " + run_cmd.run.logic_path + " " + run_cmd.run.csv_path);
        return sugar::app::run_unified_cli_command(run_cmd, out, err);
    }

    int run_interactive_obs_report_command(const sugar::app::InteractiveCommand& interactive,
        std::istream& in,
        std::ostream& out,
        std::ostream& err)
    {
        sugar::app::AppCommand report_cmd{};
        report_cmd.kind = sugar::app::CommandKind::ObsReport;

        if (!prompt_path(in, out, "Layout JSON path", interactive.runtime_data_spec_path, report_cmd.obs_report.layout_path) ||
            !prompt_path(in, out, "CSV data path", interactive.csv_path, report_cmd.obs_report.csv_path))
        {
            out << "[Info] Interactive observation report cancelled.\n";
            return 0;
        }

        print_equivalent_cli(out, "--obs-report " + report_cmd.obs_report.layout_path + " " + report_cmd.obs_report.csv_path);
        return sugar::app::run_obs_report_command(report_cmd, out, err);
    }

    int run_interactive_plan_report_command(const sugar::app::InteractiveCommand& interactive,
        std::istream& in,
        std::ostream& out,
        std::ostream& err)
    {
        sugar::app::AppCommand report_cmd{};
        report_cmd.kind = sugar::app::CommandKind::PlanReport;

        if (!prompt_path(in, out, "Layout JSON path", interactive.runtime_data_spec_path, report_cmd.plan_report.layout_path) ||
            !prompt_path(in, out, "Logic/PlanSpec JSON path", {}, report_cmd.plan_report.logic_path) ||
            !prompt_path(in, out, "CSV data path", interactive.csv_path, report_cmd.plan_report.csv_path))
        {
            out << "[Info] Interactive plan report cancelled.\n";
            return 0;
        }

        print_equivalent_cli(out, "--plan-report " + report_cmd.plan_report.layout_path + " " +
            report_cmd.plan_report.logic_path + " " + report_cmd.plan_report.csv_path);
        return sugar::app::run_plan_report_command(report_cmd, out, err);
    }

    int run_interactive_feature_dump_command(const sugar::app::InteractiveCommand& interactive,
        std::istream& in,
        std::ostream& out,
        std::ostream& err)
    {
        sugar::app::AppCommand dump_cmd{};
        dump_cmd.kind = sugar::app::CommandKind::FeatureDump;

        if (!prompt_path(in, out, "Layout JSON path", interactive.runtime_data_spec_path, dump_cmd.feature_dump.layout_path) ||
            !prompt_path(in, out, "Logic/PlanSpec JSON path", {}, dump_cmd.feature_dump.logic_path) ||
            !prompt_path(in, out, "CSV data path", interactive.csv_path, dump_cmd.feature_dump.csv_path))
        {
            out << "[Info] Interactive feature dump cancelled.\n";
            return 0;
        }

        print_equivalent_cli(out, "--feature-dump " + dump_cmd.feature_dump.layout_path + " " +
            dump_cmd.feature_dump.logic_path + " " + dump_cmd.feature_dump.csv_path);
        return sugar::app::run_feature_dump_command(dump_cmd, out, err);
    }

    int run_interactive_align_dump_command(const sugar::app::InteractiveCommand& interactive,
        std::istream& in,
        std::ostream& out,
        std::ostream& err)
    {
        sugar::app::AppCommand dump_cmd{};
        dump_cmd.kind = sugar::app::CommandKind::AlignDump;
        dump_cmd.align_dump.calendar = sugar::align::CalendarPolicy::Intersection;

        if (!prompt_path(in, out, "Layout/DataSpec JSON path", interactive.runtime_data_spec_path, dump_cmd.align_dump.spec_path) ||
            !prompt_path(in, out, "CSV data path", interactive.csv_path, dump_cmd.align_dump.csv_path) ||
            !prompt_required_text(in, out, "Series key column", dump_cmd.align_dump.series_key_col))
        {
            out << "[Info] Interactive alignment dump cancelled.\n";
            return 0;
        }

        print_equivalent_cli(out, "--align-dump " + dump_cmd.align_dump.spec_path + " " +
            dump_cmd.align_dump.csv_path + " --series-key-col " + dump_cmd.align_dump.series_key_col);
        return sugar::app::run_align_dump_command(dump_cmd, out, err);
    }

} // namespace

namespace sugar::app {

    int run_interactive_command(const AppCommand& cmd, std::istream& in, std::ostream& out, std::ostream& err) {
        const InteractiveWorkflow workflow = prompt_interactive_workflow(in, out);
        if (workflow == InteractiveWorkflow::Canceled) {
            out << "[Info] Interactive command cancelled.\n";
            return 0;
        }

        switch (workflow) {
        case InteractiveWorkflow::RunArtifacts:
            return run_interactive_run_command(cmd.interactive, in, out, err);
        case InteractiveWorkflow::ObsReport:
            return run_interactive_obs_report_command(cmd.interactive, in, out, err);
        case InteractiveWorkflow::PlanReport:
            return run_interactive_plan_report_command(cmd.interactive, in, out, err);
        case InteractiveWorkflow::FeatureDump:
            return run_interactive_feature_dump_command(cmd.interactive, in, out, err);
        case InteractiveWorkflow::AlignDump:
            return run_interactive_align_dump_command(cmd.interactive, in, out, err);
        case InteractiveWorkflow::CustomPlanSpec:
        case InteractiveWorkflow::StrategyTemplate:
            break;
        case InteractiveWorkflow::Canceled:
            out << "[Info] Interactive command cancelled.\n";
            return 0;
        }

        if (workflow == InteractiveWorkflow::CustomPlanSpec) {
            sugar::CustomPlanSpecRunOptions custom_opt{};
            sugar::prompt_custom_user_planspec(in, out, custom_opt);
            if (custom_opt.plan_spec_path.empty()) {
                out << "[Info] Custom strategy cancelled (no PlanSpec JSON provided).\n";
                return 0;
            }

            sugar::PlanSpec spec{};
            sugar::PlanSweepRuntimeOptions rt{};
            bool has_embedded_observations = false;
            std::string spec_err;
            if (!load_custom_planspec_bundle(custom_opt, spec, rt, has_embedded_observations, spec_err)) {
                err << "Error: failed to load PlanSpec runtime: " << spec_err << "\n";
                return 1;
            }

            bool use_embedded_observations = false;
            if (has_embedded_observations) {
                out << "[Info] PlanSpec embeds legacy observation inputs. Interactive custom execution can use either those "
                    << "embedded observations or the current CSV/DataSpec input.\n";
                use_embedded_observations = sugar::prompt_yes_no(
                    in,
                    out,
                    "Use embedded legacy observations instead of interactive CSV/DataSpec input?",
                    true);
            }

            bool ran = false;
            if (use_embedded_observations) {
                AlignedRuntimeInput runtime{};
                std::string runtime_err;
                if (!build_embedded_observation_runtime(spec, runtime, runtime_err)) {
                    err << "Error: " << runtime_err << "\n";
                    return 1;
                }

                out << "[Info] custom PlanSpec execution using embedded legacy observations.\n";
                print_observation_capability_report(runtime, out);
                const auto plan_report = sugar::diagnose_plan_capabilities(spec, runtime);
                sugar::append_plan_capability_snapshot(plan_report, runtime.run_manifest);
                print_plan_capability_report(plan_report, out);
                const sugar::DataView dv(*runtime.aligned_bundle);
                ran = sugar::run_custom_plan_spec(dv, runtime.primary_ref, spec, rt, out);
            }
            else {
                std::string csv_path;
                if (!resolve_interactive_csv_path(cmd.interactive, in, out, csv_path)) {
                    out << "[Info] Custom strategy cancelled.\n";
                    return 0;
                }

                RuntimeSession session{};
                std::string runtime_err;
                if (!build_interactive_runtime_session(cmd.interactive, csv_path, session, runtime_err)) {
                    err << "Error: " << runtime_err << "\n";
                    return 1;
                }

                print_interactive_runtime_summary(cmd.interactive, session, out);
                const auto plan_report = sugar::diagnose_plan_capabilities(spec, session.runtime);
                sugar::append_plan_capability_snapshot(plan_report, session.runtime.run_manifest);
                print_plan_capability_report(plan_report, out);
                out << "[Info] custom PlanSpec execution using interactive CSV/DataSpec observations.\n";

                if (session.runtime.aligned_bundle->frames[session.runtime.primary_series_index].row_count() == 0) {
                    maybe_pause_before_exit(in, out);
                    return 0;
                }

                const sugar::DataView dv(*session.runtime.aligned_bundle);
                ran = sugar::run_custom_plan_spec(dv, session.runtime.primary_ref, spec, rt, out);
            }

            maybe_pause_before_exit(in, out);
            return ran ? 0 : 1;
        }

        const sugar::StrategyTemplate templ = sugar::prompt_strategy_template(in, out);
        if (templ == sugar::StrategyTemplate::Canceled) {
            out << "[Info] Interactive command cancelled.\n";
            return 0;
        }
        sugar::StrategyRunOptions opt{};
        opt.mode = sugar::prompt_trade_mode(in, out, sugar::TradeMode::LongShort);

#ifdef _DEBUG
        const bool customize = sugar::prompt_yes_no(in, out, "Customize sweep ranges?", true);
#else
        const bool customize = true;
#endif

        if (customize) {
            switch (templ) {
            case sugar::StrategyTemplate::Donchian_Breakout:
                sugar::prompt_donchian_sweep(in, out, opt);
                break;
            case sugar::StrategyTemplate::Gated_Diff_Cross:
                sugar::prompt_gated_diff_cross_sweep(in, out, opt);
                break;
            case sugar::StrategyTemplate::ZScore_MeanReversion:
                sugar::prompt_zscore_mean_reversion_sweep_ranges(in, out, opt);
                break;
            case sugar::StrategyTemplate::Swing_Breakout_Validation:
                sugar::prompt_swing_breakout_validation_sweep(in, out, opt);
                break;
            default:
                break;
            }
        }

        std::string csv_path;
        if (!resolve_interactive_csv_path(cmd.interactive, in, out, csv_path)) {
            out << "[Info] Interactive template execution cancelled.\n";
            return 0;
        }

        RuntimeSession session{};
        std::string runtime_err;
        if (!build_interactive_runtime_session(cmd.interactive, csv_path, session, runtime_err)) {
            err << "Error: " << runtime_err << "\n";
            return 1;
        }

        print_interactive_runtime_summary(cmd.interactive, session, out);
        PlanSpec template_spec{};
        std::string template_err;
        if (!load_template_planspec(templ, template_spec, template_err)) {
            err << "Error: template PlanSpec mapping failed: " << template_err << "\n";
            return 1;
        }
        const auto template_report = sugar::diagnose_plan_capabilities(template_spec, session.runtime);
        sugar::append_plan_capability_snapshot(template_report, session.runtime.run_manifest);
        print_plan_capability_report(template_report, out);
        const sugar::DataView dv(*session.runtime.aligned_bundle);

        if (session.runtime.aligned_bundle->frames[session.runtime.primary_series_index].row_count() == 0) {
            return 0;
        }

        const bool ran = sugar::run_strategy_template(dv, session.runtime.primary_ref, templ, opt, out);
        if (!ran) {
            out << "\n[Info] Selected template is not implemented yet.\n";
        }

        maybe_pause_before_exit(in, out);
        return 0;
    }

} // namespace sugar::app
