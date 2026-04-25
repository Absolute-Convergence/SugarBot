#include "tests/app_test_support.h"

#include <filesystem>
#include <sstream>
#include <string>
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
        tests.push_back(test_unified_run_executes_primary_series_multi_entity_scope);
        tests.push_back(test_unified_run_executes_ranked_multi_entity_scope);
        tests.push_back(test_unified_run_rejects_multi_entity_without_run_scope);
        tests.push_back(test_unified_run_rejects_legacy_embedded_logic);
        tests.push_back(test_app_custom_surfaces_avoid_spec_only_runtime);
        tests.push_back(test_checked_in_graph_specs_avoid_sandbox_paths);
    }

} // namespace sugar::app::tests
