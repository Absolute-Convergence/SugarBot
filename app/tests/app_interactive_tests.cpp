#include "tests/app_test_support.h"

#include <sstream>
#include <string>

#include "commands/app_commands.h"
#include "interactive/app_interactive.h"

namespace sugar::app::tests {

    namespace {

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
                std::string("2\n")
                + plan_path + "\n"
                + "n\n"
                + "n\n");
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
                std::string("2\n")
                + plan_path + "\n"
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
            t.check(text.find("ranking_metric=pnl_dd") != std::string::npos,
                "panel custom command reports explicit ranking metric");
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
        tests.push_back(test_interactive_custom_uses_app_runtime_without_series_inputs);
        tests.push_back(test_interactive_custom_conflict_defaults_to_app_runtime);
        tests.push_back(test_panel_custom_uses_app_runtime_without_series_inputs);
        tests.push_back(test_panel_template_reports_plan_compatibility);
        tests.push_back(test_panel_custom_reports_missing_runtime_ref_before_execution);
    }

} // namespace sugar::app::tests
