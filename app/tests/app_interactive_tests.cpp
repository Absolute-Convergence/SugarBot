#include "tests/app_test_support.h"

#include <sstream>
#include <string>

#include "commands/app_commands.h"
#include "interactive/app_interactive.h"

namespace sugar::app::tests {

    namespace {

        bool test_interactive_q_cancels_before_default_run(TestContext& t) {
            t.out << "[app_interactive] q cancels before default run\n";
            AppCommand cmd{};
            cmd.kind = CommandKind::Interactive;

            std::istringstream in("q\n");
            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_interactive_command(cmd, in, out, err);
            const std::string text = out.str();

            t.check(rc == 0, "interactive q command returns success");
            t.check(err.str().empty(), "interactive q command produces no error output");
            t.check(text.find("Interactive command cancelled") != std::string::npos,
                "interactive q reports cancellation");
            t.check(text.find("report-first path: 2) inspect data, 3) validate logic, 1) execute") != std::string::npos,
                "interactive menu advertises the report-first workflow");
            t.check(text.find("Observation report (ingest diagnostics + capability surfaces)") != std::string::npos,
                "interactive menu explains observation-report purpose");
            t.check(text.find("Run layout.json + logic.json + data.csv (final run/sweep/panel report)") != std::string::npos,
                "interactive menu explains final run reporting surface");
            t.check(text.find("PnL:") == std::string::npos,
                "interactive q does not fall through into default execution");
            return t.failed == 0;
        }

        bool test_interactive_invalid_menu_reprompts_then_cancels(TestContext& t) {
            t.out << "[app_interactive] invalid menu input reprompts\n";
            AppCommand cmd{};
            cmd.kind = CommandKind::Interactive;

            std::istringstream in("wat\nq\n");
            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_interactive_command(cmd, in, out, err);
            const std::string text = out.str();

            t.check(rc == 0, "interactive invalid menu cancellation returns success");
            t.check(err.str().empty(), "interactive invalid menu cancellation produces no error output");
            t.check(text.find("Invalid selection. Enter 1-7 or q to cancel.") != std::string::npos,
                "interactive invalid menu input is reported");
            t.check(text.find("Interactive command cancelled") != std::string::npos,
                "interactive invalid menu path can still cancel");
            t.check(text.find("PnL:") == std::string::npos,
                "interactive invalid menu does not fall through into execution");
            return t.failed == 0;
        }

        bool test_interactive_file_run_uses_unified_pipeline(TestContext& t) {
            t.out << "[app_interactive] file run uses unified pipeline\n";
            RunFixturePaths fixture{};
            std::string fixture_err;
            const bool fixture_ok = create_unified_single_series_fixture(fixture, fixture_err);
            t.check(fixture_ok, "interactive file-run fixture files are created");
            if (!fixture_ok) return false;

            AppCommand cmd{};
            cmd.kind = CommandKind::Interactive;

            std::istringstream in(
                std::string("\n")
                + fixture.layout_path + "\n"
                + fixture.logic_path + "\n"
                + fixture.csv_path + "\n");
            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_interactive_command(cmd, in, out, err);
            const std::string text = out.str();

            t.check(rc == 0, "interactive file run returns success");
            t.check(err.str().empty(), "interactive file run produces no error output");
            t.check(text.find("interactive file run using existing noninteractive pipeline") != std::string::npos,
                "interactive file run reports shared pipeline");
            t.check(text.find("equivalent CLI: sugarbot run") != std::string::npos,
                "interactive file run prints equivalent CLI command");
            t.check(text.find("noninteractive artifact run active") != std::string::npos,
                "interactive file run reaches unified run command");
            t.check(text.find("Plan compatibility") != std::string::npos,
                "interactive file run emits plan compatibility report");
            t.check(text.find("PnL:") != std::string::npos,
                "interactive file run executes the strategy");
            return t.failed == 0;
        }

        bool test_interactive_obs_report_delegates_to_command(TestContext& t) {
            t.out << "[app_interactive] obs-report delegates to command\n";
            RunFixturePaths fixture{};
            std::string fixture_err;
            const bool fixture_ok = create_unified_single_series_fixture(fixture, fixture_err);
            t.check(fixture_ok, "interactive obs-report fixture files are created");
            if (!fixture_ok) return false;

            AppCommand cmd{};
            cmd.kind = CommandKind::Interactive;

            std::istringstream in(
                std::string("2\n")
                + fixture.layout_path + "\n"
                + fixture.csv_path + "\n");
            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_interactive_command(cmd, in, out, err);
            const std::string text = out.str();

            t.check(rc == 0, "interactive obs-report returns success");
            t.check(err.str().empty(), "interactive obs-report produces no error output");
            t.check(text.find("equivalent CLI: sugarbot --obs-report") != std::string::npos,
                "interactive obs-report prints equivalent CLI command");
            t.check(text.find("[Info] obs-report active.") != std::string::npos,
                "interactive obs-report reaches obs-report command");
            t.check(text.find("Observation world provenance") != std::string::npos,
                "interactive obs-report includes observation-world provenance");
            t.check(text.find("Plan compatibility") == std::string::npos,
                "interactive obs-report does not execute plan diagnostics");
            return t.failed == 0;
        }

        bool test_interactive_plan_report_delegates_to_command(TestContext& t) {
            t.out << "[app_interactive] plan-report delegates to command\n";
            RunFixturePaths fixture{};
            std::string fixture_err;
            const bool fixture_ok = create_unified_single_series_fixture(fixture, fixture_err);
            t.check(fixture_ok, "interactive plan-report fixture files are created");
            if (!fixture_ok) return false;

            AppCommand cmd{};
            cmd.kind = CommandKind::Interactive;

            std::istringstream in(
                std::string("3\n")
                + fixture.layout_path + "\n"
                + fixture.logic_path + "\n"
                + fixture.csv_path + "\n");
            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_interactive_command(cmd, in, out, err);
            const std::string text = out.str();

            t.check(rc == 0, "interactive plan-report returns success");
            t.check(err.str().empty(), "interactive plan-report produces no error output");
            t.check(text.find("equivalent CLI: sugarbot --plan-report") != std::string::npos,
                "interactive plan-report prints equivalent CLI command");
            t.check(text.find("[Info] plan-report active.") != std::string::npos,
                "interactive plan-report reaches plan-report command");
            t.check(text.find("Plan compatibility") != std::string::npos,
                "interactive plan-report includes plan compatibility");
            t.check(text.find("PnL:") == std::string::npos,
                "interactive plan-report does not execute the strategy");
            return t.failed == 0;
        }

        bool test_interactive_feature_dump_delegates_to_command(TestContext& t) {
            t.out << "[app_interactive] feature-dump delegates to command\n";
            RunFixturePaths fixture{};
            std::string fixture_err;
            const bool fixture_ok = create_unified_feature_dump_fixture(fixture, fixture_err);
            t.check(fixture_ok, "interactive feature-dump fixture files are created");
            if (!fixture_ok) return false;

            AppCommand cmd{};
            cmd.kind = CommandKind::Interactive;

            std::istringstream in(
                std::string("4\n")
                + fixture.layout_path + "\n"
                + fixture.logic_path + "\n"
                + fixture.csv_path + "\n");
            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_interactive_command(cmd, in, out, err);
            const std::string text = out.str();

            t.check(rc == 0, "interactive feature-dump returns success");
            t.check(err.str().empty(), "interactive feature-dump produces no error output");
            t.check(text.find("equivalent CLI: sugarbot --feature-dump") != std::string::npos,
                "interactive feature-dump prints equivalent CLI command");
            t.check(text.find("series_key,entity_key,row_index,time_key,c,sma2") != std::string::npos,
                "interactive feature-dump includes feature CSV header");
            t.check(text.find("__single__,__single__,1,20240102,12,11") != std::string::npos,
                "interactive feature-dump includes computed feature values");
            return t.failed == 0;
        }

        bool test_interactive_align_dump_delegates_to_command(TestContext& t) {
            t.out << "[app_interactive] align-dump delegates to command\n";
            std::string spec_path;
            std::string csv_path;
            std::string fixture_err;
            const bool fixture_ok = create_spatial_panel_fixture(spec_path, csv_path, fixture_err, true);
            t.check(fixture_ok, "interactive align-dump fixture files are created");
            if (!fixture_ok) return false;

            AppCommand cmd{};
            cmd.kind = CommandKind::Interactive;

            std::istringstream in(
                std::string("5\n")
                + spec_path + "\n"
                + csv_path + "\n"
                + "region\n");
            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_interactive_command(cmd, in, out, err);
            const std::string text = out.str();

            t.check(rc == 0, "interactive align-dump returns success");
            t.check(err.str().empty(), "interactive align-dump produces no error output");
            t.check(text.find("equivalent CLI: sugarbot --align-dump") != std::string::npos,
                "interactive align-dump prints equivalent CLI command");
            t.check(text.find("Observation capability") != std::string::npos,
                "interactive align-dump includes observation capability");
            t.check(text.find("series_count=2") != std::string::npos,
                "interactive align-dump reports aligned series count");
            return t.failed == 0;
        }

        bool test_interactive_custom_uses_app_runtime_without_series_inputs(TestContext& t) {
            t.out << "[app_interactive] custom PlanSpec uses app runtime\n";
            PanelFixturePaths fixture{};
            std::string fixture_err;
            const bool fixture_ok = create_panel_fixture(fixture, fixture_err);
            t.check(fixture_ok, "interactive custom fixture files are created");
            if (!fixture_ok) return false;

            std::string plan_path;
            const bool plan_ok = create_custom_planspec_fixture(false, plan_path, fixture_err);
            t.check(plan_ok, "interactive custom PlanSpec fixture without series_inputs is created");
            if (!plan_ok) return false;

            AppCommand cmd{};
            cmd.kind = CommandKind::Interactive;
            cmd.interactive.has_runtime_data_spec = true;
            cmd.interactive.runtime_data_spec_path = fixture.spec_path;
            cmd.interactive.csv_path = fixture.csv_path;

            std::istringstream in(
                std::string("7\n")
                + plan_path + "\n"
                + "n\n"
                + "n\n"
                + "\n");
            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_interactive_command(cmd, in, out, err);
            const std::string text = out.str();

            t.check(rc == 0, "interactive custom command returns success for app-provided observations");
            t.check(err.str().empty(), "interactive custom command produces no error output");
            t.check(text.find("[Info] frame-first ingestion active (--data-spec)") != std::string::npos,
                "interactive custom command reports frame-first ingestion");
            t.check(text.find("Observation capability") != std::string::npos,
                "interactive custom command includes observation capability output");
            t.check(text.find("Plan compatibility") != std::string::npos,
                "interactive custom command includes plan compatibility output");
            t.check(text.find("status: valid") != std::string::npos,
                "interactive custom command reports valid plan compatibility");
            t.check(text.find("Temporal panel: valid") != std::string::npos,
                "interactive custom command reports temporal panel validity");
            t.check(text.find("Spatial panel: conditional - provide spatial neighborhood/index schema") != std::string::npos,
                "interactive custom command reports spatial-panel conditionality");
            t.check(text.find("custom PlanSpec execution using interactive CSV/DataSpec observations.") != std::string::npos,
                "interactive custom command executes against app-provided observations");
            t.check(text.find("CSV file path [default: ") != std::string::npos,
                "interactive custom command prompts for the CSV data source");
            t.check(text.find("inputs are ignored") == std::string::npos,
                "interactive custom command no longer claims app inputs are ignored");
            t.check(text.find("series_inputs is required for custom execution") == std::string::npos,
                "interactive custom command no longer fails on missing series_inputs");
            return t.failed == 0;
        }

        bool test_interactive_custom_conflict_defaults_to_app_runtime(TestContext& t) {
            t.out << "[app_interactive] custom PlanSpec conflict defaults to app runtime\n";
            PanelFixturePaths fixture{};
            std::string fixture_err;
            const bool fixture_ok = create_panel_fixture(fixture, fixture_err);
            t.check(fixture_ok, "interactive conflict fixture files are created");
            if (!fixture_ok) return false;

            std::string plan_path;
            const bool plan_ok = create_custom_planspec_fixture(true, plan_path, fixture_err);
            t.check(plan_ok, "interactive conflict PlanSpec fixture with series_inputs is created");
            if (!plan_ok) return false;

            AppCommand cmd{};
            cmd.kind = CommandKind::Interactive;
            cmd.interactive.has_runtime_data_spec = true;
            cmd.interactive.runtime_data_spec_path = fixture.spec_path;
            cmd.interactive.csv_path = fixture.csv_path;

            std::istringstream in(
                std::string("7\n")
                + plan_path + "\n"
                + "n\n"
                + "n\n"
                + "n\n"
                + "\n");
            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_interactive_command(cmd, in, out, err);
            const std::string text = out.str();

            t.check(rc == 0, "interactive custom conflict command returns success");
            t.check(err.str().empty(), "interactive custom conflict command produces no error output");
            t.check(text.find("PlanSpec embeds legacy observation inputs.") != std::string::npos,
                "interactive custom conflict reports both observation sources");
            t.check(text.find("custom PlanSpec execution using interactive CSV/DataSpec observations.") != std::string::npos,
                "interactive custom conflict defaults to app-provided observations");
            return t.failed == 0;
        }

        bool test_interactive_file_run_reprompts_missing_path(TestContext& t) {
            t.out << "[app_interactive] file run reprompts missing paths\n";
            RunFixturePaths fixture{};
            std::string fixture_err;
            const bool fixture_ok = create_unified_single_series_fixture(fixture, fixture_err);
            t.check(fixture_ok, "interactive bad-path fixture files are created");
            if (!fixture_ok) return false;

            AppCommand cmd{};
            cmd.kind = CommandKind::Interactive;

            const std::string missing_layout = fixture.layout_path + ".missing";
            std::istringstream in(
                std::string("\n")
                + missing_layout + "\n"
                + fixture.layout_path + "\n"
                + fixture.logic_path + "\n"
                + fixture.csv_path + "\n");
            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_interactive_command(cmd, in, out, err);
            const std::string text = out.str();

            t.check(rc == 0, "interactive file run succeeds after reprompting missing path");
            t.check(err.str().empty(), "interactive file run reprompt produces no error output");
            t.check(text.find("File not found: " + missing_layout) != std::string::npos,
                "interactive file run reports the missing path at prompt time");
            t.check(text.find("equivalent CLI: sugarbot run") != std::string::npos,
                "interactive file run still delegates after a corrected path");
            t.check(text.find("PnL:") != std::string::npos,
                "interactive file run executes after path correction");
            return t.failed == 0;
        }

        bool test_interactive_template_prompts_for_csv_source(TestContext& t) {
            t.out << "[app_interactive] template prompts for CSV source\n";
            AppCommand cmd{};
            cmd.kind = CommandKind::Interactive;

            std::istringstream in("6\n1\n1\n\n\n\nq\n");
            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_interactive_command(cmd, in, out, err);
            const std::string text = out.str();

            t.check(rc == 0, "interactive template CSV cancellation returns success");
            t.check(err.str().empty(), "interactive template CSV cancellation produces no error output");
            t.check(text.find("CSV file path (q to cancel): ") != std::string::npos,
                "interactive template path requires an explicit CSV path when none was provided");
            t.check(text.find("Path is required. Enter a path or q to cancel.") != std::string::npos,
                "interactive template path reprompts when no CSV path is provided");
            t.check(text.find("Interactive template execution cancelled.") != std::string::npos,
                "interactive template path can cancel at the CSV prompt");
            t.check(text.find("data\\BTCUSD_420.csv") == std::string::npos,
                "interactive template path no longer leaks the repo fixture CSV as a default");
            return t.failed == 0;
        }

        bool test_interactive_template_runs_real_sweep_with_live_progress(TestContext& t) {
            t.out << "[app_interactive] template path runs a real live sweep\n";
            std::string csv_path;
            std::string fixture_err;
            const bool fixture_ok = create_time_header_daily_fixture(csv_path, fixture_err);
            t.check(fixture_ok, "interactive template sweep fixture CSV is created");
            if (!fixture_ok) return false;

            AppCommand cmd{};
            cmd.kind = CommandKind::Interactive;
            cmd.interactive.csv_path = csv_path;

            std::istringstream in("6\n2\n\n\n\n\n");
            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_interactive_command(cmd, in, out, err);
            const std::string text = out.str();

            t.check(rc == 0, "interactive template sweep returns success");
            t.check(err.str().empty(), "interactive template sweep produces no error output");
            t.check(text.find("Strategy template list (built-in sweep search)") != std::string::npos,
                "interactive menu labels template path as a built-in sweep search");
            t.check(text.find("[Info] template sweep configured: engine=exhaustive") != std::string::npos,
                "interactive template run explains that a real sweep is configured");
            t.check(text.find("[Info] sweep params: n_fast, n_slow") != std::string::npos,
                "interactive template run names the swept parameters");
            t.check(text.find("[sweep] candidate evaluation ") != std::string::npos,
                "interactive template run emits live sweep progress");
            t.check(text.find("=== PlanSpec Sweep Results ===") != std::string::npos,
                "interactive template run prints the structured sweep report");
            t.check(text.find("Total combos: 6878") != std::string::npos,
                "interactive template run reports the expanded search space");
            t.check(text.find("Sweep params:") != std::string::npos,
                "interactive template run prints sweep parameter ranges in the final report");
            return t.failed == 0;
        }

        bool test_panel_custom_uses_app_runtime_without_series_inputs(TestContext& t) {
            t.out << "[app_panel] custom PlanSpec uses app runtime\n";
            PanelFixturePaths fixture{};
            std::string fixture_err;
            const bool fixture_ok = create_panel_fixture(fixture, fixture_err);
            t.check(fixture_ok, "panel custom fixture files are created");
            if (!fixture_ok) return false;

            std::string plan_path;
            const bool plan_ok = create_custom_planspec_fixture(false, plan_path, fixture_err);
            t.check(plan_ok, "panel custom PlanSpec fixture without series_inputs is created");
            if (!plan_ok) return false;

            AppCommand cmd{};
            cmd.kind = CommandKind::RunPanel;
            cmd.run_panel.spec_path = fixture.spec_path;
            cmd.run_panel.csv_path = fixture.csv_path;
            cmd.run_panel.topk = 2;

            std::istringstream in(
                std::string("2\n")
                + plan_path + "\n"
                + "n\n"
                + "n\n");
            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_panel_command(cmd, in, out, err);
            const std::string text = out.str();

            t.check(rc == 0, "panel custom command returns success for app-provided observations");
            t.check(err.str().empty(), "panel custom command produces no error output");
            t.check(text.find("Observation capability") != std::string::npos,
                "panel custom command includes observation capability output");
            t.check(text.find("Plan compatibility") != std::string::npos,
                "panel custom command includes plan compatibility output");
            t.check(text.find("status: valid") != std::string::npos,
                "panel custom command reports valid plan compatibility");
            t.check(text.find("Temporal panel: valid") != std::string::npos,
                "panel custom command reports temporal panel validity");
            t.check(text.find("Spatial panel: conditional - provide spatial neighborhood/index schema") != std::string::npos,
                "panel custom command reports spatial-panel conditionality");
            t.check(text.find("custom panel execution using --run-panel DataSpec/CSV observations.") != std::string::npos,
                "panel custom command executes against app-provided observations");
            t.check(text.find("ranking_objective=maximize reported_metric:pnl - 0.25 * reported_metric:max_drawdown") != std::string::npos,
                "panel custom command reports canonical ranking objective");
            t.check(text.find("inputs are ignored") == std::string::npos,
                "panel custom command no longer claims CLI inputs are ignored");
            t.check(text.find("series_inputs is required for custom execution") == std::string::npos,
                "panel custom command no longer fails on missing series_inputs");
            return t.failed == 0;
        }

        bool test_panel_template_reports_plan_compatibility(TestContext& t) {
            t.out << "[app_panel] template execution reports plan compatibility\n";
            PanelFixturePaths fixture{};
            std::string fixture_err;
            const bool fixture_ok = create_panel_fixture(fixture, fixture_err);
            t.check(fixture_ok, "panel template fixture files are created");
            if (!fixture_ok) return false;

            AppCommand cmd{};
            cmd.kind = CommandKind::RunPanel;
            cmd.run_panel.spec_path = fixture.spec_path;
            cmd.run_panel.csv_path = fixture.csv_path;
            cmd.run_panel.topk = 2;

            std::istringstream in("1\n1\n1\n");
            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_panel_command(cmd, in, out, err);
            const std::string text = out.str();

            t.check(rc == 0, "panel template command returns success");
            t.check(err.str().empty(), "panel template command produces no error output");
            t.check(text.find("Plan compatibility") != std::string::npos,
                "panel template command includes plan compatibility output");
            t.check(text.find("status: valid") != std::string::npos,
                "panel template command reports valid plan compatibility");
            return t.failed == 0;
        }

        bool test_panel_custom_reports_missing_runtime_ref_before_execution(TestContext& t) {
            t.out << "[app_panel] custom plan reports missing runtime ref before execution\n";
            PanelFixturePaths fixture{};
            std::string fixture_err;
            const bool fixture_ok = create_panel_fixture(fixture, fixture_err);
            t.check(fixture_ok, "panel missing-runtime-ref fixture files are created");
            if (!fixture_ok) return false;

            std::string plan_path;
            const bool plan_ok = create_custom_missing_runtime_ref_planspec_fixture(plan_path, fixture_err);
            t.check(plan_ok, "panel missing-runtime-ref PlanSpec fixture is created");
            if (!plan_ok) return false;

            AppCommand cmd{};
            cmd.kind = CommandKind::RunPanel;
            cmd.run_panel.spec_path = fixture.spec_path;
            cmd.run_panel.csv_path = fixture.csv_path;
            cmd.run_panel.topk = 2;

            std::istringstream in(
                std::string("2\n")
                + plan_path + "\n"
                + "n\n"
                + "n\n");
            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_panel_command(cmd, in, out, err);
            const std::string text = out.str();

            t.check(rc == 0, "panel missing-runtime-ref command returns success with panel failures reported");
            t.check(err.str().empty(), "panel missing-runtime-ref command produces no error output");
            t.check(text.find("Plan compatibility") != std::string::npos,
                "panel missing-runtime-ref command includes plan compatibility output");
            t.check(text.find("status: invalid") != std::string::npos,
                "panel missing-runtime-ref command reports invalid plan compatibility");
            t.check(text.find("missing runtime series: ZZZ") != std::string::npos,
                "panel missing-runtime-ref command explains missing runtime series before execution");
            t.check(text.find("Panel result") != std::string::npos,
                "panel missing-runtime-ref command still prints panel result summary");
            t.check(text.find("failed=2") != std::string::npos,
                "panel missing-runtime-ref command reports failed entities");
            t.check(text.find("series=AAA:") != std::string::npos,
                "panel missing-runtime-ref command preserves failing series identity in output");
            return t.failed == 0;
        }

    } // namespace

    void register_interactive_tests(AppTestRegistry& tests) {
        tests.push_back(test_interactive_q_cancels_before_default_run);
        tests.push_back(test_interactive_invalid_menu_reprompts_then_cancels);
        tests.push_back(test_interactive_file_run_uses_unified_pipeline);
        tests.push_back(test_interactive_obs_report_delegates_to_command);
        tests.push_back(test_interactive_plan_report_delegates_to_command);
        tests.push_back(test_interactive_feature_dump_delegates_to_command);
        tests.push_back(test_interactive_align_dump_delegates_to_command);
        tests.push_back(test_interactive_custom_uses_app_runtime_without_series_inputs);
        tests.push_back(test_interactive_custom_conflict_defaults_to_app_runtime);
        tests.push_back(test_interactive_file_run_reprompts_missing_path);
        tests.push_back(test_interactive_template_prompts_for_csv_source);
        tests.push_back(test_interactive_template_runs_real_sweep_with_live_progress);
        tests.push_back(test_panel_custom_uses_app_runtime_without_series_inputs);
        tests.push_back(test_panel_template_reports_plan_compatibility);
        tests.push_back(test_panel_custom_reports_missing_runtime_ref_before_execution);
    }

} // namespace sugar::app::tests
