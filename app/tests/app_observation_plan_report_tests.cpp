#include "tests/app_test_support.h"

#include <fstream>
#include <ios>
#include <sstream>
#include <string>

#include "commands/app_commands.h"

namespace sugar::app::tests {

    namespace {

        bool test_obs_report_dispatch_smoke(TestContext& t) {
            t.out << "[app_dispatch] obs-report smoke\n";
            RunFixturePaths fixture{};
            std::string fixture_err;
            const bool fixture_ok = create_unified_single_series_fixture(fixture, fixture_err);
            t.check(fixture_ok, "obs-report fixture files are created");
            if (!fixture_ok) return false;

            AppCommand cmd{};
            cmd.kind = CommandKind::ObsReport;
            cmd.obs_report.layout_path = fixture.layout_path;
            cmd.obs_report.csv_path = fixture.csv_path;

            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_obs_report_command(cmd, out, err);
            const std::string text = out.str();

            t.check(rc == 0, "obs-report command returns success");
            t.check(err.str().empty(), "obs-report command produces no error output");
            t.check(text.find("[Info] obs-report active.") != std::string::npos,
                "obs-report output includes active banner");
            t.check(text.find("Observation world provenance") != std::string::npos,
                "obs-report output includes observation-world provenance");
            t.check(text.find("Parsed CSV/layout confirmation") != std::string::npos &&
                    text.find("parsed_rows: 2") != std::string::npos &&
                    text.find("time_column: date (YYYY-MM-DD; internal=YyyyMmDd)") != std::string::npos &&
                    text.find("value_columns: close") != std::string::npos &&
                    text.find("primary_value_column: close (single value column)") != std::string::npos,
                "obs-report echoes parsed layout facts in beginner-readable terms");
            t.check(text.find("Input diagnostics") != std::string::npos,
                "obs-report output includes input diagnostics");
            t.check(text.find("Observation capability") != std::string::npos,
                "obs-report output includes observation capability");
            t.check(text.find("Plan compatibility") == std::string::npos,
                "obs-report output does not include plan compatibility");
            return t.failed == 0;
        }

        bool test_obs_report_json_success_is_machine_readable(TestContext& t) {
            t.out << "[app_dispatch] obs-report JSON success\n";
            RunFixturePaths fixture{};
            std::string fixture_err;
            const bool fixture_ok = create_unified_single_series_fixture(fixture, fixture_err);
            t.check(fixture_ok, "obs-report JSON fixture files are created");
            if (!fixture_ok) return false;

            AppCommand cmd{};
            cmd.kind = CommandKind::ObsReport;
            cmd.obs_report.layout_path = fixture.layout_path;
            cmd.obs_report.csv_path = fixture.csv_path;
            cmd.obs_report.json_output = true;

            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_obs_report_command(cmd, out, err);
            const std::string text = out.str();

            t.check(rc == 0, "obs-report JSON command returns success");
            t.check(err.str().empty(), "obs-report JSON success keeps stderr quiet");
            t.check(text.find("\"schema_version\":\"sugarbot.observation_report.v1\"") != std::string::npos &&
                    text.find("\"kind\":\"observation_report\"") != std::string::npos &&
                    text.find("\"ok\":true") != std::string::npos,
                "obs-report JSON emits the stable observation report envelope");
            t.check(text.find("\"capabilities\":[") != std::string::npos &&
                    text.find("\"code\":\"spatial_schema_can_be_provided\"") != std::string::npos,
                "obs-report JSON includes stable observation capability reason codes");
            t.check(text.find("\"series_retention\":[") != std::string::npos &&
                    text.find("\"raw_rows\":2") != std::string::npos &&
                    text.find("\"aligned_rows\":2") != std::string::npos,
                "obs-report JSON includes per-series raw/aligned row retention counts");
            t.check(text.find("[Info] obs-report active.") == std::string::npos &&
                    text.find("Observation world provenance") == std::string::npos,
                "obs-report JSON suppresses human prose");
            return t.failed == 0;
        }

        bool test_obs_report_json_numeric_parse_categories(TestContext& t) {
            t.out << "[app_dispatch] obs-report JSON numeric parse categories\n";
            std::string fixture_err;
            const auto fixture_dir = app_fixture_dir(fixture_err);
            t.check(!fixture_dir.empty(), "numeric parse fixture directory is available");
            if (fixture_dir.empty()) return false;

            const auto layout_path = fixture_dir / "app_tests_tmp_obs_json_numeric_layout.json";
            const auto csv_path = fixture_dir / "app_tests_tmp_obs_json_numeric.csv";

            {
                std::ofstream layout_out(layout_path, std::ios::binary | std::ios::trunc);
                t.check(static_cast<bool>(layout_out), "numeric parse layout file opens");
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
                std::ofstream csv_out(csv_path, std::ios::binary | std::ios::trunc);
                t.check(static_cast<bool>(csv_out), "numeric parse csv file opens");
                if (!csv_out) return false;
                csv_out <<
                    "date,close\n"
                    "2024-01-01,$10\n"
                    "2024-01-02,12%\n"
                    "2024-01-03,10k\n"
                    "2024-01-04,(14)\n";
            }

            AppCommand cmd{};
            cmd.kind = CommandKind::ObsReport;
            cmd.obs_report.layout_path = layout_path.string();
            cmd.obs_report.csv_path = csv_path.string();
            cmd.obs_report.json_output = true;

            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_obs_report_command(cmd, out, err);
            const std::string text = out.str();

            t.check(rc == 1, "obs-report JSON fails on invalid numeric tokens");
            t.check(err.str().empty(), "obs-report JSON numeric failure keeps stderr quiet");
            t.check(text.find("\"ok\":false") != std::string::npos &&
                    text.find("\"code\":\"preprocess.value_parse_failed\"") != std::string::npos,
                "obs-report JSON numeric failure emits a stable error code");
            t.check(text.find("\"category\":\"currency_symbol\"") != std::string::npos &&
                    text.find("\"category\":\"percent\"") != std::string::npos &&
                    text.find("\"category\":\"suffix\"") != std::string::npos &&
                    text.find("\"category\":\"parenthesized_negative\"") != std::string::npos,
                "obs-report JSON classifies common numeric token repair categories");
            t.check(text.find("\"csv_data_row\":1") != std::string::npos &&
                    text.find("\"token\":\"$10\"") != std::string::npos,
                "obs-report JSON includes bounded row/token samples");
            return t.failed == 0;
        }

        bool test_obs_report_json_duplicate_time_key_counts(TestContext& t) {
            t.out << "[app_dispatch] obs-report JSON duplicate time keys\n";
            std::string fixture_err;
            const auto fixture_dir = app_fixture_dir(fixture_err);
            t.check(!fixture_dir.empty(), "duplicate time-key fixture directory is available");
            if (fixture_dir.empty()) return false;

            const auto layout_path = fixture_dir / "app_tests_tmp_obs_json_duplicate_layout.json";
            const auto csv_path = fixture_dir / "app_tests_tmp_obs_json_duplicate.csv";

            {
                std::ofstream layout_out(layout_path, std::ios::binary | std::ios::trunc);
                t.check(static_cast<bool>(layout_out), "duplicate layout file opens");
                if (!layout_out) return false;
                layout_out <<
                    "{\n"
                    "  \"ingest\": {\n"
                    "    \"time_col\": \"date\",\n"
                    "    \"time_kind\": \"YYYY-MM-DD\",\n"
                    "    \"value_cols\": [\"close\"],\n"
                    "    \"primary_value_col\": \"close\",\n"
                    "    \"dedupe\": \"Error\"\n"
                    "  }\n"
                    "}\n";
            }

            {
                std::ofstream csv_out(csv_path, std::ios::binary | std::ios::trunc);
                t.check(static_cast<bool>(csv_out), "duplicate csv file opens");
                if (!csv_out) return false;
                csv_out <<
                    "date,close\n"
                    "2024-01-01,10\n"
                    "2024-01-01,11\n"
                    "2024-01-02,12\n";
            }

            AppCommand cmd{};
            cmd.kind = CommandKind::ObsReport;
            cmd.obs_report.layout_path = layout_path.string();
            cmd.obs_report.csv_path = csv_path.string();
            cmd.obs_report.json_output = true;

            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_obs_report_command(cmd, out, err);
            const std::string text = out.str();

            t.check(rc == 1, "obs-report JSON fails on duplicate time keys");
            t.check(err.str().empty(), "obs-report JSON duplicate failure keeps stderr quiet");
            t.check(text.find("\"code\":\"preprocess.duplicate_time_key\"") != std::string::npos &&
                    text.find("\"key\":\"20240101\"") != std::string::npos &&
                    text.find("\"count\":2") != std::string::npos,
                "obs-report JSON reports duplicate key counts");
            t.check(text.find("\"csv_data_row\":1") != std::string::npos &&
                    text.find("\"csv_data_row\":2") != std::string::npos,
                "obs-report JSON reports duplicate sample row numbers");
            return t.failed == 0;
        }

        bool test_obs_report_accepts_friendly_time_alias_and_single_value_primary_inference(TestContext& t) {
            t.out << "[app_dispatch] obs-report friendly layout shorthand\n";
            std::string fixture_err;
            const auto fixture_dir = app_fixture_dir(fixture_err);
            t.check(!fixture_dir.empty(), "friendly shorthand fixture directory is available");
            if (fixture_dir.empty()) return false;

            const auto layout_path = fixture_dir / "app_tests_tmp_friendly_layout.json";
            const auto csv_path = fixture_dir / "app_tests_tmp_friendly_layout.csv";

            {
                std::ofstream layout_out(layout_path, std::ios::binary | std::ios::trunc);
                if (!layout_out) {
                    t.check(false, "friendly shorthand layout file is writable");
                    return false;
                }
                layout_out <<
                    "{\n"
                    "  \"ingest\": {\n"
                    "    \"time_col\": \"date\",\n"
                    "    \"time_kind\": \"YYYY-MM-DD\",\n"
                    "    \"header_policy\": \"RequiredHeaders\",\n"
                    "    \"value_cols\": [\"close\"]\n"
                    "  }\n"
                    "}\n";
            }

            {
                std::ofstream csv_out(csv_path, std::ios::binary | std::ios::trunc);
                if (!csv_out) {
                    t.check(false, "friendly shorthand CSV file is writable");
                    return false;
                }
                csv_out <<
                    "date,close\n"
                    "2024-01-01,10\n"
                    "2024-01-02,11\n";
            }

            AppCommand cmd{};
            cmd.kind = CommandKind::ObsReport;
            cmd.obs_report.layout_path = layout_path.string();
            cmd.obs_report.csv_path = csv_path.string();

            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_obs_report_command(cmd, out, err);
            const std::string text = out.str();

            t.check(rc == 0, "obs-report accepts friendly time alias and omitted single primary");
            t.check(err.str().empty(), "friendly shorthand obs-report produces no error output");
            t.check(text.find("time_column: date (YYYY-MM-DD; internal=YyyyMmDd)") != std::string::npos &&
                    text.find("value_columns: close") != std::string::npos &&
                    text.find("primary_value_column: close (single value column)") != std::string::npos,
                "obs-report confirms the inferred single-column primary and readable time kind");
            return t.failed == 0;
        }

        bool test_obs_report_warns_on_primary_only_value_shorthand(TestContext& t) {
            t.out << "[app_dispatch] obs-report primary-only value shorthand\n";
            std::string fixture_err;
            const auto fixture_dir = app_fixture_dir(fixture_err);
            t.check(!fixture_dir.empty(), "primary-only shorthand fixture directory is available");
            if (fixture_dir.empty()) return false;

            const auto layout_path = fixture_dir / "app_tests_tmp_primary_only_layout.json";
            const auto csv_path = fixture_dir / "app_tests_tmp_primary_only_layout.csv";

            {
                std::ofstream layout_out(layout_path, std::ios::binary | std::ios::trunc);
                if (!layout_out) {
                    t.check(false, "primary-only shorthand layout file is writable");
                    return false;
                }
                layout_out <<
                    "{\n"
                    "  \"ingest\": {\n"
                    "    \"time_col\": \"date\",\n"
                    "    \"time_kind\": \"YYYY-MM-DD\",\n"
                    "    \"header_policy\": \"RequiredHeaders\",\n"
                    "    \"primary_value_col\": \"measurement\"\n"
                    "  }\n"
                    "}\n";
            }

            {
                std::ofstream csv_out(csv_path, std::ios::binary | std::ios::trunc);
                if (!csv_out) {
                    t.check(false, "primary-only shorthand CSV file is writable");
                    return false;
                }
                csv_out <<
                    "date,measurement\n"
                    "2024-01-01,10\n"
                    "2024-01-02,11\n";
            }

            AppCommand cmd{};
            cmd.kind = CommandKind::ObsReport;
            cmd.obs_report.layout_path = layout_path.string();
            cmd.obs_report.csv_path = csv_path.string();

            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_obs_report_command(cmd, out, err);
            const std::string text = out.str();

            t.check(rc == 0, "obs-report accepts omitted value_cols when primary_value_col is supplied");
            t.check(err.str().empty(), "primary-only shorthand obs-report produces no error output");
            t.check(text.find("value_columns: measurement") != std::string::npos &&
                    text.find("primary_value_column: measurement (single value column)") != std::string::npos,
                "obs-report confirms the normalized single value column");
            t.check(text.find("layout_warning: SINGLE VALUE COLUMN ASSUMED") != std::string::npos &&
                    text.find("`value_cols` was omitted and inferred from `primary_value_col`") != std::string::npos,
                "obs-report emits a loud generic warning for primary-only shorthand");
            t.check(text.find("close") == std::string::npos,
                "primary-only shorthand warning does not teach a close-specific example");
            return t.failed == 0;
        }

        bool test_plan_report_dispatch_smoke(TestContext& t) {
            t.out << "[app_dispatch] plan-report smoke\n";
            RunFixturePaths fixture{};
            std::string fixture_err;
            const bool fixture_ok = create_unified_single_series_fixture(fixture, fixture_err);
            t.check(fixture_ok, "plan-report fixture files are created");
            if (!fixture_ok) return false;

            AppCommand cmd{};
            cmd.kind = CommandKind::PlanReport;
            cmd.plan_report.layout_path = fixture.layout_path;
            cmd.plan_report.logic_path = fixture.logic_path;
            cmd.plan_report.csv_path = fixture.csv_path;

            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_plan_report_command(cmd, out, err);
            const std::string text = out.str();

            t.check(rc == 0, "plan-report command returns success for valid single-series input");
            t.check(err.str().empty(), "plan-report command produces no error output for valid input");
            t.check(text.find("[Info] plan-report active.") != std::string::npos,
                "plan-report output includes active banner");
            t.check(text.find("=== [1/2] Pre-binding observation assessment ===") != std::string::npos,
                "plan-report output labels the pre-binding observation stage");
            t.check(text.find("=== DAG runtime binding ===") != std::string::npos,
                "plan-report output labels the runtime binding stage");
            t.check(text.find("=== [2/2] Post-binding runtime assessment ===") != std::string::npos,
                "plan-report output labels the post-binding runtime stage");
            t.check(text.find("Observation world provenance") != std::string::npos,
                "plan-report output includes observation-world provenance");
            t.check(text.find("Runtime provenance") != std::string::npos,
                "plan-report output includes runtime provenance for bound target");
            t.check(text.find("Input diagnostics") != std::string::npos,
                "plan-report output includes input diagnostics");
            t.check(text.find("Plan compatibility") != std::string::npos,
                "plan-report output includes plan compatibility");
            t.check(text.find("status: valid") != std::string::npos,
                "plan-report output preserves valid compatibility status");
            return t.failed == 0;
        }

        bool test_plan_report_surfaces_topology_query_requirements(TestContext& t) {
            t.out << "[app_dispatch] plan-report topology query requirements\n";
            std::string fixture_err;
            const auto fixture_dir = app_fixture_dir(fixture_err);
            t.check(!fixture_dir.empty(), "topology requirement fixture dir is available");
            if (fixture_dir.empty()) return false;

            const auto layout_path = fixture_dir / "app_tests_tmp_topology_plan_requirement_layout.json";
            const auto logic_path = fixture_dir / "app_tests_tmp_topology_plan_requirement_logic.json";
            const auto csv_path = fixture_dir / "app_tests_tmp_topology_plan_requirement.csv";

            {
                std::ofstream layout_out(layout_path, std::ios::binary | std::ios::trunc);
                t.check(static_cast<bool>(layout_out), "topology requirement layout file opens");
                if (!layout_out) return false;
                layout_out <<
                    "{\n"
                    "  \"ingest\": {\n"
                    "    \"time_col\": \"row_id\",\n"
                    "    \"time_kind\": \"Index\",\n"
                    "    \"header_policy\": \"RequiredHeaders\",\n"
                    "    \"value_cols\": [\"close\"],\n"
                    "    \"primary_value_col\": \"close\"\n"
                    "  },\n"
                    "  \"topologies\": [\n"
                    "    {\n"
                    "      \"name\": \"row.weighted.graph\",\n"
                    "      \"bearing\": \"query_bearing\",\n"
                    "      \"family\": \"relational_graph\",\n"
                    "      \"domain_scope\": \"row_domain\",\n"
                    "      \"query_kinds\": [\"neighbor_set\"],\n"
                    "      \"weighted\": true,\n"
                    "      \"node_count\": 3,\n"
                    "      \"edges\": [{ \"source\": 0, \"target\": 1, \"weight\": 0.5 }]\n"
                    "    }\n"
                    "  ]\n"
                    "}\n";
            }

            {
                std::ofstream logic_out(logic_path, std::ios::binary | std::ios::trunc);
                t.check(static_cast<bool>(logic_out), "topology requirement logic file opens");
                if (!logic_out) return false;
                logic_out <<
                    "{\n"
                    "  \"version\": 1,\n"
                    "  \"name\": \"app_tests_topology_query_requirement\",\n"
                    "  \"trade_mode\": \"LongOnly\",\n"
                    "  \"numeric_nodes\": [\n"
                    "    { \"id\": \"c\", \"op\": \"core.close\" },\n"
                    "    { \"id\": \"weighted\", \"op\": \"topology.weighted_sum\", \"args\": [\"c\"], \"params\": { \"topology\": \"row.weighted.graph\" } }\n"
                    "  ],\n"
                    "  \"bool_nodes\": [\n"
                    "    { \"id\": \"entry\", \"op\": \"cmp.gt\", \"args\": [\"weighted\", \"c\"] },\n"
                    "    { \"id\": \"exit\", \"op\": \"cmp.lt\", \"args\": [\"weighted\", \"c\"] }\n"
                    "  ],\n"
                    "  \"bindings\": { \"entry_long\": \"entry\", \"exit_long\": \"exit\", \"entry_short\": \"\", \"exit_short\": \"\", \"allow_trade\": \"\" }\n"
                    "}\n";
            }

            {
                std::ofstream csv_out(csv_path, std::ios::binary | std::ios::trunc);
                t.check(static_cast<bool>(csv_out), "topology requirement csv file opens");
                if (!csv_out) return false;
                csv_out <<
                    "row_id,close\n"
                    "0,10\n"
                    "1,11\n"
                    "2,12\n";
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

            t.check(rc == 1, "plan-report fails when topology lacks required query kind");
            t.check(err.str().empty(), "plan-report keeps topology capability failure in report output");
            t.check(text.find("required_query_kinds=weighted_neighbor_set") != std::string::npos,
                "plan-report prints operator-required topology query kinds");
            t.check(text.find("declared_query_kinds=neighbor_set") != std::string::npos,
                "plan-report prints declared topology query kinds");
            t.check(text.find("missing_query_kinds=weighted_neighbor_set") != std::string::npos,
                "plan-report flags missing topology query kinds");
            t.check(text.find("hint=add weighted_neighbor_set") != std::string::npos,
                "plan-report hints the weighted query declaration fix");
            return t.failed == 0;
        }

        bool test_plan_report_rejects_unknown_binding_key(TestContext& t) {
            t.out << "[app_dispatch] plan-report rejects unknown binding key\n";
            RunFixturePaths fixture{};
            std::string fixture_err;
            const bool fixture_ok = create_unified_single_series_fixture(fixture, fixture_err);
            t.check(fixture_ok, "unknown-binding fixture files are created");
            if (!fixture_ok) return false;

            const auto fixture_dir = app_fixture_dir(fixture_err);
            t.check(!fixture_dir.empty(), "unknown-binding fixture dir is available");
            if (fixture_dir.empty()) return false;
            const auto logic_path = fixture_dir / "app_tests_tmp_unknown_binding_key_logic.json";

            {
                std::ofstream logic_out(logic_path, std::ios::binary | std::ios::trunc);
                t.check(static_cast<bool>(logic_out), "unknown-binding logic file opens");
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
                    "  \"bindings\": { \"entry\": \"entry\" }\n"
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
            const std::string error_text = err.str();

            t.check(rc == 1, "plan-report rejects plans with unknown binding keys");
            t.check(error_text.find("bindings.entry is not supported") != std::string::npos &&
                    error_text.find("entry_long, exit_long, entry_short, exit_short, allow_trade") != std::string::npos,
                "unknown binding diagnostic names the valid strategy binding keys");
            return t.failed == 0;
        }

        bool test_plan_report_rejects_time_valid_topology_without_time_axis(TestContext& t) {
            t.out << "[app_dispatch] plan-report rejects time-valid topology without time axis\n";
            std::string fixture_err;
            const auto fixture_dir = app_fixture_dir(fixture_err);
            t.check(!fixture_dir.empty(), "time-valid topology fixture dir is available");
            if (fixture_dir.empty()) return false;

            const auto layout_path = fixture_dir / "app_tests_tmp_time_valid_no_time_layout.json";
            const auto logic_path = fixture_dir / "app_tests_tmp_time_valid_no_time_logic.json";
            const auto csv_path = fixture_dir / "app_tests_tmp_time_valid_no_time.csv";

            {
                std::ofstream layout_out(layout_path, std::ios::binary | std::ios::trunc);
                t.check(static_cast<bool>(layout_out), "time-valid no-time layout file opens");
                if (!layout_out) return false;
                layout_out <<
                    "{\n"
                    "  \"ingest\": {\n"
                    "    \"spatial_x_col\": \"x\",\n"
                    "    \"spatial_y_col\": \"y\",\n"
                    "    \"header_policy\": \"RequiredHeaders\",\n"
                    "    \"value_cols\": [\"close\"],\n"
                    "    \"primary_value_col\": \"close\"\n"
                    "  },\n"
                    "  \"topologies\": [\n"
                    "    {\n"
                    "      \"name\": \"row.time.graph\",\n"
                    "      \"bearing\": \"query_bearing\",\n"
                    "      \"family\": \"relational_graph\",\n"
                    "      \"domain_scope\": \"row_domain\",\n"
                    "      \"query_kinds\": [\"neighbor_set\"],\n"
                    "      \"node_count\": 3,\n"
                    "      \"edges\": [{ \"source\": 0, \"target\": 1, \"valid_from\": 20240101 }]\n"
                    "    }\n"
                    "  ]\n"
                    "}\n";
            }

            {
                std::ofstream logic_out(logic_path, std::ios::binary | std::ios::trunc);
                t.check(static_cast<bool>(logic_out), "time-valid no-time logic file opens");
                if (!logic_out) return false;
                logic_out <<
                    "{\n"
                    "  \"version\": 1,\n"
                    "  \"trade_mode\": \"LongOnly\",\n"
                    "  \"numeric_nodes\": [\n"
                    "    { \"id\": \"c\", \"op\": \"core.close\" },\n"
                    "    { \"id\": \"mean\", \"op\": \"topology.neighbor_mean\", \"args\": [\"c\"], \"params\": { \"topology\": \"row.time.graph\" } }\n"
                    "  ],\n"
                    "  \"bool_nodes\": [\n"
                    "    { \"id\": \"entry\", \"op\": \"cmp.gt\", \"args\": [\"mean\", \"c\"] }\n"
                    "  ],\n"
                    "  \"bindings\": { \"entry_long\": \"entry\", \"exit_long\": \"\", \"entry_short\": \"\", \"exit_short\": \"\", \"allow_trade\": \"\" }\n"
                    "}\n";
            }

            {
                std::ofstream csv_out(csv_path, std::ios::binary | std::ios::trunc);
                t.check(static_cast<bool>(csv_out), "time-valid no-time csv file opens");
                if (!csv_out) return false;
                csv_out <<
                    "x,y,close\n"
                    "0,0,10\n"
                    "1,0,11\n"
                    "2,0,12\n";
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

            t.check(rc == 1, "plan-report rejects time-valid topology edges without a time axis");
            t.check(err.str().empty(), "time-valid topology compatibility failure stays in report output");
            t.check(text.find("status: invalid") != std::string::npos &&
                    text.find("time-valid edges") != std::string::npos &&
                    text.find("time_col") != std::string::npos &&
                    text.find("time_kind") != std::string::npos,
                "plan-report explains that time-valid topology edges require layout time binding");
            return t.failed == 0;
        }

        bool test_plan_report_links_sparse_columns_to_nodes(TestContext& t) {
            t.out << "[app_dispatch] plan-report links sparse columns to nodes\n";
            std::string fixture_err;
            const auto fixture_dir = app_fixture_dir(fixture_err);
            t.check(!fixture_dir.empty(), "sparse-column plan fixture dir is available");
            if (fixture_dir.empty()) return false;

            const auto layout_path = fixture_dir / "app_tests_tmp_sparse_column_layout.json";
            const auto logic_path = fixture_dir / "app_tests_tmp_sparse_column_logic.json";
            const auto csv_path = fixture_dir / "app_tests_tmp_sparse_column.csv";

            {
                std::ofstream layout_out(layout_path, std::ios::binary | std::ios::trunc);
                t.check(static_cast<bool>(layout_out), "sparse-column layout file opens");
                if (!layout_out) return false;
                layout_out <<
                    "{\n"
                    "  \"ingest\": {\n"
                    "    \"time_col\": \"time\",\n"
                    "    \"time_kind\": \"YyyyMmDd\",\n"
                    "    \"value_cols\": [\"close\", \"MA\"],\n"
                    "    \"primary_value_col\": \"close\",\n"
                    "    \"missing_tokens\": [\"\", \"NA\", \"null\", \"nan\", \"NaN\"],\n"
                    "    \"missing_default\": { \"kind\": \"NaN\" }\n"
                    "  }\n"
                    "}\n";
            }

            {
                std::ofstream logic_out(logic_path, std::ios::binary | std::ios::trunc);
                t.check(static_cast<bool>(logic_out), "sparse-column logic file opens");
                if (!logic_out) return false;
                logic_out <<
                    "{\n"
                    "  \"version\": 1,\n"
                    "  \"trade_mode\": \"LongOnly\",\n"
                    "  \"numeric_nodes\": [\n"
                    "    { \"id\": \"ma_val\", \"op\": \"core.ref\", \"params\": { \"ref\": \"MA\" } },\n"
                    "    { \"id\": \"zero\", \"op\": \"core.const\", \"params\": { \"value\": 0.0 } }\n"
                    "  ],\n"
                    "  \"bool_nodes\": [\n"
                    "    { \"id\": \"entry\", \"op\": \"cmp.gt\", \"args\": [\"ma_val\", \"zero\"] }\n"
                    "  ],\n"
                    "  \"bindings\": { \"entry_long\": \"entry\", \"exit_long\": \"\", \"entry_short\": \"\", \"exit_short\": \"\", \"allow_trade\": \"\" }\n"
                    "}\n";
            }

            {
                std::ofstream csv_out(csv_path, std::ios::binary | std::ios::trunc);
                t.check(static_cast<bool>(csv_out), "sparse-column csv file opens");
                if (!csv_out) return false;
                csv_out <<
                    "time,close,MA\n"
                    "20240101,10,1\n"
                    "20240102,11,NA\n"
                    "20240103,12,\n"
                    "20240104,13,NaN\n";
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

            t.check(rc == 0, "sparse-column plan-report remains semantically valid");
            t.check(err.str().empty(), "sparse-column plan-report keeps stderr quiet");
            t.check(text.find("value column 'MA' has 75% missing values after preprocessing (3/4 rows)") != std::string::npos,
                "plan-report carries the sparse-column ingest warning");
            t.check(text.find("Plan data-quality notes") != std::string::npos &&
                    text.find("numeric node ma_val (core.ref) reads value column 'MA'") != std::string::npos &&
                    text.find("75% missing values") != std::string::npos,
                "plan-report connects sparse columns to referencing nodes");
            return t.failed == 0;
        }

        bool test_obs_report_exposes_drop_row_diagnostics(TestContext& t) {
            t.out << "[app_dispatch] obs-report drop-row diagnostics\n";
            RunFixturePaths fixture{};
            std::string fixture_err;
            const bool fixture_ok = create_unified_drop_row_fixture(fixture, fixture_err);
            t.check(fixture_ok, "drop-row obs-report fixture files are created");
            if (!fixture_ok) return false;

            AppCommand cmd{};
            cmd.kind = CommandKind::ObsReport;
            cmd.obs_report.layout_path = fixture.layout_path;
            cmd.obs_report.csv_path = fixture.csv_path;

            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_obs_report_command(cmd, out, err);
            const std::string text = out.str();

            t.check(rc == 0, "drop-row obs-report command returns success");
            t.check(err.str().empty(), "drop-row obs-report command produces no error output");
            t.check(text.find("csv_rows: 3") != std::string::npos,
                "drop-row obs-report reports data-row count");
            t.check(text.find("blank_rows_skipped: 1") != std::string::npos,
                "drop-row obs-report reports blank rows skipped");
            t.check(text.find("rows_dropped: 1") != std::string::npos,
                "drop-row obs-report reports dropped rows");
            t.check(text.find("drop-row missing policy removed 1 row(s)") != std::string::npos,
                "drop-row obs-report preserves input warning text");
            return t.failed == 0;
        }

        bool test_obs_report_warns_on_degenerate_value_columns(TestContext& t) {
            t.out << "[app_dispatch] obs-report degenerate value-column warnings\n";
            std::string fixture_err;
            const auto fixture_dir = app_fixture_dir(fixture_err);
            t.check(!fixture_dir.empty(), "degenerate value-column fixture dir is available");
            if (fixture_dir.empty()) return false;

            const auto layout_path = fixture_dir / "app_tests_tmp_degenerate_columns_layout.json";
            const auto csv_path = fixture_dir / "app_tests_tmp_degenerate_columns.csv";

            {
                std::ofstream layout_out(layout_path, std::ios::binary | std::ios::trunc);
                t.check(static_cast<bool>(layout_out), "degenerate value-column layout file opens");
                if (!layout_out) return false;
                layout_out <<
                    "{\n"
                    "  \"ingest\": {\n"
                    "    \"time_col\": \"time\",\n"
                    "    \"time_kind\": \"YyyyMmDd\",\n"
                    "    \"value_cols\": [\"close\", \"volume\", \"ma\", \"sparse\"],\n"
                    "    \"primary_value_col\": \"close\",\n"
                    "    \"missing_tokens\": [\"\", \"NA\", \"null\", \"nan\", \"NaN\"],\n"
                    "    \"missing_default\": { \"kind\": \"NaN\" }\n"
                    "  }\n"
                    "}\n";
            }

            {
                std::ofstream csv_out(csv_path, std::ios::binary | std::ios::trunc);
                t.check(static_cast<bool>(csv_out), "degenerate value-column csv file opens");
                if (!csv_out) return false;
                csv_out <<
                    "time,close,volume,ma,sparse\n"
                    "20240101,10,0,NaN,1\n"
                    "20240102,11,0,NA,NA\n"
                    "20240103,12,0,,\n"
                    "20240104,13,0,null,NaN\n";
            }

            AppCommand cmd{};
            cmd.kind = CommandKind::ObsReport;
            cmd.obs_report.layout_path = layout_path.string();
            cmd.obs_report.csv_path = csv_path.string();

            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_obs_report_command(cmd, out, err);
            const std::string text = out.str();

            t.check(rc == 0, "degenerate value-column obs-report returns success");
            t.check(err.str().empty(), "degenerate value-column obs-report produces no error output");
            t.check(text.find("value column 'volume' is uniformly zero after preprocessing") != std::string::npos,
                "obs-report warns on uniformly-zero value columns");
            t.check(text.find("value column 'ma' contains no finite values after preprocessing") != std::string::npos,
                "obs-report warns on all-NaN value columns");
            t.check(text.find("value column 'sparse' has 75% missing values after preprocessing (3/4 rows)") != std::string::npos,
                "obs-report warns on mostly missing value columns");
            t.check(text.find("value_column_quality:") != std::string::npos &&
                    text.find("- sparse: missing=3/4 (75%), finite=1/4") != std::string::npos,
                "obs-report prints per-column missingness statistics");
            return t.failed == 0;
        }

        bool test_obs_report_time_parse_failure_includes_time_kind_hint(TestContext& t) {
            t.out << "[app_dispatch] obs-report time parse hint\n";
            std::string fixture_err;
            const auto fixture_dir = app_fixture_dir(fixture_err);
            t.check(!fixture_dir.empty(), "time parse hint fixture dir is available");
            if (fixture_dir.empty()) return false;

            const auto layout_path = fixture_dir / "app_tests_tmp_time_parse_hint_layout.json";
            const auto csv_path = fixture_dir / "app_tests_tmp_time_parse_hint.csv";

            {
                std::ofstream layout_out(layout_path, std::ios::binary | std::ios::trunc);
                t.check(static_cast<bool>(layout_out), "time parse hint layout file opens");
                if (!layout_out) return false;
                layout_out <<
                    "{\n"
                    "  \"ingest\": {\n"
                    "    \"time_col\": \"time\",\n"
                    "    \"time_kind\": \"Iso8601\",\n"
                    "    \"value_cols\": [\"close\"],\n"
                    "    \"primary_value_col\": \"close\"\n"
                    "  }\n"
                    "}\n";
            }

            {
                std::ofstream csv_out(csv_path, std::ios::binary | std::ios::trunc);
                t.check(static_cast<bool>(csv_out), "time parse hint csv file opens");
                if (!csv_out) return false;
                csv_out <<
                    "time,close\n"
                    "2021-10-14,10\n";
            }

            AppCommand cmd{};
            cmd.kind = CommandKind::ObsReport;
            cmd.obs_report.layout_path = layout_path.string();
            cmd.obs_report.csv_path = csv_path.string();

            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_obs_report_command(cmd, out, err);

            t.check(rc == 1, "obs-report fails for date-only values under Iso8601");
            t.check(err.str().find("token looks date-only; use time_kind \"YyyyMmDd\"") != std::string::npos,
                "obs-report time parse failure suggests the correct time_kind");
            return t.failed == 0;
        }

        bool test_obs_report_surfaces_topology_layout_warnings(TestContext& t) {
            t.out << "[app_dispatch] obs-report topology layout warnings\n";
            std::string fixture_err;
            const auto fixture_dir = app_fixture_dir(fixture_err);
            t.check(!fixture_dir.empty(), "topology warning fixture dir is available");
            if (fixture_dir.empty()) return false;

            const auto layout_path = fixture_dir / "app_tests_tmp_topology_obs_warning_layout.json";
            const auto csv_path = fixture_dir / "app_tests_tmp_topology_obs_warning.csv";

            {
                std::ofstream layout_out(layout_path, std::ios::binary | std::ios::trunc);
                t.check(static_cast<bool>(layout_out), "topology warning layout file opens");
                if (!layout_out) return false;
                layout_out <<
                    "{\n"
                    "  \"ingest\": {\n"
                    "    \"time_col\": \"row_id\",\n"
                    "    \"time_kind\": \"Index\",\n"
                    "    \"header_policy\": \"RequiredHeaders\",\n"
                    "    \"value_cols\": [\"close\"],\n"
                    "    \"primary_value_col\": \"close\"\n"
                    "  },\n"
                    "  \"topologies\": [\n"
                    "    {\n"
                    "      \"name\": \"row.warning.graph\",\n"
                    "      \"bearing\": \"query_bearing\",\n"
                    "      \"family\": \"relational_graph\",\n"
                    "      \"domain_scope\": \"row_domain\",\n"
                    "      \"query_kinds\": [\"neighbor_set\"],\n"
                    "      \"weighted\": true,\n"
                    "      \"node_count\": 2,\n"
                    "      \"edges\": [{ \"source\": 0, \"target\": 1, \"weight\": 0.5 }]\n"
                    "    },\n"
                    "    {\n"
                    "      \"name\": \"row.warning.semantic\",\n"
                    "      \"bearing\": \"query_bearing\",\n"
                    "      \"family\": \"semantic_graph\",\n"
                    "      \"domain_scope\": \"row_domain\",\n"
                    "      \"query_kinds\": [\"neighbor_set\"],\n"
                    "      \"node_count\": 3,\n"
                    "      \"embeddings\": [\n"
                    "        { \"node\": 0, \"vector\": [0.0, 0.0] },\n"
                    "        { \"node\": 1, \"vector\": [1.0, 0.0, 9.0] },\n"
                    "        { \"node\": 2, \"vector\": [2.0, 0.0] }\n"
                    "      ]\n"
                    "    }\n"
                    "  ]\n"
                    "}\n";
            }

            {
                std::ofstream csv_out(csv_path, std::ios::binary | std::ios::trunc);
                t.check(static_cast<bool>(csv_out), "topology warning csv file opens");
                if (!csv_out) return false;
                csv_out <<
                    "row_id,close\n"
                    "0,10\n"
                    "1,11\n"
                    "2,12\n";
            }

            AppCommand cmd{};
            cmd.kind = CommandKind::ObsReport;
            cmd.obs_report.layout_path = layout_path.string();
            cmd.obs_report.csv_path = csv_path.string();

            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_obs_report_command(cmd, out, err);
            const std::string text = out.str();

            t.check(rc == 0, "topology warning obs-report returns success");
            t.check(err.str().empty(), "topology warning obs-report keeps stderr quiet");
            t.check(text.find("parsed_columns: row_id, close") != std::string::npos,
                "obs-report prints parsed column names");
            t.check(text.find("row_domain_size: 3") != std::string::npos,
                "obs-report prints resolved row-domain size");
            t.check(text.find("domain_scope=row_domain means topology node ids index the aligned row domain") != std::string::npos,
                "obs-report explains row-domain topology indexing");
            t.check(text.find("warning: explicit node_count=2 differs from aligned row_domain_size=3") != std::string::npos,
                "obs-report warns on topology node_count mismatch");
            t.check(text.find("weighted edge payload is present but query_kinds does not declare weighted_neighbor_set") != std::string::npos,
                "obs-report warns when weighted edge data lacks weighted query capability");
            t.check(text.find("semantic embedding vector dimensions differ") != std::string::npos &&
                    text.find("node=1 has dimension 3, expected 2") != std::string::npos,
                "obs-report flags malformed semantic embedding dimensions before plan-report");
            return t.failed == 0;
        }

        bool test_obs_report_flags_time_valid_edges_without_time_axis(TestContext& t) {
            t.out << "[app_dispatch] obs-report topology time-valid edge warning\n";
            std::string fixture_err;
            const auto fixture_dir = app_fixture_dir(fixture_err);
            t.check(!fixture_dir.empty(), "time-valid topology warning fixture dir is available");
            if (fixture_dir.empty()) return false;

            const auto layout_path = fixture_dir / "app_tests_tmp_topology_time_valid_no_time_layout.json";
            const auto csv_path = fixture_dir / "app_tests_tmp_topology_time_valid_no_time.csv";

            {
                std::ofstream layout_out(layout_path, std::ios::binary | std::ios::trunc);
                t.check(static_cast<bool>(layout_out), "time-valid no-time layout file opens");
                if (!layout_out) return false;
                layout_out <<
                    "{\n"
                    "  \"ingest\": {\n"
                    "    \"spatial_x_col\": \"x\",\n"
                    "    \"spatial_y_col\": \"y\",\n"
                    "    \"value_cols\": [\"close\"],\n"
                    "    \"primary_value_col\": \"close\"\n"
                    "  },\n"
                    "  \"topologies\": [\n"
                    "    {\n"
                    "      \"name\": \"row.spatial.time_valid\",\n"
                    "      \"bearing\": \"query_bearing\",\n"
                    "      \"family\": \"relational_graph\",\n"
                    "      \"domain_scope\": \"row_domain\",\n"
                    "      \"query_kinds\": [\"neighbor_set\"],\n"
                    "      \"edges\": [\n"
                    "        { \"source\": 0, \"target\": 1, \"valid_from\": 20240101, \"valid_to\": 20240104 },\n"
                    "        { \"source\": 1, \"target\": 2 }\n"
                    "      ]\n"
                    "    }\n"
                    "  ]\n"
                    "}\n";
            }

            {
                std::ofstream csv_out(csv_path, std::ios::binary | std::ios::trunc);
                t.check(static_cast<bool>(csv_out), "time-valid no-time csv file opens");
                if (!csv_out) return false;
                csv_out <<
                    "x,y,close\n"
                    "0,0,10\n"
                    "1,0,11\n"
                    "2,0,12\n";
            }

            AppCommand cmd{};
            cmd.kind = CommandKind::ObsReport;
            cmd.obs_report.layout_path = layout_path.string();
            cmd.obs_report.csv_path = csv_path.string();

            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_obs_report_command(cmd, out, err);
            const std::string text = out.str();

            t.check(rc == 0, "time-valid no-time obs-report returns success");
            t.check(err.str().empty(), "time-valid no-time obs-report keeps stderr quiet");
            t.check(text.find("has_time_axis: false") != std::string::npos &&
                    text.find("has_spatial_axis: true") != std::string::npos,
                "obs-report confirms this is a spatial layout without time");
            t.check(text.find("time-valid edges are declared with valid_from/valid_to") != std::string::npos &&
                    text.find("spatial axes do not provide graph time") != std::string::npos &&
                    text.find("Add `time_col`/`time_kind`") != std::string::npos,
                "obs-report gives topology-specific guidance for time-valid edges without a time axis");
            return t.failed == 0;
        }

        bool test_plan_report_reports_multi_entity_missing_scope(TestContext& t) {
            t.out << "[app_dispatch] plan-report missing-scope diagnostics\n";
            RunFixturePaths fixture{};
            std::string fixture_err;
            const bool fixture_ok = create_unified_panel_missing_run_fixture(fixture, fixture_err);
            t.check(fixture_ok, "plan-report missing-scope fixture files are created");
            if (!fixture_ok) return false;

            AppCommand cmd{};
            cmd.kind = CommandKind::PlanReport;
            cmd.plan_report.layout_path = fixture.layout_path;
            cmd.plan_report.logic_path = fixture.logic_path;
            cmd.plan_report.csv_path = fixture.csv_path;

            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_plan_report_command(cmd, out, err);
            const std::string text = out.str();

            t.check(rc == 1, "plan-report returns failure for multi-entity input without run scope");
            t.check(text.find("=== [1/1] Pre-binding observation assessment ===") != std::string::npos,
                "plan-report missing-scope output labels the facts-only observation stage");
            t.check(text.find("Observation world provenance") != std::string::npos,
                "plan-report missing-scope output still includes observation-world provenance");
            t.check(text.find("Plan compatibility") != std::string::npos,
                "plan-report missing-scope output still includes plan compatibility");
            t.check(err.str().find("logic.run.scope") != std::string::npos,
                "plan-report missing-scope failure is explicit");
            t.check(err.str().find("primary_series or per_entity_ranked") != std::string::npos,
                "plan-report missing-scope failure lists valid scope values");
            return t.failed == 0;
        }

    } // namespace

    void register_observation_plan_report_tests(AppTestRegistry& tests) {
        tests.push_back(test_obs_report_dispatch_smoke);
        tests.push_back(test_obs_report_json_success_is_machine_readable);
        tests.push_back(test_obs_report_json_numeric_parse_categories);
        tests.push_back(test_obs_report_json_duplicate_time_key_counts);
        tests.push_back(test_obs_report_accepts_friendly_time_alias_and_single_value_primary_inference);
        tests.push_back(test_obs_report_warns_on_primary_only_value_shorthand);
        tests.push_back(test_plan_report_dispatch_smoke);
        tests.push_back(test_plan_report_surfaces_topology_query_requirements);
        tests.push_back(test_plan_report_rejects_unknown_binding_key);
        tests.push_back(test_plan_report_rejects_time_valid_topology_without_time_axis);
        tests.push_back(test_plan_report_links_sparse_columns_to_nodes);
        tests.push_back(test_obs_report_exposes_drop_row_diagnostics);
        tests.push_back(test_obs_report_warns_on_degenerate_value_columns);
        tests.push_back(test_obs_report_time_parse_failure_includes_time_kind_hint);
        tests.push_back(test_obs_report_surfaces_topology_layout_warnings);
        tests.push_back(test_obs_report_flags_time_valid_edges_without_time_axis);
        tests.push_back(test_plan_report_reports_multi_entity_missing_scope);
    }

} // namespace sugar::app::tests
