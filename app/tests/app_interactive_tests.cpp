#include "tests/app_test_support.h"

#include <sstream>
#include <string>

#include "commands/app_commands.h"
#include "interactive/app_interactive.h"

namespace sugar::app::tests {

    namespace {

        std::size_t count_occurrences(const std::string& text, const std::string& needle) {
            std::size_t count = 0;
            std::size_t pos = 0;
            while ((pos = text.find(needle, pos)) != std::string::npos) {
                ++count;
                pos += needle.size();
            }
            return count;
        }

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
            t.check(text.find("new-user path: 1) first real run, 2) inspect before running") != std::string::npos,
                "interactive menu advertises the tutorial journey");
            t.check(text.find("Start here: layout.json + PlanSpec + CSV -> real run report") != std::string::npos,
                "interactive menu explains the first-run lesson");
            t.check(text.find("Inspect before running: combined plan report, no execution") != std::string::npos,
                "interactive menu explains inspection before execution");
            t.check(text.find("Sweep engine chapters: exhaustive, Monte Carlo, coarse/fine, Bayesian posterior") != std::string::npos &&
                    text.find("Dynamic execution + topology chapters over checked-in demos") != std::string::npos,
                "interactive menu advertises follow-on tutorial chapters");
            t.check(text.find("Advanced report projections: table, objective, diversity, duplicates, geometry, Pareto, trace, JSON") != std::string::npos,
                "interactive menu advertises the advanced report tutorial");
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
            t.check(text.find("Invalid selection. Enter 1-6 or q to cancel.") != std::string::npos,
                "interactive invalid menu input is reported");
            t.check(text.find("Interactive command cancelled") != std::string::npos,
                "interactive invalid menu path can still cancel");
            t.check(text.find("PnL:") == std::string::npos,
                "interactive invalid menu does not fall through into execution");
            return t.failed == 0;
        }

        bool test_interactive_first_run_tutorial_uses_checked_in_assets(TestContext& t) {
            t.out << "[app_interactive] first-run tutorial uses checked-in assets\n";
            AppCommand cmd{};
            cmd.kind = CommandKind::Interactive;

            std::istringstream in("\n\nq\n");
            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_interactive_command(cmd, in, out, err);
            const std::string text = out.str();

            t.check(rc == 0, "interactive first-run tutorial returns success");
            t.check(err.str().empty(), "interactive first-run tutorial produces no error output");
            t.check(text.find("SugarBot tutorial: first real run") != std::string::npos,
                "interactive first-run tutorial labels the lesson");
            t.check(text.find("Learning goal: understand the three artifacts SugarBot actually runs.") != std::string::npos,
                "interactive first-run tutorial states the learning goal");
            t.check(text.find("graphs/demos/quickstart_ohlcv.layout.json") != std::string::npos &&
                    text.find("graphs/demos/quickstart_ohlcv.planspec.json") != std::string::npos &&
                    text.find("data/sample.csv") != std::string::npos,
                "interactive first-run tutorial uses checked-in quickstart assets");
            t.check(text.find("Schema spotlight:") != std::string::npos &&
                    text.find("layout.ingest maps CSV columns") != std::string::npos &&
                    text.find("PlanSpec numeric_nodes and bool_nodes") != std::string::npos,
                "interactive first-run tutorial teaches the schema pieces before running");
            t.check(text.find("equivalent CLI: sugarbot run") != std::string::npos,
                "interactive first-run tutorial prints equivalent CLI command");
            t.check(text.find("tutorial run uses the existing noninteractive pipeline") != std::string::npos,
                "interactive first-run tutorial reports shared pipeline");
            t.check(text.find("noninteractive artifact run active") != std::string::npos,
                "interactive first-run tutorial reaches unified run command");
            t.check(text.find("Tutorial debrief:") != std::string::npos &&
                    text.find("Plan compatibility") != std::string::npos &&
                    text.find("PnL:") != std::string::npos,
                "interactive first-run tutorial runs and debriefs a real report");
            t.check(text.find("Press Enter to return to the tutorial menu (q to quit):") != std::string::npos,
                "interactive first-run tutorial pauses before returning to the menu");
            t.check(count_occurrences(text, "Choose SugarBot tutorial:") == 2,
                "interactive first-run tutorial returns to the menu without reloading");
            return t.failed == 0;
        }

        bool test_interactive_advanced_reports_tutorial_runs_real_projections(TestContext& t) {
            t.out << "[app_interactive] advanced reports tutorial runs real projections\n";
            AppCommand cmd{};
            cmd.kind = CommandKind::Interactive;

            std::istringstream in("6\n");
            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_interactive_command(cmd, in, out, err);
            const std::string text = out.str();

            t.check(rc == 0, "interactive advanced reports tutorial returns success");
            t.check(err.str().empty(), "interactive advanced reports tutorial produces no error output");
            t.check(text.find("SugarBot tutorial: advanced report projections") != std::string::npos,
                "interactive advanced reports tutorial labels the lesson");
            t.check(text.find("Learning goal: choose the narrowest report projection for the question being asked.") != std::string::npos,
                "interactive advanced reports tutorial states the learning goal");
            t.check(text.find("i_advanced_report_projection_sweep.planspec.json") != std::string::npos,
                "interactive advanced reports tutorial uses the checked-in advanced report artifact");
            t.check(text.find("--report candidate_table") != std::string::npos &&
                    text.find("Sweep candidate table:") != std::string::npos,
                "interactive advanced reports tutorial runs candidate table projection");
            t.check(text.find("--report objective_breakdown") != std::string::npos &&
                    text.find("Sweep objective breakdown:") != std::string::npos &&
                    text.find("components:") != std::string::npos,
                "interactive advanced reports tutorial runs objective projection");
            t.check(text.find("--report diversity") != std::string::npos &&
                    text.find("Sweep diversity diagnostics:") != std::string::npos &&
                    text.find("requested=true") != std::string::npos,
                "interactive advanced reports tutorial runs diversity projection");
            t.check(text.find("--report duplicate_trade_paths") != std::string::npos &&
                    text.find("Duplicate trade-path groups:") != std::string::npos,
                "interactive advanced reports tutorial runs duplicate-path projection");
            t.check(text.find("--report search_geometry") != std::string::npos &&
                    text.find("Search geometry:") != std::string::npos &&
                    text.find("Top basin summaries") != std::string::npos,
                "interactive advanced reports tutorial runs search-geometry projection");
            t.check(text.find("--report pareto") != std::string::npos &&
                    text.find("Sweep Pareto front:") != std::string::npos &&
                    text.find("non_dominated_candidates=") != std::string::npos,
                "interactive advanced reports tutorial runs Pareto projection");
            t.check(text.find("--report trace") != std::string::npos &&
                    text.find("Sweep replay trace:") != std::string::npos &&
                    text.find("exact_replay_from_report=true") != std::string::npos,
                "interactive advanced reports tutorial runs replay trace projection");
            t.check(text.find("--report json") != std::string::npos &&
                    text.find("{\"schema_version\":\"sugarbot.evaluation_report.v1\"") != std::string::npos &&
                    text.find("\"pareto\":{") != std::string::npos,
                "interactive advanced reports tutorial runs scriptable JSON projection");
            t.check(text.find("Milestone I is complete") != std::string::npos,
                "interactive advanced reports tutorial closes the milestone explicitly");
            return t.failed == 0;
        }

        bool test_interactive_sweep_engine_tutorial_runs_real_chapters(TestContext& t) {
            t.out << "[app_interactive] sweep engine tutorial chapters run real reports\n";
            AppCommand cmd{};
            cmd.kind = CommandKind::Interactive;

            std::istringstream in("4\n");
            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_interactive_command(cmd, in, out, err);
            const std::string text = out.str();

            t.check(rc == 0, "interactive sweep-engine tutorial returns success");
            t.check(err.str().empty(), "interactive sweep-engine tutorial produces no error output");
            t.check(text.find("SugarBot tutorial: sweep engine chapters") != std::string::npos,
                "interactive sweep-engine tutorial labels the lesson");
            t.check(text.find("Learning goal: compare sweep.engine choices while keeping layout and CSV fixed.") != std::string::npos,
                "interactive sweep-engine tutorial states the learning goal");
            t.check(text.find("Chapter: exhaustive grid") != std::string::npos &&
                    text.find("sweep.engine=\"exhaustive\"") != std::string::npos &&
                    text.find("engine=exhaustive") != std::string::npos,
                "interactive sweep-engine tutorial runs exhaustive compact chapter");
            t.check(text.find("Chapter: Monte Carlo seeded sample") != std::string::npos &&
                    text.find("sweep.engine_config.monte_carlo carries draws, seed, dedupe") != std::string::npos &&
                    text.find("engine=monte_carlo") != std::string::npos,
                "interactive sweep-engine tutorial runs Monte Carlo chapter");
            t.check(text.find("Chapter: coarse/fine neighborhood search") != std::string::npos &&
                    text.find("sweep.engine_config.coarse_fine carries stage breadth") != std::string::npos &&
                    text.find("engine=coarse_fine") != std::string::npos,
                "interactive sweep-engine tutorial runs coarse/fine chapter");
            t.check(text.find("Chapter: Bayesian posterior policy") != std::string::npos &&
                    text.find("sweep.engine_config.bayesian_posterior carries surrogate") != std::string::npos &&
                    text.find("Bayesian posterior diagnostics:") != std::string::npos &&
                    text.find("policy config:") != std::string::npos,
                "interactive sweep-engine tutorial runs Bayesian posterior diagnostics chapter");
            t.check(text.find("equivalent CLI: sugarbot run") != std::string::npos &&
                    text.find("--report compact") != std::string::npos &&
                    text.find("--report posterior") != std::string::npos,
                "interactive sweep-engine tutorial prints equivalent report commands");
            t.check(text.find("chapter run uses the existing noninteractive pipeline") != std::string::npos &&
                    text.find("Tutorial debrief:") != std::string::npos,
                "interactive sweep-engine tutorial debriefs after real command execution");
            return t.failed == 0;
        }

        bool test_interactive_dynamic_topology_tutorial_runs_real_chapters(TestContext& t) {
            t.out << "[app_interactive] dynamic/topology tutorial chapters run real reports\n";
            AppCommand cmd{};
            cmd.kind = CommandKind::Interactive;

            std::istringstream in("5\n");
            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_interactive_command(cmd, in, out, err);
            const std::string text = out.str();

            t.check(rc == 0, "interactive dynamic/topology tutorial returns success");
            t.check(err.str().empty(), "interactive dynamic/topology tutorial produces no error output");
            t.check(text.find("SugarBot tutorial: dynamic execution and topology") != std::string::npos,
                "interactive dynamic/topology tutorial labels the lesson");
            t.check(text.find("Chapter: row-varying declared dynamic routing") != std::string::npos &&
                    text.find("execution.mode=\"declared_dynamic\"") != std::string::npos &&
                    text.find("Dynamic execution:") != std::string::npos &&
                    text.find("route row_threshold_gate condition=route_high") != std::string::npos,
                "interactive dynamic/topology tutorial runs the row-varying dynamic chapter");
            t.check(text.find("Chapter: topology-informed dynamic routing") != std::string::npos &&
                    text.find("layout.topologies declares query-bearing graph assets") != std::string::npos &&
                    text.find("route topology_degree_gate condition=route_dense") != std::string::npos &&
                    text.find("f8.row.market.graph") != std::string::npos,
                "interactive dynamic/topology tutorial runs the topology-informed dynamic chapter");
            t.check(text.find("Chapter: focused topology projection") != std::string::npos &&
                    text.find("--report topology") != std::string::npos &&
                    text.find("Topology diagnostics:") != std::string::npos &&
                    text.find("node=degree op=topology.degree topology=f8.row.market.graph") != std::string::npos,
                "interactive dynamic/topology tutorial runs the focused topology projection");
            t.check(text.find("equivalent CLI: sugarbot run") != std::string::npos &&
                    text.find("graphs/demos/dynamic_execution/g10_row_varying_declared_dynamic.planspec.json") != std::string::npos &&
                    text.find("graphs/demos/dynamic_execution/g10_topology_declared_dynamic.planspec.json") != std::string::npos,
                "interactive dynamic/topology tutorial prints equivalent checked-in demo commands");
            t.check(text.find("Tutorial debrief:") != std::string::npos &&
                    text.find("not a second executor") != std::string::npos,
                "interactive dynamic/topology tutorial debriefs declared dynamic boundaries");
            return t.failed == 0;
        }

        bool test_interactive_direct_file_run_uses_unified_pipeline(TestContext& t) {
            t.out << "[app_interactive] direct file run uses unified pipeline\n";
            RunFixturePaths fixture{};
            std::string fixture_err;
            const bool fixture_ok = create_unified_single_series_fixture(fixture, fixture_err);
            t.check(fixture_ok, "interactive file-run fixture files are created");
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

            t.check(rc == 0, "interactive file run returns success");
            t.check(err.str().empty(), "interactive file run produces no error output");
            t.check(text.find("SugarBot tutorial: direct artifact run") != std::string::npos,
                "interactive direct file run labels the direct-run path");
            t.check(text.find("direct artifact run using existing noninteractive pipeline") != std::string::npos,
                "interactive file run reports shared pipeline");
            t.check(text.find("equivalent CLI: sugarbot run") != std::string::npos,
                "interactive file run prints equivalent CLI command");
            t.check(text.find("noninteractive artifact run active") != std::string::npos,
                "interactive file run reaches unified run command");
            t.check(text.find("Plan compatibility") != std::string::npos,
                "interactive file run emits plan compatibility report");
            t.check(text.find("PnL:") != std::string::npos,
                "interactive file run executes the strategy");
            t.check(text.find("Tutorial debrief:") != std::string::npos &&
                    text.find("same path as `sugarbot run`") != std::string::npos &&
                    text.find("reproducible form of this run") != std::string::npos,
                "interactive file run debriefs a successful user-artifact run");
            return t.failed == 0;
        }

        bool test_interactive_inspect_tutorial_delegates_to_diagnostics(TestContext& t) {
            t.out << "[app_interactive] inspect tutorial delegates to diagnostics\n";
            AppCommand cmd{};
            cmd.kind = CommandKind::Interactive;

            std::istringstream in("2\n");
            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_interactive_command(cmd, in, out, err);
            const std::string text = out.str();

            t.check(rc == 0, "interactive inspect tutorial returns success");
            t.check(err.str().empty(), "interactive inspect tutorial produces no error output");
            t.check(text.find("SugarBot tutorial: inspect before running") != std::string::npos,
                "interactive inspect tutorial labels the lesson");
            t.check(text.find("Learning goal: diagnose data and logic before asking SugarBot for a result.") != std::string::npos,
                "interactive inspect tutorial states the learning goal");
            t.check(text.find("Schema spotlight:") != std::string::npos &&
                    text.find("--obs-report is the narrow data-only check") != std::string::npos &&
                    text.find("--plan-report includes observation checks") != std::string::npos,
                "interactive inspect tutorial teaches diagnostic report purpose");
            t.check(text.find("equivalent CLI: sugarbot --obs-report") == std::string::npos &&
                    text.find("[Info] obs-report active.") == std::string::npos,
                "interactive inspect tutorial avoids a standalone duplicate obs-report run");
            t.check(text.find("Observation world provenance") != std::string::npos,
                "interactive inspect tutorial includes observation-world provenance through plan-report");
            t.check(text.find("equivalent CLI: sugarbot --plan-report") != std::string::npos,
                "interactive inspect tutorial prints equivalent plan-report command");
            t.check(text.find("[Info] plan-report active.") != std::string::npos,
                "interactive inspect tutorial reaches plan-report command");
            t.check(text.find("Plan compatibility") != std::string::npos,
                "interactive inspect tutorial includes plan compatibility");
            t.check(text.find("PnL:") == std::string::npos,
                "interactive inspect tutorial does not execute the strategy");
            t.check(text.find("Tutorial debrief:") != std::string::npos,
                "interactive inspect tutorial debriefs the diagnostics");
            t.check(text.find("Use standalone --obs-report only when data parsing") != std::string::npos,
                "interactive inspect tutorial explains when to use the narrower obs-report");
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
                std::string("3\n")
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
            t.check(text.find("direct artifact run using existing noninteractive pipeline") != std::string::npos,
                "interactive file run reports shared pipeline after correction");
            t.check(text.find("equivalent CLI: sugarbot run") != std::string::npos,
                "interactive file run still delegates after a corrected path");
            t.check(text.find("PnL:") != std::string::npos,
                "interactive file run executes after path correction");
            t.check(text.find("Tutorial debrief:") != std::string::npos,
                "interactive file run debriefs after path correction and successful execution");
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
        tests.push_back(test_interactive_first_run_tutorial_uses_checked_in_assets);
        tests.push_back(test_interactive_direct_file_run_uses_unified_pipeline);
        tests.push_back(test_interactive_inspect_tutorial_delegates_to_diagnostics);
        tests.push_back(test_interactive_file_run_reprompts_missing_path);
        tests.push_back(test_interactive_sweep_engine_tutorial_runs_real_chapters);
        tests.push_back(test_interactive_dynamic_topology_tutorial_runs_real_chapters);
        tests.push_back(test_interactive_advanced_reports_tutorial_runs_real_projections);
        tests.push_back(test_panel_custom_uses_app_runtime_without_series_inputs);
        tests.push_back(test_panel_template_reports_plan_compatibility);
        tests.push_back(test_panel_custom_reports_missing_runtime_ref_before_execution);
    }

} // namespace sugar::app::tests
