#include "app_interactive.h"

#include <iostream>
#include <limits>
#include <string>

#include "app_reporters.h"
#include "app_runtime.h"

#include "runtime_graph_preview.h"
#include "strategy_menu.h"
#include "utils.h"

namespace {

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

    std::string resolve_interactive_csv_path(const sugar::app::InteractiveCommand& cmd,
        std::istream& in,
        std::ostream& out)
    {
        std::string csv_path = cmd.csv_path.empty() ?
            sugar::app::default_csv_path_string() :
            cmd.csv_path;
        csv_path = sugar::normalize_path_for_runtime(std::move(csv_path));

#ifndef _DEBUG
        out << "CSV file path [default: " << csv_path << "]: \n"
            << "Enter new path to change default -> ";
        std::string line;
        std::getline(in, line);
        if (!line.empty()) {
            csv_path = sugar::normalize_path_for_runtime(std::move(line));
        }
#else
        (void)in;
        (void)out;
#endif

        return csv_path;
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

} // namespace

namespace sugar::app {

    int run_interactive_command(const AppCommand& cmd, std::istream& in, std::ostream& out, std::ostream& err) {
        const sugar::StrategySource strategy_source = sugar::prompt_strategy_source(in, out);
        if (strategy_source == sugar::StrategySource::CustomPlanSpec) {
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
                const std::string csv_path = resolve_interactive_csv_path(cmd.interactive, in, out);

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

        const std::string csv_path = resolve_interactive_csv_path(cmd.interactive, in, out);

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
