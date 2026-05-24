#include "tests/app_test_support.h"

#include <filesystem>
#include <fstream>
#include <ios>
#include <sstream>
#include <string>
#include <vector>

#include "commands/app_commands.h"
#include "sweep/plan_spec.h"

namespace sugar::app::tests {

    namespace {

        namespace fs = std::filesystem;

        std::string make_dynamic_validation_plan_json(
            const std::string& route_condition,
            const std::string& activate_subgraph,
            const std::string& selected_subgraph_node) {
            std::ostringstream json;
            json <<
                "{\n"
                "  \"version\": 1,\n"
                "  \"trade_mode\": \"LongOnly\",\n"
                "  \"numeric_nodes\": [\n"
                "    { \"id\": \"c\", \"op\": \"core.close\" },\n"
                "    { \"id\": \"threshold\", \"op\": \"core.const\", \"params\": { \"value\": 10.0 } }\n"
                "  ],\n"
                "  \"bool_nodes\": [\n"
                "    { \"id\": \"fallback_entry\", \"op\": \"core.bool_const\", \"params\": { \"value\": false } },\n"
                "    { \"id\": \"route_high\", \"op\": \"cmp.gt\", \"args\": [\"c\", \"threshold\"] },\n"
                "    { \"id\": \"selected_entry\", \"op\": \"cmp.gt\", \"args\": [\"c\", \"threshold\"] }\n"
                "  ],\n"
                "  \"bindings\": { \"entry_long\": \"fallback_entry\", \"exit_long\": \"\", \"entry_short\": \"\", \"exit_short\": \"\", \"allow_trade\": \"\" },\n"
                "  \"execution\": {\n"
                "    \"mode\": \"declared_dynamic\",\n"
                "    \"subgraphs\": [\n"
                "      { \"id\": \"fallback\", \"nodes\": [\"fallback_entry\"] },\n"
                "      { \"id\": \"selected\", \"nodes\": [\"" << selected_subgraph_node << "\"] }\n"
                "    ],\n"
                "    \"routes\": [\n"
                "      {\n"
                "        \"id\": \"row_gate\",\n"
                "        \"condition\": \"" << route_condition << "\",\n"
                "        \"cases\": [\n"
                "          { \"id\": \"high_row\", \"when\": true, \"activate\": [\"" << activate_subgraph << "\"] }\n"
                "        ],\n"
                "        \"fallback\": [\"fallback\"]\n"
                "      }\n"
                "    ],\n"
                "    \"outputs\": { \"entry_long\": \"selected_entry\" },\n"
                "    \"bounds\": {\n"
                "      \"max_routes\": 1,\n"
                "      \"max_active_subgraphs\": 1,\n"
                "      \"max_selected_outputs\": 1,\n"
                "      \"decision_sample_limit\": 4\n"
                "    }\n"
                "  }\n"
                "}\n";
            return json.str();
        }

        bool test_dump_plan_power_user_diagnostic_guidance(TestContext& t) {
            t.out << "[app_ux] dump-plan power-user diagnostic guidance\n";
            std::ostringstream out;
            const int rc = run_dump_plan_command(out);
            const std::string text = out.str();

            t.check(rc == 0, "dump-plan command returns success");
            t.check(text.find("bayesian_posterior") != std::string::npos &&
                    text.find("sweep.engine_config.bayesian_posterior") != std::string::npos &&
                    text.find("posterior diagnostics are available through local CLI") != std::string::npos &&
                    text.find("stabilization target") == std::string::npos &&
                    text.find("deferred") == std::string::npos,
                "dump-plan documents posterior usage without internal roadmap language");
            t.check(text.find("core.const") != std::string::npos &&
                    text.find("params.value={\"ref\":\"param_name\"}") != std::string::npos &&
                    text.find("there is no `core.param` operator") != std::string::npos,
                "dump-plan documents sweep parameter thresholds through core.const refs");
            t.check(text.find("do not write `run.objective: \"pnl\"`") != std::string::npos &&
                    text.find("compatibility `run.ranking_metric: \"pnl\"`") != std::string::npos,
                "dump-plan distinguishes run.objective object form from ranking_metric shorthand");
            return t.failed == 0;
        }

        bool test_plan_parser_explains_run_objective_string_shortcut(TestContext& t) {
            t.out << "[app_ux] run.objective string shortcut hint\n";
            const std::string json =
                "{\n"
                "  \"version\": 1,\n"
                "  \"trade_mode\": \"LongOnly\",\n"
                "  \"numeric_nodes\": [],\n"
                "  \"bool_nodes\": [],\n"
                "  \"bindings\": { \"entry_long\": \"\", \"exit_long\": \"\", \"entry_short\": \"\", \"exit_short\": \"\", \"allow_trade\": \"\" },\n"
                "  \"run\": {\n"
                "    \"scope\": \"per_entity_ranked\",\n"
                "    \"top_k\": 2,\n"
                "    \"objective\": \"pnl\"\n"
                "  }\n"
                "}\n";

            sugar::PlanSpec spec{};
            std::string err;
            const bool ok = sugar::parse_plan_spec_json(json, spec, err);
            t.check(!ok, "PlanSpec parser rejects string run.objective");
            t.check(err.find("run.objective must be object") != std::string::npos &&
                    err.find("run.ranking_metric=\"pnl\"") != std::string::npos &&
                    err.find("string shortcut is only accepted by sweep.objective") != std::string::npos,
                "PlanSpec parser explains the ranked-run pnl shorthand");
            return t.failed == 0;
        }

        bool test_plan_report_missing_column_case_lists_available_columns(TestContext& t) {
            t.out << "[app_ux] plan-report missing column lists available columns\n";
            std::string fixture_err;
            const auto fixture_dir = app_fixture_dir(fixture_err);
            t.check(!fixture_dir.empty(), "missing-column case fixture dir is available");
            if (fixture_dir.empty()) return false;

            const auto layout_path = fixture_dir / "app_tests_tmp_missing_column_case_layout.json";
            const auto logic_path = fixture_dir / "app_tests_tmp_missing_column_case_logic.json";
            const auto csv_path = fixture_dir / "app_tests_tmp_missing_column_case.csv";

            {
                std::ofstream layout_out(layout_path, std::ios::binary | std::ios::trunc);
                t.check(static_cast<bool>(layout_out), "missing-column case layout file opens");
                if (!layout_out) return false;
                layout_out <<
                    "{\n"
                    "  \"ingest\": {\n"
                    "    \"time_col\": \"date\",\n"
                    "    \"time_kind\": \"YYYY-MM-DD\",\n"
                    "    \"value_cols\": [\"close\", \"MA\"],\n"
                    "    \"primary_value_col\": \"close\"\n"
                    "  }\n"
                    "}\n";
            }

            {
                std::ofstream logic_out(logic_path, std::ios::binary | std::ios::trunc);
                t.check(static_cast<bool>(logic_out), "missing-column case logic file opens");
                if (!logic_out) return false;
                logic_out <<
                    "{\n"
                    "  \"version\": 1,\n"
                    "  \"trade_mode\": \"LongOnly\",\n"
                    "  \"numeric_nodes\": [\n"
                    "    { \"id\": \"ma_lower\", \"op\": \"core.ref\", \"params\": { \"ref\": \"ma\" } },\n"
                    "    { \"id\": \"zero\", \"op\": \"core.const\", \"params\": { \"value\": 0.0 } }\n"
                    "  ],\n"
                    "  \"bool_nodes\": [\n"
                    "    { \"id\": \"entry\", \"op\": \"cmp.gt\", \"args\": [\"ma_lower\", \"zero\"] }\n"
                    "  ],\n"
                    "  \"bindings\": { \"entry_long\": \"entry\", \"exit_long\": \"\", \"entry_short\": \"\", \"exit_short\": \"\", \"allow_trade\": \"\" }\n"
                    "}\n";
            }

            {
                std::ofstream csv_out(csv_path, std::ios::binary | std::ios::trunc);
                t.check(static_cast<bool>(csv_out), "missing-column case csv file opens");
                if (!csv_out) return false;
                csv_out <<
                    "date,close,MA\n"
                    "2024-01-01,10,9\n"
                    "2024-01-02,11,10\n";
            }

            AppCommand cmd{};
            cmd.kind = CommandKind::PlanReport;
            cmd.plan_report.layout_path = layout_path.string();
            cmd.plan_report.logic_path = logic_path.string();
            cmd.plan_report.csv_path = csv_path.string();

            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_plan_report_command(cmd, out, err);
            const std::string text = out.str();

            t.check(rc == 1, "plan-report rejects wrong-case column refs");
            t.check(err.str().empty(), "wrong-case column compatibility failure stays in report output");
            t.check(text.find("Plan compatibility (post-binding runtime-target)") != std::string::npos,
                "plan-report labels the runtime-target compatibility status");
            t.check(text.find("missing runtime column: ma (available columns: close, MA)") != std::string::npos,
                "plan-report missing-column diagnostic lists available columns inline");
            return t.failed == 0;
        }

        bool test_plan_report_core_param_unknown_op_has_repair_hint(TestContext& t) {
            t.out << "[app_ux] plan-report core.param repair hint\n";
            std::string fixture_err;
            const auto fixture_dir = app_fixture_dir(fixture_err);
            t.check(!fixture_dir.empty(), "core.param hint fixture dir is available");
            if (fixture_dir.empty()) return false;

            const auto layout_path = fixture_dir / "app_tests_tmp_core_param_hint_layout.json";
            const auto logic_path = fixture_dir / "app_tests_tmp_core_param_hint_logic.json";
            const auto csv_path = fixture_dir / "app_tests_tmp_core_param_hint.csv";

            {
                std::ofstream layout_out(layout_path, std::ios::binary | std::ios::trunc);
                t.check(static_cast<bool>(layout_out), "core.param hint layout file opens");
                if (!layout_out) return false;
                layout_out <<
                    "{\n"
                    "  \"ingest\": {\n"
                    "    \"time_col\": \"date\",\n"
                    "    \"time_kind\": \"YYYY-MM-DD\",\n"
                    "    \"value_cols\": [\"close\"],\n"
                    "    \"primary_value_col\": \"close\"\n"
                    "  }\n"
                    "}\n";
            }

            {
                std::ofstream logic_out(logic_path, std::ios::binary | std::ios::trunc);
                t.check(static_cast<bool>(logic_out), "core.param hint logic file opens");
                if (!logic_out) return false;
                logic_out <<
                    "{\n"
                    "  \"version\": 1,\n"
                    "  \"trade_mode\": \"LongOnly\",\n"
                    "  \"params\": { \"threshold\": 10.0 },\n"
                    "  \"numeric_nodes\": [\n"
                    "    { \"id\": \"c\", \"op\": \"core.close\" },\n"
                    "    { \"id\": \"threshold_node\", \"op\": \"core.param\", \"params\": { \"name\": \"threshold\" } }\n"
                    "  ],\n"
                    "  \"bool_nodes\": [\n"
                    "    { \"id\": \"entry\", \"op\": \"cmp.gt\", \"args\": [\"c\", \"threshold_node\"] }\n"
                    "  ],\n"
                    "  \"bindings\": { \"entry_long\": \"entry\", \"exit_long\": \"\", \"entry_short\": \"\", \"exit_short\": \"\", \"allow_trade\": \"\" }\n"
                    "}\n";
            }

            {
                std::ofstream csv_out(csv_path, std::ios::binary | std::ios::trunc);
                t.check(static_cast<bool>(csv_out), "core.param hint csv file opens");
                if (!csv_out) return false;
                csv_out <<
                    "date,close\n"
                    "2024-01-01,10\n"
                    "2024-01-02,11\n";
            }

            AppCommand cmd{};
            cmd.kind = CommandKind::PlanReport;
            cmd.plan_report.layout_path = layout_path.string();
            cmd.plan_report.logic_path = logic_path.string();
            cmd.plan_report.csv_path = csv_path.string();

            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_plan_report_command(cmd, out, err);
            const std::string text = out.str();

            t.check(rc == 1, "plan-report rejects core.param with repair hint");
            t.check(err.str().empty(), "core.param compatibility failure stays in report output");
            t.check(text.find("unknown operator: core.param") != std::string::npos &&
                    text.find("use core.const with params.value={\"ref\":\"param_name\"}") != std::string::npos,
                "plan-report suggests the core.const parameter-ref pattern for core.param");
            return t.failed == 0;
        }

        bool test_plan_report_shows_dynamic_execution_validation(TestContext& t) {
            t.out << "[app_ux] plan-report dynamic execution validation block\n";
            const fs::path repo_root = find_repo_root();
            t.check(!repo_root.empty(), "repo root is found for dynamic plan-report demo");
            if (repo_root.empty()) return false;

            AppCommand cmd{};
            cmd.kind = CommandKind::PlanReport;
            cmd.plan_report.layout_path =
                (repo_root / "graphs" / "demos" / "quickstart_ohlcv.layout.json").string();
            cmd.plan_report.logic_path =
                (repo_root / "graphs" / "demos" / "dynamic_execution" /
                    "g10_row_varying_declared_dynamic.planspec.json").string();
            cmd.plan_report.csv_path = (repo_root / "data" / "sample.csv").string();

            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_plan_report_command(cmd, out, err);
            const std::string text = out.str();

            t.check(rc == 0, "valid declared_dynamic plan-report returns success");
            t.check(err.str().empty(), "valid declared_dynamic plan-report keeps stderr quiet");
            const auto compatibility_pos = text.find("Plan compatibility (post-binding runtime-target)");
            const auto execution_pos = text.find("Execution validation (post-binding runtime-target)");
            t.check(execution_pos != std::string::npos &&
                    text.find("mode: declared_dynamic") != std::string::npos &&
                    text.find("status: valid") != std::string::npos &&
                    text.find("subgraphs: 2") != std::string::npos &&
                    text.find("routes: 1") != std::string::npos &&
                    text.find("outputs: 1") != std::string::npos &&
                    text.find("bounds.decision_sample_limit: 16") != std::string::npos &&
                    text.find("static_validator: passed") != std::string::npos,
                "plan-report summarizes declared_dynamic shape and static validator status");
            t.check(compatibility_pos != std::string::npos && compatibility_pos < execution_pos,
                "plan-report surfaces execution validation beside successful runtime-target compatibility");
            return t.failed == 0;
        }

        bool test_plan_report_rejects_dynamic_execution_structural_errors(TestContext& t) {
            t.out << "[app_ux] plan-report declared_dynamic structural validation\n";
            RunFixturePaths fixture{};
            std::string fixture_err;
            const bool fixture_ok = create_unified_single_series_fixture(fixture, fixture_err);
            t.check(fixture_ok, "dynamic validation fixture files are created");
            if (!fixture_ok) return false;

            const auto fixture_dir = app_fixture_dir(fixture_err);
            t.check(!fixture_dir.empty(), "dynamic validation fixture dir is available");
            if (fixture_dir.empty()) return false;

            struct InvalidDynamicCase {
                std::string name;
                std::string json;
                std::string expected_code;
                std::string expected_subject;
            };

            const std::vector<InvalidDynamicCase> cases = {
                InvalidDynamicCase{
                    "unknown_condition",
                    make_dynamic_validation_plan_json(
                        "missing_condition",
                        "selected",
                        "selected_entry"),
                    "unknown_bool_condition_ref",
                    "missing_condition",
                },
                InvalidDynamicCase{
                    "unknown_subgraph",
                    make_dynamic_validation_plan_json(
                        "route_high",
                        "missing_subgraph",
                        "selected_entry"),
                    "unknown_subgraph_ref",
                    "missing_subgraph",
                },
                InvalidDynamicCase{
                    "missing_output_owner",
                    make_dynamic_validation_plan_json(
                        "route_high",
                        "selected",
                        "route_high"),
                    "missing_output_node_ownership",
                    "selected_entry",
                },
            };

            for (const auto& c : cases) {
                const fs::path logic_path =
                    fixture_dir / ("app_tests_tmp_dynamic_validation_" + c.name + ".json");
                {
                    std::ofstream logic_out(logic_path, std::ios::binary | std::ios::trunc);
                    t.check(static_cast<bool>(logic_out), "dynamic validation logic file opens: " + c.name);
                    if (!logic_out) return false;
                    logic_out << c.json;
                }

                AppCommand cmd{};
                cmd.kind = CommandKind::PlanReport;
                cmd.plan_report.layout_path = fixture.layout_path;
                cmd.plan_report.logic_path = logic_path.string();
                cmd.plan_report.csv_path = fixture.csv_path;

                std::ostringstream out;
                std::ostringstream err;
                const int rc = run_plan_report_command(cmd, out, err);
                const std::string text = out.str();

                t.check(rc == 1, "plan-report rejects declared_dynamic structural error: " + c.name);
                t.check(err.str().empty(), "declared_dynamic validation failure stays in report output: " + c.name);
                t.check(text.find("Execution validation") != std::string::npos &&
                        text.find("status: invalid") != std::string::npos &&
                        text.find("static_validator: failed") != std::string::npos &&
                        text.find(c.expected_code) != std::string::npos &&
                        text.find(c.expected_subject) != std::string::npos,
                    "plan-report prints declared_dynamic validation issue: " + c.name);
                t.check(text.find("Plan compatibility (post-binding runtime-target)") == std::string::npos,
                    "plan-report stops before compatibility after invalid execution metadata: " + c.name);
            }

            return t.failed == 0;
        }

        bool test_plan_report_flags_empty_entry_bindings(TestContext& t) {
            t.out << "[app_ux] plan-report empty entry binding note\n";
            RunFixturePaths fixture{};
            std::string fixture_err;
            const bool fixture_ok = create_unified_single_series_fixture(fixture, fixture_err);
            t.check(fixture_ok, "empty-binding fixture files are created");
            if (!fixture_ok) return false;

            const auto fixture_dir = app_fixture_dir(fixture_err);
            t.check(!fixture_dir.empty(), "empty-binding fixture dir is available");
            if (fixture_dir.empty()) return false;

            const auto logic_path = fixture_dir / "app_tests_tmp_empty_bindings_logic.json";
            {
                std::ofstream logic_out(logic_path, std::ios::binary | std::ios::trunc);
                t.check(static_cast<bool>(logic_out), "empty-binding logic file opens");
                if (!logic_out) return false;
                logic_out <<
                    "{\n"
                    "  \"version\": 1,\n"
                    "  \"trade_mode\": \"LongOnly\",\n"
                    "  \"numeric_nodes\": [\n"
                    "    { \"id\": \"c\", \"op\": \"core.close\" },\n"
                    "    { \"id\": \"z\", \"op\": \"core.const\", \"params\": { \"value\": 0.0 } }\n"
                    "  ],\n"
                    "  \"bool_nodes\": [\n"
                    "    { \"id\": \"entry\", \"op\": \"cmp.gt\", \"args\": [\"c\", \"z\"] }\n"
                    "  ],\n"
                    "  \"bindings\": {}\n"
                    "}\n";
            }

            AppCommand cmd{};
            cmd.kind = CommandKind::PlanReport;
            cmd.plan_report.layout_path = fixture.layout_path;
            cmd.plan_report.logic_path = logic_path.string();
            cmd.plan_report.csv_path = fixture.csv_path;

            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_plan_report_command(cmd, out, err);
            const std::string text = out.str();

            t.check(rc == 0, "empty-bindings plan-report remains compatibility-valid");
            t.check(err.str().empty(), "empty-bindings plan-report keeps stderr quiet");
            t.check(text.find("Plan binding notes") != std::string::npos &&
                    text.find("no entry bindings are active") != std::string::npos &&
                    text.find("cannot open positions") != std::string::npos,
                "plan-report warns that empty bindings will produce no entries");
            return t.failed == 0;
        }

    } // namespace

    void register_ux_diagnostic_tests(AppTestRegistry& tests) {
        tests.push_back(test_dump_plan_power_user_diagnostic_guidance);
        tests.push_back(test_plan_parser_explains_run_objective_string_shortcut);
        tests.push_back(test_plan_report_missing_column_case_lists_available_columns);
        tests.push_back(test_plan_report_core_param_unknown_op_has_repair_hint);
        tests.push_back(test_plan_report_shows_dynamic_execution_validation);
        tests.push_back(test_plan_report_rejects_dynamic_execution_structural_errors);
        tests.push_back(test_plan_report_flags_empty_entry_bindings);
    }

} // namespace sugar::app::tests
