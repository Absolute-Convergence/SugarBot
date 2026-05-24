#include "tests/app_test_support.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "commands/app_commands.h"

namespace sugar::app::tests {

    namespace {

        namespace fs = std::filesystem;

        bool test_unified_run_executes_single_series_without_run_section(TestContext& t) {
            t.out << "[app_run] unified run executes single-series without run section\n";
            RunFixturePaths fixture{};
            std::string fixture_err;
            const bool fixture_ok = create_unified_single_series_fixture(fixture, fixture_err);
            t.check(fixture_ok, "unified single-series fixture files are created");
            if (!fixture_ok) return false;

            AppCommand cmd{};
            cmd.kind = CommandKind::Run;
            cmd.run.layout_path = fixture.layout_path;
            cmd.run.logic_path = fixture.logic_path;
            cmd.run.csv_path = fixture.csv_path;

            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_unified_cli_command(cmd, out, err);
            const std::string text = out.str();

            t.check(rc == 0, "unified single-series run returns success");
            t.check(err.str().empty(), "unified single-series run produces no error output");
            t.check(text.find("=== [1/2] Pre-binding observation assessment ===") != std::string::npos,
                "unified single-series run labels the pre-binding observation stage");
            t.check(text.find("=== DAG runtime binding ===") != std::string::npos,
                "unified single-series run labels the runtime binding stage");
            t.check(text.find("=== [2/2] Post-binding runtime assessment ===") != std::string::npos,
                "unified single-series run labels the post-binding runtime stage");
            t.check(text.find("Observation world provenance") != std::string::npos,
                "unified single-series run reports observation-world provenance");
            t.check(text.find("Runtime provenance") != std::string::npos,
                "unified single-series run reports runtime provenance");
            t.check(text.find("primary_selection_mode: single_series_implicit") != std::string::npos,
                "unified single-series run records implicit singleton target provenance");
            t.check(text.find("Plan compatibility") != std::string::npos,
                "unified single-series run reports plan compatibility");
            t.check(text.find("status: valid") != std::string::npos,
                "unified single-series run reports valid plan compatibility");
            return t.failed == 0;
        }

        bool test_unified_run_accepts_precomputed_column_and_plain_aliases(TestContext& t) {
            t.out << "[app_run] unified run accepts precomputed column and plain aliases\n";
            std::string fixture_err;
            const fs::path fixture_dir = app_fixture_dir(fixture_err);
            t.check(!fixture_dir.empty(), "precomputed alias fixture dir is available");
            if (fixture_dir.empty()) return false;

            const fs::path layout_path = fixture_dir / "app_tests_tmp_precomputed_alias_layout.json";
            const fs::path logic_path = fixture_dir / "app_tests_tmp_precomputed_alias_logic.json";
            const fs::path csv_path = fixture_dir / "app_tests_tmp_precomputed_alias.csv";

            {
                std::ofstream layout_out(layout_path, std::ios::binary | std::ios::trunc);
                t.check(static_cast<bool>(layout_out), "precomputed alias layout file opens");
                if (!layout_out) return false;
                layout_out <<
                    "{\n"
                    "  \"ingest\": {\n"
                    "    \"time_col\": \"date\",\n"
                    "    \"time_kind\": \"YYYY-MM-DD\",\n"
                    "    \"header_policy\": \"RequiredHeaders\",\n"
                    "    \"value_cols\": [\"price\", \"MA\"],\n"
                    "    \"primary_value_col\": \"price\"\n"
                    "  }\n"
                    "}\n";
            }

            {
                std::ofstream logic_out(logic_path, std::ios::binary | std::ios::trunc);
                t.check(static_cast<bool>(logic_out), "precomputed alias logic file opens");
                if (!logic_out) return false;
                logic_out <<
                    "{\n"
                    "  \"version\": 1,\n"
                    "  \"trade_mode\": \"LongOnly\",\n"
                    "  \"numeric_nodes\": [\n"
                    "    { \"id\": \"p\", \"op\": \"core.close\" },\n"
                    "    { \"id\": \"ma\", \"op\": \"core.precomputed_num\", \"params\": { \"name\": \"MA\" } }\n"
                    "  ],\n"
                    "  \"bool_nodes\": [\n"
                    "    { \"id\": \"above\", \"op\": \"cmp.gte\", \"args\": [\"ma\", \"p\"] },\n"
                    "    { \"id\": \"entry\", \"op\": \"bool.and\", \"args\": [\"above\", \"above\"] }\n"
                    "  ],\n"
                    "  \"bindings\": { \"entry_long\": \"entry\", \"exit_long\": \"\", \"entry_short\": \"\", \"exit_short\": \"\", \"allow_trade\": \"\" }\n"
                    "}\n";
            }

            {
                std::ofstream csv_out(csv_path, std::ios::binary | std::ios::trunc);
                t.check(static_cast<bool>(csv_out), "precomputed alias csv file opens");
                if (!csv_out) return false;
                csv_out <<
                    "date,price,MA\n"
                    "2024-01-01,10,10\n"
                    "2024-01-02,11,12\n"
                    "2024-01-03,12,13\n";
            }

            AppCommand cmd{};
            cmd.kind = CommandKind::Run;
            cmd.run.layout_path = layout_path.string();
            cmd.run.logic_path = logic_path.string();
            cmd.run.csv_path = csv_path.string();

            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_unified_cli_command(cmd, out, err);
            const std::string text = out.str();

            t.check(rc == 0, "unified run succeeds with direct precomputed CSV column and alias ops");
            t.check(err.str().empty(), "precomputed alias run produces no error output");
            t.check(text.find("PlanSpec compile failed") == std::string::npos &&
                    text.find("precomputed numeric leaf 'MA' not found") == std::string::npos,
                "direct precomputed CSV column is registered in the compile environment");
            t.check(text.find("status: valid") != std::string::npos,
                "precomputed alias run keeps plan compatibility valid");
            return t.failed == 0;
        }

        bool test_unified_run_explains_zero_trades_from_sparse_column(TestContext& t) {
            t.out << "[app_run] unified run explains zero trades from sparse column\n";
            std::string fixture_err;
            const fs::path fixture_dir = app_fixture_dir(fixture_err);
            t.check(!fixture_dir.empty(), "sparse-column run fixture dir is available");
            if (fixture_dir.empty()) return false;

            const fs::path layout_path = fixture_dir / "app_tests_tmp_sparse_run_layout.json";
            const fs::path logic_path = fixture_dir / "app_tests_tmp_sparse_run_logic.json";
            const fs::path csv_path = fixture_dir / "app_tests_tmp_sparse_run.csv";

            {
                std::ofstream layout_out(layout_path, std::ios::binary | std::ios::trunc);
                t.check(static_cast<bool>(layout_out), "sparse run layout file opens");
                if (!layout_out) return false;
                layout_out <<
                    "{\n"
                    "  \"ingest\": {\n"
                    "    \"time_col\": \"date\",\n"
                    "    \"time_kind\": \"YYYY-MM-DD\",\n"
                    "    \"value_cols\": [\"price\", \"MA\"],\n"
                    "    \"primary_value_col\": \"price\",\n"
                    "    \"missing_tokens\": [\"\", \"NA\", \"null\", \"nan\", \"NaN\"],\n"
                    "    \"missing_default\": { \"kind\": \"NaN\" }\n"
                    "  }\n"
                    "}\n";
            }

            {
                std::ofstream logic_out(logic_path, std::ios::binary | std::ios::trunc);
                t.check(static_cast<bool>(logic_out), "sparse run logic file opens");
                if (!logic_out) return false;
                logic_out <<
                    "{\n"
                    "  \"version\": 1,\n"
                    "  \"trade_mode\": \"LongOnly\",\n"
                    "  \"numeric_nodes\": [\n"
                    "    { \"id\": \"ma\", \"op\": \"core.ref\", \"params\": { \"ref\": \"MA\" } },\n"
                    "    { \"id\": \"zero\", \"op\": \"core.const\", \"params\": { \"value\": 0.0 } }\n"
                    "  ],\n"
                    "  \"bool_nodes\": [\n"
                    "    { \"id\": \"entry\", \"op\": \"cmp.gt\", \"args\": [\"ma\", \"zero\"] }\n"
                    "  ],\n"
                    "  \"bindings\": { \"entry_long\": \"entry\", \"exit_long\": \"\", \"entry_short\": \"\", \"exit_short\": \"\", \"allow_trade\": \"\" }\n"
                    "}\n";
            }

            {
                std::ofstream csv_out(csv_path, std::ios::binary | std::ios::trunc);
                t.check(static_cast<bool>(csv_out), "sparse run csv file opens");
                if (!csv_out) return false;
                csv_out <<
                    "date,price,MA\n"
                    "2024-01-01,10,-1\n"
                    "2024-01-02,11,NA\n"
                    "2024-01-03,12,\n"
                    "2024-01-04,13,NaN\n";
            }

            AppCommand cmd{};
            cmd.kind = CommandKind::Run;
            cmd.run.layout_path = layout_path.string();
            cmd.run.logic_path = logic_path.string();
            cmd.run.csv_path = csv_path.string();

            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_unified_cli_command(cmd, out, err);
            const std::string text = out.str();

            t.check(rc == 0, "sparse-column zero-trade run remains a valid execution");
            t.check(err.str().empty(), "sparse-column zero-trade run keeps stderr quiet");
            t.check(text.find("value column 'MA' has 75% missing values after preprocessing (3/4 rows)") != std::string::npos,
                "run output explains sparse source data before the zero-trade result");
            t.check(text.find("Plan data-quality notes") != std::string::npos &&
                    text.find("numeric node ma (core.ref) reads value column 'MA'") != std::string::npos,
                "run output links sparse source data to the referencing PlanSpec node");
            t.check(text.find("Trades: 0") != std::string::npos &&
                    text.find("entry_long=0") != std::string::npos,
                "sparse-column run reaches the zero-trade result with visible signal counts");
            return t.failed == 0;
        }

        bool test_unified_run_executes_primary_series_multi_entity_scope(TestContext& t) {
            t.out << "[app_run] unified run executes primary-series multi-entity scope\n";
            RunFixturePaths fixture{};
            std::string fixture_err;
            const bool fixture_ok = create_unified_primary_series_fixture(fixture, fixture_err);
            t.check(fixture_ok, "unified primary-series fixture files are created");
            if (!fixture_ok) return false;

            AppCommand cmd{};
            cmd.kind = CommandKind::Run;
            cmd.run.layout_path = fixture.layout_path;
            cmd.run.logic_path = fixture.logic_path;
            cmd.run.csv_path = fixture.csv_path;

            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_unified_cli_command(cmd, out, err);
            const std::string text = out.str();

            t.check(rc == 0, "unified primary-series run returns success");
            t.check(err.str().empty(), "unified primary-series run produces no error output");
            t.check(text.find("=== [1/2] Pre-binding observation assessment ===") != std::string::npos,
                "unified primary-series run labels the pre-binding observation stage");
            t.check(text.find("=== DAG runtime binding ===") != std::string::npos,
                "unified primary-series run labels the runtime binding stage");
            t.check(text.find("=== [2/2] Post-binding runtime assessment ===") != std::string::npos,
                "unified primary-series run labels the post-binding runtime stage");
            t.check(text.find("primary_selection_mode: explicit_series") != std::string::npos,
                "unified primary-series run records explicit target provenance");
            t.check(text.find("[Info] run scope: primary_series target_entity=BBB") != std::string::npos,
                "unified primary-series run reports explicit target entity");
            t.check(text.find("Plan compatibility") != std::string::npos,
                "unified primary-series run reports plan compatibility");
            return t.failed == 0;
        }

        bool test_unified_run_executes_ranked_multi_entity_scope(TestContext& t) {
            t.out << "[app_run] unified run executes ranked multi-entity scope\n";
            RunFixturePaths fixture{};
            std::string fixture_err;
            const bool fixture_ok = create_unified_panel_ranked_fixture(fixture, fixture_err);
            t.check(fixture_ok, "unified ranked fixture files are created");
            if (!fixture_ok) return false;

            AppCommand cmd{};
            cmd.kind = CommandKind::Run;
            cmd.run.layout_path = fixture.layout_path;
            cmd.run.logic_path = fixture.logic_path;
            cmd.run.csv_path = fixture.csv_path;

            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_unified_cli_command(cmd, out, err);
            const std::string text = out.str();

            t.check(rc == 0, "unified ranked run returns success");
            t.check(err.str().empty(), "unified ranked run produces no error output");
            t.check(text.find("=== [1/1] Pre-binding observation assessment ===") != std::string::npos,
                "unified ranked run labels the facts-only observation stage");
            t.check(text.find("=== [2/2] Post-binding runtime assessment ===") == std::string::npos,
                "unified ranked run avoids a post-binding stage because no single runtime target is bound");
            t.check(text.find("Observation world provenance") != std::string::npos,
                "unified ranked run reports observation-world provenance");
            t.check(text.find("semantic.regime.test") != std::string::npos,
                "unified ranked run reports declared layout topology metadata");
            t.check(text.find("[Info] run scope: per_entity_ranked top_k=2 ranking_objective=maximize reported_metric:pnl") != std::string::npos,
                "unified ranked run reports ranked scope derived from logic.json");
            t.check(text.find("Panel result") != std::string::npos,
                "unified ranked run prints ranked panel summary");
            t.check(text.find("ranking_objective=maximize reported_metric:pnl") != std::string::npos,
                "unified ranked run reports canonical ranking objective output");
            return t.failed == 0;
        }

        bool test_unified_run_rejects_multi_entity_without_run_scope(TestContext& t) {
            t.out << "[app_run] unified run rejects multi-entity input without run scope\n";
            RunFixturePaths fixture{};
            std::string fixture_err;
            const bool fixture_ok = create_unified_panel_missing_run_fixture(fixture, fixture_err);
            t.check(fixture_ok, "unified missing-scope fixture files are created");
            if (!fixture_ok) return false;

            AppCommand cmd{};
            cmd.kind = CommandKind::Run;
            cmd.run.layout_path = fixture.layout_path;
            cmd.run.logic_path = fixture.logic_path;
            cmd.run.csv_path = fixture.csv_path;

            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_unified_cli_command(cmd, out, err);

            t.check(rc == 1, "unified multi-entity run without scope returns failure");
            t.check(err.str().find("multi-entity runs require logic.run.scope") != std::string::npos,
                "unified multi-entity run without scope fails explicitly");
            t.check(err.str().find("primary_series or per_entity_ranked") != std::string::npos,
                "unified multi-entity run without scope lists valid scope values");
            t.check(err.str().find("sugarbot --plan-report") != std::string::npos &&
                    err.str().find("sugarbot --align-dump") != std::string::npos,
                "unified multi-entity run without scope points to plan-report and align-dump");
            return t.failed == 0;
        }

        bool test_unified_run_rejects_legacy_embedded_logic(TestContext& t) {
            t.out << "[app_run] unified run rejects legacy embedded observation logic\n";
            RunFixturePaths fixture{};
            std::string fixture_err;
            const bool fixture_ok = create_unified_legacy_logic_fixture(fixture, fixture_err);
            t.check(fixture_ok, "unified legacy-logic fixture files are created");
            if (!fixture_ok) return false;

            AppCommand cmd{};
            cmd.kind = CommandKind::Run;
            cmd.run.layout_path = fixture.layout_path;
            cmd.run.logic_path = fixture.logic_path;
            cmd.run.csv_path = fixture.csv_path;

            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_unified_cli_command(cmd, out, err);

            t.check(rc == 1, "unified legacy-logic run returns failure");
            t.check(err.str().find("interactive-only") != std::string::npos,
                "unified legacy-logic run quarantines embedded observations to interactive mode");
            t.check(err.str().find("sugarbot --plan-report") != std::string::npos,
                "unified legacy-logic run points to plan-report for artifact correction");
            return t.failed == 0;
        }

        bool test_unified_run_ingest_failure_suggests_obs_report(TestContext& t) {
            t.out << "[app_run] unified run ingest failure suggests obs-report\n";
            RunFixturePaths fixture{};
            std::string fixture_err;
            const bool fixture_ok = create_unified_single_series_fixture(fixture, fixture_err);
            t.check(fixture_ok, "unified single-series fixture files are created");
            if (!fixture_ok) return false;

            AppCommand cmd{};
            cmd.kind = CommandKind::Run;
            cmd.run.layout_path = fixture.layout_path;
            cmd.run.logic_path = fixture.logic_path;
            cmd.run.csv_path = (fs::path(fixture.csv_path).parent_path() / "missing_run_input.csv").string();

            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_unified_cli_command(cmd, out, err);
            const std::string error_text = err.str();

            t.check(rc == 1, "unified run with missing CSV returns failure");
            t.check(error_text.find("sugarbot --obs-report") != std::string::npos,
                "missing CSV run points to obs-report");
            t.check(error_text.find(cmd.run.layout_path) != std::string::npos &&
                    error_text.find(cmd.run.csv_path) != std::string::npos,
                "obs-report next step includes the same layout and CSV paths");
            return t.failed == 0;
        }

        bool test_unified_run_logic_failure_suggests_plan_report(TestContext& t) {
            t.out << "[app_run] unified run logic failure suggests plan-report\n";
            RunFixturePaths fixture{};
            std::string fixture_err;
            const bool fixture_ok = create_unified_single_series_fixture(fixture, fixture_err);
            t.check(fixture_ok, "unified single-series fixture files are created");
            if (!fixture_ok) return false;

            AppCommand cmd{};
            cmd.kind = CommandKind::Run;
            cmd.run.layout_path = fixture.layout_path;
            cmd.run.logic_path = (fs::path(fixture.logic_path).parent_path() / "missing_logic.json").string();
            cmd.run.csv_path = fixture.csv_path;

            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_unified_cli_command(cmd, out, err);
            const std::string error_text = err.str();

            t.check(rc == 1, "unified run with missing logic returns failure");
            t.check(error_text.find("sugarbot --plan-report") != std::string::npos,
                "missing logic run points to plan-report");
            t.check(error_text.find(cmd.run.layout_path) != std::string::npos &&
                    error_text.find(cmd.run.logic_path) != std::string::npos &&
                    error_text.find(cmd.run.csv_path) != std::string::npos,
                "plan-report next step includes the same layout, logic, and CSV paths");
            return t.failed == 0;
        }

        bool test_unified_run_topology_report_projection(TestContext& t) {
            t.out << "[app_run] unified run topology report projection\n";
            std::string fixture_err;
            const fs::path fixture_dir = app_fixture_dir(fixture_err);
            t.check(!fixture_dir.empty(), "topology report fixture dir is available");
            if (fixture_dir.empty()) return false;

            const fs::path layout_path =
                fixture_dir / "app_tests_tmp_topology_report_layout.json";
            const fs::path logic_path =
                fixture_dir / "app_tests_tmp_topology_report_logic.json";
            const fs::path csv_path =
                fixture_dir / "app_tests_tmp_topology_report.csv";

            {
                std::ofstream layout_out(layout_path, std::ios::binary | std::ios::trunc);
                t.check(static_cast<bool>(layout_out), "topology report layout file opens");
                if (!layout_out) return false;
                layout_out <<
                    "{\n"
                    "  \"ingest\": {\n"
                    "    \"header_policy\": \"RequiredHeaders\",\n"
                    "    \"time_col\": \"date\",\n"
                    "    \"time_kind\": \"YyyyMmDd\",\n"
                    "    \"value_cols\": [\"close\"],\n"
                    "    \"primary_value_col\": \"close\",\n"
                    "    \"sort_by_time\": true,\n"
                    "    \"dedupe\": \"Error\"\n"
                    "  },\n"
                    "  \"topologies\": [\n"
                    "    {\n"
                    "      \"name\": \"row.time.graph\",\n"
                    "      \"bearing\": \"query_bearing\",\n"
                    "      \"family\": \"relational_graph\",\n"
                    "      \"domain_scope\": \"row_domain\",\n"
                    "      \"query_kinds\": [\"neighbor_set\", \"weighted_neighbor_set\"],\n"
                    "      \"directed\": true,\n"
                    "      \"weighted\": true,\n"
                    "      \"node_count\": 4,\n"
                    "      \"artifact_ref\": {\n"
                    "        \"stable_id\": \"row.time.graph.v1\",\n"
                    "        \"locator\": \"inline://layout/topologies/row.time.graph\",\n"
                    "        \"format\": \"layout_static_edges\",\n"
                    "        \"producer\": \"app_test\"\n"
                    "      },\n"
                    "      \"edges\": [\n"
                    "        {\"source\": 0, \"target\": 1, \"weight\": 1.0, \"valid_from\": 20240101, \"valid_to\": 20240103},\n"
                    "        {\"source\": 1, \"target\": 3, \"weight\": 1.0},\n"
                    "        {\"source\": 1, \"target\": 2, \"weight\": 2.0, \"version\": \"alt\"},\n"
                    "        {\"source\": 2, \"target\": 3, \"weight\": 1.0, \"valid_from\": 20240103},\n"
                    "        {\"source\": 3, \"target\": 0, \"weight\": 1.0, \"valid_to\": 20240103}\n"
                    "      ]\n"
                    "    }\n"
                    "  ]\n"
                    "}\n";
            }

            {
                std::ofstream csv_out(csv_path, std::ios::binary | std::ios::trunc);
                t.check(static_cast<bool>(csv_out), "topology report csv file opens");
                if (!csv_out) return false;
                csv_out <<
                    "date,close\n"
                    "20240101,10\n"
                    "20240102,20\n"
                    "20240103,30\n"
                    "20240104,40\n";
            }

            {
                std::ofstream logic_out(logic_path, std::ios::binary | std::ios::trunc);
                t.check(static_cast<bool>(logic_out), "topology report logic file opens");
                if (!logic_out) return false;
                logic_out <<
                    "{\n"
                    "  \"version\": 1,\n"
                    "  \"name\": \"app_tests_topology_report\",\n"
                    "  \"trade_mode\": \"LongOnly\",\n"
                    "  \"numeric_nodes\": [\n"
                    "    { \"id\": \"c\", \"op\": \"core.close\" },\n"
                    "    { \"id\": \"z\", \"op\": \"core.const\", \"params\": { \"value\": 0.0 } },\n"
                    "    { \"id\": \"mean_default\", \"op\": \"topology.neighbor_mean\", \"args\": [\"c\"], \"params\": { \"topology\": \"row.time.graph\" } },\n"
                    "    { \"id\": \"mean_alt\", \"op\": \"topology.neighbor_mean\", \"args\": [\"c\"], \"params\": { \"topology\": \"row.time.graph\", \"version\": \"alt\" } },\n"
                    "    { \"id\": \"weighted_alt\", \"op\": \"topology.weighted_sum\", \"args\": [\"c\"], \"params\": { \"topology\": \"row.time.graph\", \"version\": \"alt\" } }\n"
                    "  ],\n"
                    "  \"bool_nodes\": [\n"
                    "    { \"id\": \"entry\", \"op\": \"cmp.gt\", \"args\": [\"c\", \"z\"] },\n"
                    "    { \"id\": \"exit\", \"op\": \"cmp.lt\", \"args\": [\"c\", \"z\"] }\n"
                    "  ],\n"
                    "  \"bindings\": {\n"
                    "    \"entry_long\": \"entry\",\n"
                    "    \"exit_long\": \"exit\",\n"
                    "    \"entry_short\": \"\",\n"
                    "    \"exit_short\": \"\",\n"
                    "    \"allow_trade\": \"\"\n"
                    "  }\n"
                    "}\n";
            }

            AppCommand cmd{};
            cmd.kind = CommandKind::Run;
            cmd.run.layout_path = layout_path.string();
            cmd.run.logic_path = logic_path.string();
            cmd.run.csv_path = csv_path.string();
            cmd.run.report_projection =
                sugar::EvaluationReportProjection::TopologyDiagnostics;

            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_unified_cli_command(cmd, out, err);
            const std::string text = out.str();

            t.check(rc == 0, "topology projection run returns success");
            t.check(err.str().empty(), "topology projection run produces no error output");
            t.check(text.find("Topology diagnostics:") != std::string::npos,
                "topology projection prints the focused diagnostics section");
            t.check(text.find("Topology diagnostics:") == 0 &&
                    text.find("Observation world provenance") == std::string::npos &&
                    text.find("Plan compatibility") == std::string::npos,
                "topology projection leads with topology diagnostics and suppresses validation boilerplate");
            t.check(text.find("declared_topologies:") != std::string::npos &&
                    text.find("topology_usages: 3") != std::string::npos,
                "topology projection summarizes declared topologies and operator usage count");
            t.check(text.find("- row.time.graph") != std::string::npos &&
                    text.find("family: relational_graph") != std::string::npos &&
                    text.find("bearing: query_bearing") != std::string::npos,
                "topology projection identifies the declared graph topology");
            t.check(text.find("edge_count: 5") != std::string::npos &&
                    text.find("time_valid_edges: 3") != std::string::npos &&
                    text.find("versioned_edges: 1") != std::string::npos,
                "topology projection prints time-valid and versioned edge counts");
            t.check(text.find("node=mean_alt op=topology.neighbor_mean topology=row.time.graph") != std::string::npos &&
                    text.find("node=weighted_alt op=topology.weighted_sum topology=row.time.graph") != std::string::npos,
                "topology projection lists topology-local operator usages");
            t.check(text.find("required_query_kinds: neighbor_set") != std::string::npos &&
                    text.find("required_query_kinds: weighted_neighbor_set") != std::string::npos,
                "topology projection prints required query kinds for operators");
            t.check(text.find("runtime_resolved: true") != std::string::npos &&
                    text.find("stable_id=row.time.graph.v1") != std::string::npos,
                "topology projection reports runtime resolution and artifact identity");
            t.check(text.find("PnL:") == std::string::npos &&
                    text.find("Trade hash:") == std::string::npos,
                "topology projection avoids the full run result body");

            cmd.run.report_projection = sugar::EvaluationReportProjection::Json;
            std::ostringstream json_out;
            std::ostringstream json_err;
            const int json_rc = run_unified_cli_command(cmd, json_out, json_err);
            const std::string json = json_out.str();

            t.check(json_rc == 0, "topology fixture json report returns success");
            t.check(json_err.str().empty(), "topology fixture json report keeps stderr quiet");
            t.check(json.find("\"topologies\":[") != std::string::npos &&
                    json.find("\"name\":\"row.time.graph\"") != std::string::npos &&
                    json.find("\"family\":\"relational_graph\"") != std::string::npos,
                "topology json report serializes declared topology identity");
            t.check(json.find("\"edge_count\":5") != std::string::npos &&
                    json.find("\"time_valid_edge_count\":3") != std::string::npos &&
                    json.find("\"versioned_edge_count\":1") != std::string::npos,
                "topology json report serializes time-valid and versioned edge counts");
            t.check(json.find("\"topology_usages\":[") != std::string::npos &&
                    json.find("\"subject_id\":\"mean_alt\"") != std::string::npos &&
                    json.find("\"op_name\":\"topology.weighted_sum\"") != std::string::npos,
                "topology json report serializes topology-local operator usages");
            t.check(json.find("\"required_topology_query_kinds\":[\"weighted_neighbor_set\"]") != std::string::npos &&
                    json.find("\"runtime_resolved\":true") != std::string::npos,
                "topology json report serializes query requirements and runtime resolution");
            t.check(json.find("\"stable_id\":\"row.time.graph.v1\"") != std::string::npos,
                "topology json report preserves artifact identity");
            return t.failed == 0;
        }

        struct F8TopologyReadinessDemo {
            const char* name;
            const char* layout;
            const char* logic;
            const char* csv;
            std::vector<std::string> topology_report_needles;
            std::vector<std::string> json_report_needles;
        };

        std::string find_line_containing(
            std::string_view text,
            std::string_view needle) {
            const std::size_t pos = text.find(needle);
            if (pos == std::string_view::npos) {
                return {};
            }
            const std::size_t line_start = text.rfind('\n', pos);
            const std::size_t start =
                (line_start == std::string_view::npos) ? 0 : line_start + 1;
            const std::size_t line_end = text.find('\n', pos);
            const std::size_t end =
                (line_end == std::string_view::npos) ? text.size() : line_end;
            return std::string(text.substr(start, end - start));
        }

        int run_checked_in_demo(
            const fs::path& layout_path,
            const fs::path& logic_path,
            const fs::path& csv_path,
            sugar::EvaluationReportProjection projection,
            std::string& stdout_text,
            std::string& stderr_text) {
            AppCommand cmd{};
            cmd.kind = CommandKind::Run;
            cmd.run.layout_path = layout_path.string();
            cmd.run.logic_path = logic_path.string();
            cmd.run.csv_path = csv_path.string();
            cmd.run.report_projection = projection;

            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_unified_cli_command(cmd, out, err);
            stdout_text = out.str();
            stderr_text = err.str();
            return rc;
        }

        bool test_checked_in_quickstart_ohlcv_workflow(TestContext& t) {
            t.out << "[app_run] checked-in quickstart OHLCV workflow\n";
            const fs::path repo_root = find_repo_root();
            t.check(!repo_root.empty(), "repo root is found for quickstart workflow");
            if (repo_root.empty()) return false;

            const fs::path layout_path = repo_root / "graphs" / "demos" / "quickstart_ohlcv.layout.json";
            const fs::path logic_path = repo_root / "graphs" / "demos" / "quickstart_ohlcv.planspec.json";
            const fs::path csv_path = repo_root / "data" / "sample.csv";
            t.check(fs::exists(layout_path), "quickstart layout exists under graphs/demos");
            t.check(fs::exists(logic_path), "quickstart PlanSpec exists under graphs/demos");
            t.check(fs::exists(csv_path), "quickstart CSV exists under data");
            if (!fs::exists(layout_path) || !fs::exists(logic_path) || !fs::exists(csv_path)) {
                return false;
            }

            AppCommand cmd{};
            cmd.kind = CommandKind::Run;
            cmd.run.layout_path = layout_path.string();
            cmd.run.logic_path = logic_path.string();
            cmd.run.csv_path = csv_path.string();

            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_unified_cli_command(cmd, out, err);
            const std::string text = out.str();

            t.check(rc == 0, "quickstart workflow returns success");
            t.check(err.str().empty(), "quickstart workflow keeps stderr quiet");
            t.check(text.find("[Info] noninteractive artifact run active.") != std::string::npos,
                "quickstart workflow uses the noninteractive artifact runtime");
            t.check(text.find("Plan compatibility") != std::string::npos &&
                    text.find("status: valid") != std::string::npos,
                "quickstart workflow validates the PlanSpec against the observation world");
            t.check(text.find("PnL:") != std::string::npos &&
                    text.find("Trade hash:") != std::string::npos,
                "quickstart workflow reaches the final human-readable run report");
            t.check(text.find("legacy embedded") == std::string::npos &&
                    text.find("\"series_inputs\"") == std::string::npos,
                "quickstart workflow avoids legacy embedded observation dependence");
            return t.failed == 0;
        }

        bool test_checked_in_g10_dynamic_execution_demos(TestContext& t) {
            t.out << "[app_run] checked-in G10 dynamic execution demos\n";
            const fs::path repo_root = find_repo_root();
            t.check(!repo_root.empty(), "repo root is found for G10 demo scan");
            if (repo_root.empty()) return false;

            const fs::path quickstart_layout =
                repo_root / "graphs" / "demos" / "quickstart_ohlcv.layout.json";
            const fs::path sample_csv = repo_root / "data" / "sample.csv";
            const fs::path demo_dir =
                repo_root / "graphs" / "demos" / "dynamic_execution";
            const fs::path static_logic =
                demo_dir / "g10_constant_static_equivalent.planspec.json";
            const fs::path constant_dynamic_logic =
                demo_dir / "g10_constant_declared_dynamic.planspec.json";
            const fs::path row_dynamic_logic =
                demo_dir / "g10_row_varying_declared_dynamic.planspec.json";
            const fs::path topology_layout =
                repo_root / "graphs" / "demos" / "topology_readiness" /
                "f8_relational_time_graph.layout.json";
            const fs::path topology_logic =
                demo_dir / "g10_topology_declared_dynamic.planspec.json";
            const fs::path topology_csv =
                repo_root / "data" / "topology_readiness" /
                "f8_relational_time_graph.csv";

            const std::vector<fs::path> required_paths = {
                quickstart_layout,
                sample_csv,
                static_logic,
                constant_dynamic_logic,
                row_dynamic_logic,
                topology_layout,
                topology_logic,
                topology_csv,
            };
            for (const auto& path : required_paths) {
                t.check(fs::exists(path),
                    "G10 demo artifact exists: " + path.filename().string());
            }
            for (const auto& path : required_paths) {
                if (!fs::exists(path)) return false;
            }

            std::string static_out;
            std::string static_err;
            const int static_rc = run_checked_in_demo(
                quickstart_layout,
                static_logic,
                sample_csv,
                sugar::EvaluationReportProjection::Full,
                static_out,
                static_err);
            t.check(static_rc == 0, "G10 static equivalence demo returns success");
            t.check(static_err.empty(), "G10 static equivalence demo keeps stderr quiet");
            t.check(static_out.find("Dynamic execution:") == std::string::npos,
                "G10 static equivalence demo remains a static PlanSpec");

            std::string constant_out;
            std::string constant_err;
            const int constant_rc = run_checked_in_demo(
                quickstart_layout,
                constant_dynamic_logic,
                sample_csv,
                sugar::EvaluationReportProjection::Full,
                constant_out,
                constant_err);
            t.check(constant_rc == 0, "G10 constant-route dynamic demo returns success");
            t.check(constant_err.empty(), "G10 constant-route dynamic demo keeps stderr quiet");
            t.check(constant_out.find("Dynamic execution:") != std::string::npos &&
                    constant_out.find("mode: declared_dynamic") != std::string::npos &&
                    constant_out.find("route constant_gate condition=route_true") != std::string::npos,
                "G10 constant-route dynamic demo reports declared dynamic routing");
            t.check(find_line_containing(static_out, "PnL:") ==
                    find_line_containing(constant_out, "PnL:"),
                "G10 constant-route dynamic demo matches static PnL");
            t.check(!find_line_containing(static_out, "Trade hash:").empty() &&
                    find_line_containing(static_out, "Trade hash:") ==
                    find_line_containing(constant_out, "Trade hash:"),
                "G10 constant-route dynamic demo matches static trade hash");

            std::string row_out;
            std::string row_err;
            const int row_rc = run_checked_in_demo(
                quickstart_layout,
                row_dynamic_logic,
                sample_csv,
                sugar::EvaluationReportProjection::Full,
                row_out,
                row_err);
            t.check(row_rc == 0, "G10 row-varying dynamic demo returns success");
            t.check(row_err.empty(), "G10 row-varying dynamic demo keeps stderr quiet");
            t.check(row_out.find("Dynamic execution:") != std::string::npos &&
                    row_out.find("mode: declared_dynamic") != std::string::npos,
                "G10 row-varying dynamic demo reports declared dynamic mode");
            t.check(row_out.find("route row_threshold_gate condition=route_high") != std::string::npos &&
                    row_out.find("true=") != std::string::npos &&
                    row_out.find("false=") != std::string::npos &&
                    row_out.find("fallback=") != std::string::npos,
                "G10 row-varying dynamic demo reports true, false, and fallback route decisions");
            t.check(row_out.find("sample row=") != std::string::npos &&
                    row_out.find("case=high_row") != std::string::npos,
                "G10 row-varying dynamic demo emits bounded decision samples");

            std::string topology_out;
            std::string topology_err;
            const int topology_rc = run_checked_in_demo(
                topology_layout,
                topology_logic,
                topology_csv,
                sugar::EvaluationReportProjection::Json,
                topology_out,
                topology_err);
            t.check(topology_rc == 0, "G10 topology dynamic demo returns success");
            t.check(topology_err.empty(), "G10 topology dynamic demo keeps stderr quiet");
            t.check(topology_out.find("{\"schema_version\":\"sugarbot.evaluation_report.v1\"") == 0,
                "G10 topology dynamic demo emits one JSON report object");
            t.check(topology_out.find("\"dynamic_execution\":{\"enabled\":true,\"mode\":\"declared_dynamic\"") != std::string::npos &&
                    topology_out.find("\"id\":\"topology_degree_gate\"") != std::string::npos &&
                    topology_out.find("\"condition_node_id\":\"route_dense\"") != std::string::npos,
                "G10 topology dynamic demo preserves dynamic route diagnostics in JSON");
            t.check(topology_out.find("\"true_decisions\":0") == std::string::npos &&
                    topology_out.find("\"selected_case_id\":\"dense_graph_row\"") != std::string::npos,
                "G10 topology dynamic demo exercises the active topology route path");
            t.check(topology_out.find("\"name\":\"f8.row.market.graph\"") != std::string::npos &&
                    topology_out.find("\"op_name\":\"topology.degree\"") != std::string::npos &&
                    topology_out.find("\"runtime_resolved\":true") != std::string::npos,
                "G10 topology dynamic demo preserves topology provenance in JSON");
            return t.failed == 0;
        }

        bool test_checked_in_f8_topology_readiness_demos(TestContext& t) {
            t.out << "[app_run] checked-in F8 topology readiness demos\n";
            const fs::path repo_root = find_repo_root();
            t.check(!repo_root.empty(), "repo root is found for F8 demo scan");
            if (repo_root.empty()) return false;

            const fs::path demo_dir = repo_root / "graphs" / "demos" / "topology_readiness";
            const std::vector<F8TopologyReadinessDemo> demos = {
                F8TopologyReadinessDemo{
                    "f8_relational_time_graph",
                    "f8_relational_time_graph.layout.json",
                    "f8_relational_time_graph.planspec.json",
                    "f8_relational_time_graph.csv",
                    {
                        "Topology diagnostics:",
                        "declared_topologies:",
                        "declared_topologies_reported:",
                        "topology_usages: 5",
                        "- f8.row.market.graph",
                        "family: relational_graph",
                        "edge_count: 7",
                        "time_valid_edges: 3",
                        "versioned_edges: 2",
                        "node=stress_weighted op=topology.weighted_sum topology=f8.row.market.graph",
                        "required_query_kinds: weighted_neighbor_set",
                        "stable_id=f8.row.market.graph.v1",
                    },
                    {
                        "\"topologies_reported_count\":",
                        "\"topology_usages_reported_count\":5",
                        "\"name\":\"f8.row.market.graph\"",
                        "\"family\":\"relational_graph\"",
                        "\"edge_count\":7",
                        "\"time_valid_edge_count\":3",
                        "\"versioned_edge_count\":2",
                        "\"subject_id\":\"stress_weighted\"",
                        "\"op_name\":\"topology.weighted_sum\"",
                        "\"required_topology_query_kinds\":[\"weighted_neighbor_set\"]",
                        "\"stable_id\":\"f8.row.market.graph.v1\"",
                    },
                },
                F8TopologyReadinessDemo{
                    "f8_hierarchy_semantic",
                    "f8_hierarchy_semantic.layout.json",
                    "f8_hierarchy_semantic.planspec.json",
                    "f8_hierarchy_semantic.csv",
                    {
                        "Topology diagnostics:",
                        "declared_topologies:",
                        "declared_topologies_reported:",
                        "topology_usages: 7",
                        "- f8.row.account.hierarchy",
                        "family: hierarchy",
                        "edge_count: 5",
                        "- f8.row.embedding.semantic",
                        "family: semantic_graph",
                        "semantic_embeddings: 6",
                        "node=hier_sibling_mean op=topology.neighbor_mean topology=f8.row.account.hierarchy",
                        "node=semantic_weighted op=topology.weighted_sum topology=f8.row.embedding.semantic",
                        "stable_id=f8.row.account.hierarchy.v1",
                        "stable_id=f8.row.embedding.semantic.v1",
                    },
                    {
                        "\"topologies_reported_count\":",
                        "\"topology_usages_reported_count\":7",
                        "\"name\":\"f8.row.account.hierarchy\"",
                        "\"family\":\"hierarchy\"",
                        "\"edge_count\":5",
                        "\"name\":\"f8.row.embedding.semantic\"",
                        "\"family\":\"semantic_graph\"",
                        "\"semantic_embedding_count\":6",
                        "\"subject_id\":\"semantic_weighted\"",
                        "\"op_name\":\"topology.weighted_sum\"",
                        "\"required_topology_query_kinds\":[\"weighted_neighbor_set\"]",
                        "\"stable_id\":\"f8.row.account.hierarchy.v1\"",
                        "\"stable_id\":\"f8.row.embedding.semantic.v1\"",
                    },
                },
            };

            for (const auto& demo : demos) {
                const fs::path layout_path = demo_dir / demo.layout;
                const fs::path logic_path = demo_dir / demo.logic;
                const fs::path csv_path = repo_root / "data" / "topology_readiness" / demo.csv;
                t.check(fs::exists(layout_path), std::string(demo.name) + " layout exists");
                t.check(fs::exists(logic_path), std::string(demo.name) + " PlanSpec exists");
                t.check(fs::exists(csv_path), std::string(demo.name) + " CSV exists under data/topology_readiness");
                if (!fs::exists(layout_path) || !fs::exists(logic_path) || !fs::exists(csv_path)) {
                    continue;
                }

                AppCommand cmd{};
                cmd.kind = CommandKind::Run;
                cmd.run.layout_path = layout_path.string();
                cmd.run.logic_path = logic_path.string();
                cmd.run.csv_path = csv_path.string();
                cmd.run.report_projection =
                    sugar::EvaluationReportProjection::TopologyDiagnostics;

                std::ostringstream topology_out;
                std::ostringstream topology_err;
                const int topology_rc = run_unified_cli_command(cmd, topology_out, topology_err);
                const std::string topology_text = topology_out.str();
                t.check(topology_rc == 0, std::string(demo.name) + " topology report returns success");
                t.check(topology_err.str().empty(), std::string(demo.name) + " topology report keeps stderr quiet");
                t.check(topology_text.find("PnL:") == std::string::npos &&
                        topology_text.find("Trade hash:") == std::string::npos &&
                        topology_text.find("Observation world provenance") == std::string::npos &&
                        topology_text.find("Plan compatibility") == std::string::npos,
                    std::string(demo.name) + " topology report stays focused");
                for (const auto& needle : demo.topology_report_needles) {
                    t.check(topology_text.find(needle) != std::string::npos,
                        std::string(demo.name) + " topology report includes " + needle);
                }

                cmd.run.report_projection = sugar::EvaluationReportProjection::Json;
                std::ostringstream json_out;
                std::ostringstream json_err;
                const int json_rc = run_unified_cli_command(cmd, json_out, json_err);
                const std::string json_text = json_out.str();
                t.check(json_rc == 0, std::string(demo.name) + " json report returns success");
                t.check(json_err.str().empty(), std::string(demo.name) + " json report keeps stderr quiet");
                t.check(json_text.find("{\"schema_version\":\"sugarbot.evaluation_report.v1\"") == 0,
                    std::string(demo.name) + " json report is a single scriptable object");
                for (const auto& needle : demo.json_report_needles) {
                    t.check(json_text.find(needle) != std::string::npos,
                        std::string(demo.name) + " json report includes " + needle);
                }
            }

            return t.failed == 0;
        }

        bool test_app_custom_surfaces_avoid_spec_only_runtime(TestContext& t) {
            t.out << "[app_sources] custom surfaces avoid spec-only runtime path\n";
            const fs::path repo_root = find_repo_root();
            t.check(!repo_root.empty(), "repo root is found for app source scan");
            if (repo_root.empty()) return false;

            const std::vector<fs::path> files = {
                repo_root / "app" / "interactive" / "app_interactive.cpp",
                repo_root / "app" / "commands" / "app_commands.cpp",
            };

            for (const auto& path : files) {
                std::string body;
                std::string err;
                const bool read_ok = read_text_file(path, body, err);
                t.check(read_ok, "app custom source file is readable: " + path.filename().string());
                if (!read_ok) continue;

                t.check(body.find("run_custom_plan_spec(spec,") == std::string::npos,
                    "app custom source avoids spec-only run_custom_plan_spec overload: " + path.filename().string());
                t.check(body.find("inputs are ignored") == std::string::npos,
                    "app custom source avoids ignored-input messaging: " + path.filename().string());
                t.check(body.find("build_embedded_plan_observation_compat_runtime(") == std::string::npos,
                    "app custom source avoids direct embedded-runtime shaping: " + path.filename().string());
                t.check(body.find("plan_spec_has_embedded_observation_inputs(") == std::string::npos,
                    "app custom source avoids direct embedded-input inspection: " + path.filename().string());
                t.check(body.find("load_custom_planspec_runtime(") == std::string::npos,
                    "app custom source avoids direct custom PlanSpec runtime loading: " + path.filename().string());
            }

            return t.failed == 0;
        }

        bool test_checked_in_graph_specs_avoid_sandbox_paths(TestContext& t) {
            t.out << "[samples] checked-in graph specs avoid sandbox-only paths\n";
            const fs::path repo_root = find_repo_root();
            t.check(!repo_root.empty(), "repo root is found for sample scan");
            if (repo_root.empty()) return false;

            const fs::path graphs_dir = repo_root / "graphs";
            std::error_code ec;
            fs::recursive_directory_iterator it(graphs_dir, ec);
            t.check(!ec, "graph sample scan starts successfully");
            if (ec) return false;

            int json_count = 0;
            for (const auto& entry : it) {
                if (!entry.is_regular_file()) continue;
                if (entry.path().extension() == ".csv") {
                    t.check(false,
                        "graphs directory keeps CSV data out of graph artifacts: " + entry.path().string());
                    continue;
                }
                if (entry.path().extension() != ".json") continue;
                ++json_count;

                std::string body;
                std::string err;
                const bool read_ok = read_text_file(entry.path(), body, err);
                t.check(read_ok, "graph sample file is readable: " + entry.path().filename().string());
                if (!read_ok) continue;

                t.check(body.find("sandbox_build/test_assets") == std::string::npos,
                    "graph sample avoids sandbox_build path leak: " + entry.path().filename().string());
                t.check(body.find("codex_build/test_assets") == std::string::npos,
                    "graph sample avoids legacy codex_build path leak: " + entry.path().filename().string());
                t.check(body.find("C:/Users/") == std::string::npos,
                    "graph sample avoids checked-in absolute Windows path: " + entry.path().filename().string());
                t.check(body.find("\"series_inputs\"") == std::string::npos,
                    "active graph sample avoids legacy embedded observation inputs: " + entry.path().filename().string());
            }

            t.check(json_count > 0, "graph sample scan found JSON files");
            return t.failed == 0;
        }

    } // namespace

    void register_run_tests(AppTestRegistry& tests) {
        tests.push_back(test_unified_run_executes_single_series_without_run_section);
        tests.push_back(test_unified_run_accepts_precomputed_column_and_plain_aliases);
        tests.push_back(test_unified_run_explains_zero_trades_from_sparse_column);
        tests.push_back(test_unified_run_executes_primary_series_multi_entity_scope);
        tests.push_back(test_unified_run_executes_ranked_multi_entity_scope);
        tests.push_back(test_unified_run_rejects_multi_entity_without_run_scope);
        tests.push_back(test_unified_run_rejects_legacy_embedded_logic);
        tests.push_back(test_unified_run_ingest_failure_suggests_obs_report);
        tests.push_back(test_unified_run_logic_failure_suggests_plan_report);
        tests.push_back(test_unified_run_topology_report_projection);
        tests.push_back(test_checked_in_quickstart_ohlcv_workflow);
        tests.push_back(test_checked_in_g10_dynamic_execution_demos);
        tests.push_back(test_checked_in_f8_topology_readiness_demos);
        tests.push_back(test_app_custom_surfaces_avoid_spec_only_runtime);
        tests.push_back(test_checked_in_graph_specs_avoid_sandbox_paths);
    }

} // namespace sugar::app::tests
