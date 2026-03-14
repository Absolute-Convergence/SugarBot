#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <ios>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "app_cli.h"
#include "app_commands.h"
#include "app_interactive.h"
#include "app_reporters.h"
#include "app_runtime.h"
#include "op_registry.h"
#include "plan_spec.h"
#include "runtime_graph_preview.h"
#include "run_pipeline.h"

namespace sugar::app::tests {

    namespace {

        namespace fs = std::filesystem;

        struct TestContext {
            std::ostream& out;
            int passed{ 0 };
            int failed{ 0 };

            void check(bool cond, std::string_view label) {
                if (cond) {
                    ++passed;
                    out << "  [PASS] " << label << "\n";
                }
                else {
                    ++failed;
                    out << "  [FAIL] " << label << "\n";
                }
            }
        };

        fs::path find_repo_root() {
            fs::path cur = fs::current_path();
            while (!cur.empty()) {
                if (fs::exists(cur / "CMakeLists.txt")) return cur;
                const auto parent = cur.parent_path();
                if (parent == cur) break;
                cur = parent;
            }
            return {};
        }

        struct PanelFixturePaths {
            std::string spec_path;
            std::string csv_path;
        };

        bool create_time_header_daily_fixture(std::string& out_csv_path, std::string& err) {
            const fs::path repo_root = find_repo_root();
            if (repo_root.empty()) {
                err = "failed to locate repo root from current working directory";
                return false;
            }

            const fs::path fixture_dir = repo_root / "out" / "test_assets";
            std::error_code ec;
            fs::create_directories(fixture_dir, ec);
            if (ec) {
                err = "failed to create fixture directory: " + ec.message();
                return false;
            }

            const fs::path csv_path = fixture_dir / "app_tests_tmp_time_header_daily.csv";
            std::ofstream csv_out(csv_path, std::ios::binary | std::ios::trunc);
            if (!csv_out) {
                err = "failed to open daily fixture CSV path for write";
                return false;
            }

            csv_out <<
                "time,open,high,low,close,Volume\n"
                "2024-01-01,100,101,99,100.5,1000\n"
                "2024-01-02,101,103,100,102.0,1100\n";

            out_csv_path = csv_path.string();
            return true;
        }

        bool create_spatial_only_fixture(std::string& out_spec_path, std::string& out_csv_path, std::string& err) {
            const fs::path repo_root = find_repo_root();
            if (repo_root.empty()) {
                err = "failed to locate repo root from current working directory";
                return false;
            }

            const fs::path fixture_dir = repo_root / "out" / "test_assets";
            std::error_code ec;
            fs::create_directories(fixture_dir, ec);
            if (ec) {
                err = "failed to create fixture directory: " + ec.message();
                return false;
            }

            const fs::path spec_path = fixture_dir / "app_tests_tmp_spatial_only.json";
            const fs::path csv_path = fixture_dir / "app_tests_tmp_spatial_only.csv";

            {
                std::ofstream spec_out(spec_path, std::ios::binary | std::ios::trunc);
                if (!spec_out) {
                    err = "failed to open spatial-only fixture DataSpec path for write";
                    return false;
                }
                spec_out <<
                    "{\n"
                    "  \"header_policy\": \"RequiredHeaders\",\n"
                    "  \"spatial_x_col\": \"x\",\n"
                    "  \"spatial_y_col\": \"y\",\n"
                    "  \"value_cols\": [\"close\"],\n"
                    "  \"primary_value_col\": \"close\",\n"
                    "  \"missing_tokens\": [\"\", \"NA\", \"null\", \"nan\", \"NaN\"],\n"
                    "  \"missing_default\": { \"kind\": \"NaN\" },\n"
                    "  \"sort_by_time\": true,\n"
                    "  \"dedupe\": \"Error\"\n"
                    "}\n";
            }

            {
                std::ofstream csv_out(csv_path, std::ios::binary | std::ios::trunc);
                if (!csv_out) {
                    err = "failed to open spatial-only fixture CSV path for write";
                    return false;
                }
                csv_out <<
                    "x,y,close\n"
                    "5,0,10\n"
                    "1,0,20\n"
                    "3,0,30\n";
            }

            out_spec_path = spec_path.string();
            out_csv_path = csv_path.string();
            return true;
        }

        bool create_spatial_panel_fixture(std::string& out_spec_path, std::string& out_csv_path, std::string& err) {
            const fs::path repo_root = find_repo_root();
            if (repo_root.empty()) {
                err = "failed to locate repo root from current working directory";
                return false;
            }

            const fs::path fixture_dir = repo_root / "out" / "test_assets";
            std::error_code ec;
            fs::create_directories(fixture_dir, ec);
            if (ec) {
                err = "failed to create fixture directory: " + ec.message();
                return false;
            }

            const fs::path spec_path = fixture_dir / "app_tests_tmp_spatial_panel.json";
            const fs::path csv_path = fixture_dir / "app_tests_tmp_spatial_panel.csv";

            {
                std::ofstream spec_out(spec_path, std::ios::binary | std::ios::trunc);
                if (!spec_out) {
                    err = "failed to open spatial-panel fixture DataSpec path for write";
                    return false;
                }
                spec_out <<
                    "{\n"
                    "  \"header_policy\": \"RequiredHeaders\",\n"
                    "  \"entity_col\": \"region\",\n"
                    "  \"spatial_x_col\": \"x\",\n"
                    "  \"spatial_y_col\": \"y\",\n"
                    "  \"value_cols\": [\"close\"],\n"
                    "  \"primary_value_col\": \"close\",\n"
                    "  \"missing_tokens\": [\"\", \"NA\", \"null\", \"nan\", \"NaN\"],\n"
                    "  \"missing_default\": { \"kind\": \"NaN\" },\n"
                    "  \"sort_by_time\": true,\n"
                    "  \"dedupe\": \"Error\"\n"
                    "}\n";
            }

            {
                std::ofstream csv_out(csv_path, std::ios::binary | std::ios::trunc);
                if (!csv_out) {
                    err = "failed to open spatial-panel fixture CSV path for write";
                    return false;
                }
                csv_out <<
                    "region,x,y,close\n"
                    "A,0,0,10\n"
                    "A,1,0,11\n"
                    "B,5,0,20\n"
                    "B,6,0,21\n";
            }

            out_spec_path = spec_path.string();
            out_csv_path = csv_path.string();
            return true;
        }

        bool create_spatiotemporal_panel_fixture(std::string& out_spec_path, std::string& out_csv_path, std::string& err) {
            const fs::path repo_root = find_repo_root();
            if (repo_root.empty()) {
                err = "failed to locate repo root from current working directory";
                return false;
            }

            const fs::path fixture_dir = repo_root / "out" / "test_assets";
            std::error_code ec;
            fs::create_directories(fixture_dir, ec);
            if (ec) {
                err = "failed to create fixture directory: " + ec.message();
                return false;
            }

            const fs::path spec_path = fixture_dir / "app_tests_tmp_spatiotemporal_panel.json";
            const fs::path csv_path = fixture_dir / "app_tests_tmp_spatiotemporal_panel.csv";

            {
                std::ofstream spec_out(spec_path, std::ios::binary | std::ios::trunc);
                if (!spec_out) {
                    err = "failed to open spatiotemporal-panel fixture DataSpec path for write";
                    return false;
                }
                spec_out <<
                    "{\n"
                    "  \"header_policy\": \"RequiredHeaders\",\n"
                    "  \"time_col\": \"time\",\n"
                    "  \"time_kind\": \"Iso8601\",\n"
                    "  \"entity_col\": \"region\",\n"
                    "  \"spatial_x_col\": \"x\",\n"
                    "  \"spatial_y_col\": \"y\",\n"
                    "  \"value_cols\": [\"close\"],\n"
                    "  \"primary_value_col\": \"close\",\n"
                    "  \"missing_tokens\": [\"\", \"NA\", \"null\", \"nan\", \"NaN\"],\n"
                    "  \"missing_default\": { \"kind\": \"NaN\" },\n"
                    "  \"sort_by_time\": true,\n"
                    "  \"dedupe\": \"Error\"\n"
                    "}\n";
            }

            {
                std::ofstream csv_out(csv_path, std::ios::binary | std::ios::trunc);
                if (!csv_out) {
                    err = "failed to open spatiotemporal-panel fixture CSV path for write";
                    return false;
                }
                csv_out <<
                    "time,region,x,y,close\n"
                    "2024-01-01T00:00:00Z,A,0.0,0.0,1.0\n"
                    "2024-01-02T00:00:00Z,A,0.1,0.0,100.0\n"
                    "2024-01-03T00:00:00Z,A,10.0,0.0,1.0\n"
                    "2024-01-01T00:00:00Z,B,0.0,0.0,0.0\n"
                    "2024-01-02T00:00:00Z,B,10.0,0.0,0.0\n"
                    "2024-01-03T00:00:00Z,B,10.1,0.0,0.0\n";
            }

            out_spec_path = spec_path.string();
            out_csv_path = csv_path.string();
            return true;
        }

        bool read_text_file(const fs::path& path, std::string& out, std::string& err) {
            std::ifstream in(path, std::ios::binary);
            if (!in) {
                err = "failed to open file: " + path.string();
                return false;
            }

            std::ostringstream oss;
            oss << in.rdbuf();
            out = oss.str();
            return true;
        }

        bool create_panel_fixture(PanelFixturePaths& out, std::string& err) {
            const fs::path repo_root = find_repo_root();
            if (repo_root.empty()) {
                err = "failed to locate repo root from current working directory";
                return false;
            }

            const fs::path fixture_dir = repo_root / "out" / "test_assets";
            std::error_code ec;
            fs::create_directories(fixture_dir, ec);
            if (ec) {
                err = "failed to create fixture directory: " + ec.message();
                return false;
            }

            const fs::path spec_path = fixture_dir / "app_tests_tmp_iso_panel_with_entity.json";
            const fs::path csv_path = fixture_dir / "app_tests_tmp_iso_panel.csv";

            {
                std::ofstream spec_out(spec_path, std::ios::binary | std::ios::trunc);
                if (!spec_out) {
                    err = "failed to open fixture DataSpec path for write";
                    return false;
                }
                spec_out <<
                    "{\n"
                    "  \"time_col\": \"time\",\n"
                    "  \"time_kind\": \"Iso8601\",\n"
                    "  \"header_policy\": \"RequiredHeaders\",\n"
                    "  \"entity_col\": \"symbol\",\n"
                    "  \"value_cols\": [\"open\", \"high\", \"low\", \"close\", \"volume\"],\n"
                    "  \"primary_value_col\": \"close\",\n"
                    "  \"missing_tokens\": [\"\", \"NA\", \"null\", \"nan\", \"NaN\"],\n"
                    "  \"missing_default\": { \"kind\": \"NaN\" },\n"
                    "  \"sort_by_time\": true,\n"
                    "  \"dedupe\": \"Error\"\n"
                    "}\n";
            }

            {
                std::ofstream csv_out(csv_path, std::ios::binary | std::ios::trunc);
                if (!csv_out) {
                    err = "failed to open fixture CSV path for write";
                    return false;
                }
                csv_out <<
                    "symbol,time,open,high,low,close,volume\n"
                    "AAA,2024-01-01T00:00:00Z,100,101,99,100.5,1000\n"
                    "BBB,2024-01-01T00:00:00Z,200,202,198,201.0,1500\n"
                    "AAA,2024-01-02T00:00:00Z,101,103,100,102.0,1100\n"
                    "BBB,2024-01-02T00:00:00Z,201,203,199,202.5,1600\n";
            }

            out.spec_path = spec_path.string();
            out.csv_path = csv_path.string();
            return true;
        }

        bool create_relative_planspec_fixture(std::string& out_path, std::string& err) {
            PanelFixturePaths fixture{};
            if (!create_panel_fixture(fixture, err)) {
                return false;
            }

            const fs::path plan_path = fs::path(fixture.spec_path).parent_path() / "app_tests_tmp_relative_plan.json";
            std::ofstream plan_out(plan_path, std::ios::binary | std::ios::trunc);
            if (!plan_out) {
                err = "failed to open PlanSpec fixture path for write";
                return false;
            }

            plan_out <<
                "{\n"
                "  \"version\": 1,\n"
                "  \"name\": \"app_tests_relative_embedded_observations\",\n"
                "  \"series_inputs\": [\n"
                "    {\n"
                "      \"key\": \"main\",\n"
                "      \"path\": \"app_tests_tmp_iso_panel.csv\",\n"
                "      \"data_spec\": \"app_tests_tmp_iso_panel_with_entity.json\",\n"
                "      \"entity\": \"AAA\"\n"
                "    }\n"
                "  ],\n"
                "  \"bindings\": {\n"
                "    \"entry_long\": \"\",\n"
                "    \"exit_long\": \"\",\n"
                "    \"entry_short\": \"\",\n"
                "    \"exit_short\": \"\",\n"
                "    \"allow_trade\": \"\"\n"
                "  }\n"
                "}\n";

            out_path = plan_path.string();
            return true;
        }

        bool create_custom_planspec_fixture(bool include_series_inputs, std::string& out_path, std::string& err) {
            PanelFixturePaths fixture{};
            if (!create_panel_fixture(fixture, err)) {
                return false;
            }

            const fs::path fixture_dir = fs::path(fixture.spec_path).parent_path();
            const fs::path plan_path = fixture_dir / (include_series_inputs ?
                "app_tests_tmp_custom_with_series_inputs.json" :
                "app_tests_tmp_custom_no_series_inputs.json");
            std::ofstream plan_out(plan_path, std::ios::binary | std::ios::trunc);
            if (!plan_out) {
                err = "failed to open custom PlanSpec fixture path for write";
                return false;
            }

            plan_out <<
                "{\n"
                "  \"version\": 1,\n"
                "  \"name\": \"app_tests_custom_positive_close\",\n";
            if (include_series_inputs) {
                plan_out <<
                    "  \"series_inputs\": [\n"
                    "    {\n"
                    "      \"key\": \"main\",\n"
                    "      \"path\": \"app_tests_tmp_iso_panel.csv\",\n"
                    "      \"data_spec\": \"app_tests_tmp_iso_panel_with_entity.json\",\n"
                    "      \"entity\": \"AAA\"\n"
                    "    }\n"
                    "  ],\n";
            }
            plan_out <<
                "  \"trade_mode\": \"LongOnly\",\n"
                "  \"numeric_nodes\": [\n"
                "    { \"id\": \"c\", \"op\": \"core.close\" },\n"
                "    { \"id\": \"z\", \"op\": \"core.const\", \"params\": { \"value\": 0.0 } }\n"
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

            out_path = plan_path.string();
            return true;
        }

        bool create_custom_missing_runtime_ref_planspec_fixture(std::string& out_path, std::string& err) {
            PanelFixturePaths fixture{};
            if (!create_panel_fixture(fixture, err)) {
                return false;
            }

            const fs::path fixture_dir = fs::path(fixture.spec_path).parent_path();
            const fs::path plan_path = fixture_dir / "app_tests_tmp_custom_missing_runtime_ref.json";
            std::ofstream plan_out(plan_path, std::ios::binary | std::ios::trunc);
            if (!plan_out) {
                err = "failed to open missing-runtime-ref PlanSpec fixture path for write";
                return false;
            }

            plan_out <<
                "{\n"
                "  \"version\": 1,\n"
                "  \"name\": \"app_tests_custom_missing_runtime_ref\",\n"
                "  \"trade_mode\": \"LongOnly\",\n"
                "  \"numeric_nodes\": [\n"
                "    { \"id\": \"other\", \"op\": \"core.ref\", \"params\": { \"ref\": \"ZZZ:close\" } },\n"
                "    { \"id\": \"z\", \"op\": \"core.const\", \"params\": { \"value\": 0.0 } }\n"
                "  ],\n"
                "  \"bool_nodes\": [\n"
                "    { \"id\": \"entry\", \"op\": \"cmp.gt\", \"args\": [\"other\", \"z\"] },\n"
                "    { \"id\": \"exit\", \"op\": \"cmp.lt\", \"args\": [\"other\", \"z\"] }\n"
                "  ],\n"
                "  \"bindings\": {\n"
                "    \"entry_long\": \"entry\",\n"
                "    \"exit_long\": \"exit\",\n"
                "    \"entry_short\": \"\",\n"
                "    \"exit_short\": \"\",\n"
                "    \"allow_trade\": \"\"\n"
                "  }\n"
                "}\n";

            out_path = plan_path.string();
            return true;
        }

        bool test_parse_dump_ops_alias(TestContext& t) {
            t.out << "[app_cli] dump-ops alias parsing\n";
            char arg0[] = "prog";
            char arg1[] = "--dump_ops";
            char* argv[] = { arg0, arg1 };

            AppCommand cmd{};
            std::string err;
            const bool ok = parse_app_command(2, argv, cmd, err);
            t.check(ok, "parse_app_command accepts --dump_ops alias");
            t.check(err.empty(), "dump-ops alias parse has no error");
            t.check(cmd.kind == CommandKind::DumpOps, "dump-ops alias resolves to DumpOps command");
            return t.failed == 0;
        }

        bool test_parse_contract_sections(TestContext& t) {
            t.out << "[app_cli] contract-section accumulation\n";
            char arg0[] = "prog";
            char arg1[] = "--run-contract-suite";
            char arg2[] = "--contract-section";
            char arg3[] = "runtime_gates";
            char arg4[] = "--contract-section";
            char arg5[] = "compile_bridge_runtime";
            char* argv[] = { arg0, arg1, arg2, arg3, arg4, arg5 };

            AppCommand cmd{};
            std::string err;
            const bool ok = parse_app_command(6, argv, cmd, err);
            t.check(ok, "contract suite command parses");
            t.check(cmd.kind == CommandKind::RunContractSuite, "contract suite command kind is correct");
            t.check(cmd.contract_suite.sections.size() == 2, "repeated contract sections are preserved");
            t.check(cmd.contract_suite.sections.size() == 2 &&
                cmd.contract_suite.sections[0] == "runtime_gates" &&
                cmd.contract_suite.sections[1] == "compile_bridge_runtime",
                "contract sections preserve input order");
            return t.failed == 0;
        }

        bool test_parse_align_dump_requires_series_key(TestContext& t) {
            t.out << "[app_cli] align-dump validation\n";
            char arg0[] = "prog";
            char arg1[] = "--align-dump";
            char arg2[] = "spec.json";
            char arg3[] = "data.csv";
            char* argv[] = { arg0, arg1, arg2, arg3 };

            AppCommand cmd{};
            std::string err;
            const bool ok = parse_app_command(4, argv, cmd, err);
            t.check(!ok, "align-dump parse fails without --series-key-col");
            t.check(err == "--series-key-col <col> is required for --align-dump",
                "align-dump failure message is explicit");
            return t.failed == 0;
        }

        bool test_parse_panel_topk_validation(TestContext& t) {
            t.out << "[app_cli] panel topk validation\n";
            char arg0[] = "prog";
            char arg1[] = "--run-panel";
            char arg2[] = "spec.json";
            char arg3[] = "data.csv";
            char arg4[] = "--topk";
            char arg5[] = "0";
            char* argv[] = { arg0, arg1, arg2, arg3, arg4, arg5 };

            AppCommand cmd{};
            std::string err;
            const bool ok = parse_app_command(6, argv, cmd, err);
            t.check(!ok, "panel parse rejects non-positive --topk");
            t.check(err == "--topk must be a positive integer",
                "panel topk error message is explicit");
            return t.failed == 0;
        }

        bool test_parse_interactive_data_spec_mode(TestContext& t) {
            t.out << "[app_cli] interactive data-spec parsing\n";
            char arg0[] = "prog";
            char arg1[] = "--data-spec";
            char arg2[] = "spec.json";
            char arg3[] = "data.csv";
            char arg4[] = "--format";
            char arg5[] = "iso";
            char* argv[] = { arg0, arg1, arg2, arg3, arg4, arg5 };

            AppCommand cmd{};
            std::string err;
            const bool ok = parse_app_command(6, argv, cmd, err);
            t.check(ok, "interactive data-spec command parses");
            t.check(cmd.kind == CommandKind::Interactive, "interactive data-spec stays Interactive command");
            t.check(cmd.interactive.has_runtime_data_spec, "interactive command records --data-spec presence");
            t.check(cmd.interactive.runtime_data_spec_path.find("spec.json") != std::string::npos,
                "interactive command records spec path");
            t.check(cmd.interactive.csv_path.find("data.csv") != std::string::npos,
                "interactive command records csv path");
            t.check(cmd.interactive.requested_format == sugar::InputProfile::IsoOhlcvTime,
                "interactive command records requested --format");
            return t.failed == 0;
        }

        bool test_build_aligned_runtime_input(TestContext& t) {
            t.out << "[app_runtime] aligned runtime input build\n";
            PanelFixturePaths fixture{};
            std::string fixture_err;
            const bool fixture_ok = create_panel_fixture(fixture, fixture_err);
            t.check(fixture_ok, "panel fixture files are created");
            if (!fixture_ok) return false;

            sugar::DataSpec ds{};
            std::string err;
            const bool load_ok = sugar::load_data_spec_file(
                fixture.spec_path,
                ds,
                err);
            t.check(load_ok, "fixture DataSpec loads");
            if (!load_ok) return false;

            sugar::align::AlignConfig cfg{};
            cfg.calendar = sugar::align::CalendarPolicy::Intersection;
            cfg.missing = sugar::align::MissingPolicy::NaN;

            AlignedRuntimeInput runtime{};
            const bool ok = sugar::app::build_aligned_runtime_input(
                ds,
                fixture.csv_path,
                ds.primary_value_col,
                cfg,
                runtime,
                err);

            t.check(ok, "aligned runtime input builds from fixture");
            t.check(ok && runtime.frameset && runtime.aligned_bundle, "aligned runtime input owns frameset and bundle");
            t.check(ok && runtime.source_kind == sugar::RuntimeObservationSourceKind::AppSuppliedObservations,
                "aligned runtime input records app observation source kind");
            t.check(ok && runtime.run_manifest.primary_series_key == runtime.primary_ref.series,
                "aligned runtime input manifest keeps primary series");
            const auto observation_cap_it = std::find_if(
                runtime.run_manifest.capability_outcomes.begin(),
                runtime.run_manifest.capability_outcomes.end(),
                [](const sugar::CapabilityOutcomeSnapshot& outcome) {
                    return outcome.domain == "observation_surface" &&
                        outcome.subject == "Temporal";
                });
            t.check(ok && observation_cap_it != runtime.run_manifest.capability_outcomes.end(),
                "aligned runtime input manifest records observation capability outcomes");
            t.check(ok && !runtime.primary_entity_key.empty(), "aligned runtime input resolves primary entity");
            t.check(ok && runtime.primary_series_index != static_cast<std::size_t>(-1), "aligned runtime input resolves primary series index");
            t.check(ok && runtime.series_key_to_entity.size() == runtime.frameset->by_entity.size(),
                "aligned runtime input records series-to-entity provenance");
            t.check(ok && runtime.aligned_bundle->frames.size() == runtime.aligned_bundle->series_keys.size(),
                "aligned runtime input keeps aligned frame/key cardinality consistent");

            sugar::ResolvedRuntimeInput shared_runtime{};
            const bool shared_ok = sugar::build_resolved_runtime_input(
                ds,
                fixture.csv_path,
                ds.primary_value_col,
                cfg,
                shared_runtime,
                err);
            t.check(shared_ok, "shared resolved runtime input builds from fixture");
            t.check(shared_ok && ok && shared_runtime.primary_ref.series == runtime.primary_ref.series,
                "app runtime wrapper matches shared primary series");
            t.check(shared_ok && ok && shared_runtime.primary_ref.col.v == runtime.primary_ref.col.v,
                "app runtime wrapper matches shared primary column");
            t.check(shared_ok && ok && shared_runtime.run_manifest.primary_series_key == runtime.run_manifest.primary_series_key,
                "app runtime wrapper matches shared run manifest primary series");
            return t.failed == 0;
        }

        bool test_interactive_runtime_auto_detects_daily_time_header_as_legacy(TestContext& t) {
            t.out << "[app_runtime] auto-detect handles time-header daily CSV\n";
            std::string csv_path;
            std::string fixture_err;
            const bool fixture_ok = create_time_header_daily_fixture(csv_path, fixture_err);
            t.check(fixture_ok, "time-header daily fixture CSV is created");
            if (!fixture_ok) return false;

            InteractiveCommand cmd{};
            cmd.csv_path = csv_path;
            cmd.requested_format = sugar::InputProfile::Auto;

            RuntimeSession session{};
            std::string err;
            const bool ok = sugar::app::build_interactive_runtime_session(cmd, csv_path, session, err);
            t.check(ok, "interactive runtime session builds for time-header daily CSV");
            t.check(ok && session.runtime.ingest_spec.has_value(), "interactive runtime session keeps resolved ingest DataSpec");
            t.check(ok && session.runtime.ingest_spec->input_profile == sugar::InputProfile::LegacyOhlcvDate,
                "auto-detect resolves time-header daily CSV to legacy date profile");
            t.check(ok && session.runtime.ingest_spec->time_kind == sugar::TimeKind::YyyyMmDd,
                "auto-detect keeps YyyyMmDd time kind for time-header daily CSV");
            t.check(ok && session.runtime.ingest_spec->time_col == "date",
                "legacy date profile keeps canonical time_col for time-header daily CSV");
            t.check(ok && session.runtime.ingest_spec->rename.find("time") != session.runtime.ingest_spec->rename.end() &&
                session.runtime.ingest_spec->rename.find("time")->second == "date",
                "legacy date profile maps time header into canonical date column");
            t.check(ok && session.runtime.ingest_spec->rename.find("Volume") != session.runtime.ingest_spec->rename.end() &&
                session.runtime.ingest_spec->rename.find("Volume")->second == "volume",
                "runtime preset reuses canonical Volume rename");
            t.check(ok && !session.notices.empty() &&
                session.notices.front().find("runtime format preset: legacy_ohlcv_date") != std::string::npos,
                "interactive runtime notice reports legacy date preset");
            t.check(ok && !session.notices.empty() &&
                session.notices.front().find("date-like sample values") != std::string::npos,
                "interactive runtime notice explains date-like time-column sample detection");
            t.check(ok && session.runtime.run_manifest.preprocess.time_kind == sugar::TimeKind::YyyyMmDd,
                "resolved runtime manifest records YyyyMmDd time kind");
            t.check(ok && session.runtime.aligned_bundle && session.runtime.aligned_bundle->time.size() == 2,
                "resolved runtime retains expected daily row count");
            return t.failed == 0;
        }

        bool test_interactive_runtime_auto_detects_iso_time_header_as_iso(TestContext& t) {
            t.out << "[app_runtime] auto-detect keeps ISO time-header CSV as ISO\n";
            PanelFixturePaths fixture{};
            std::string fixture_err;
            const bool fixture_ok = create_panel_fixture(fixture, fixture_err);
            t.check(fixture_ok, "ISO time-header fixture CSV is created");
            if (!fixture_ok) return false;

            InteractiveCommand cmd{};
            cmd.csv_path = fixture.csv_path;
            cmd.requested_format = sugar::InputProfile::Auto;

            RuntimeSession session{};
            std::string err;
            const bool ok = sugar::app::build_interactive_runtime_session(cmd, fixture.csv_path, session, err);
            t.check(ok, "interactive runtime session builds for ISO time-header CSV");
            t.check(ok && session.runtime.ingest_spec.has_value(), "interactive runtime session keeps resolved ISO ingest DataSpec");
            t.check(ok && session.runtime.ingest_spec->input_profile == sugar::InputProfile::IsoOhlcvTime,
                "auto-detect keeps ISO profile for ISO time-header CSV");
            t.check(ok && session.runtime.ingest_spec->time_kind == sugar::TimeKind::Iso8601,
                "auto-detect keeps Iso8601 time kind for ISO time-header CSV");
            t.check(ok && !session.notices.empty() &&
                session.notices.front().find("runtime format preset: iso_ohlcv_time") != std::string::npos,
                "interactive runtime notice reports ISO preset");
            t.check(ok && !session.notices.empty() &&
                session.notices.front().find("ISO timestamp sample values") != std::string::npos,
                "interactive runtime notice explains ISO sample detection");
            return t.failed == 0;
        }

        bool test_shared_runtime_normalization_helpers(TestContext& t) {
            t.out << "[run_pipeline] shared normalization helpers\n";
            PanelFixturePaths fixture{};
            std::string fixture_err;
            const bool fixture_ok = create_panel_fixture(fixture, fixture_err);
            t.check(fixture_ok, "shared-helper fixture files are created");
            if (!fixture_ok) return false;

            sugar::DataSpec ds{};
            std::string err;
            const bool load_ok = sugar::load_data_spec_file(fixture.spec_path, ds, err);
            t.check(load_ok, "shared-helper DataSpec loads");
            if (!load_ok) return false;

            sugar::align::AlignConfig cfg{};
            cfg.calendar = sugar::align::CalendarPolicy::Intersection;
            cfg.missing = sugar::align::MissingPolicy::NaN;

            sugar::FrameSet shared_fs{};
            sugar::PreprocessReport shared_report{};
            const bool shared_ok = sugar::preprocess_to_frameset(
                ds,
                fixture.csv_path,
                shared_fs,
                shared_report,
                err,
                cfg);
            t.check(shared_ok, "run_pipeline preprocess helper builds FrameSet");
            t.check(shared_ok && shared_report.produced_frameset, "run_pipeline preprocess helper preserves frameset report");
            t.check(shared_ok && shared_fs.by_entity.size() == 2, "run_pipeline preprocess helper preserves entity split");
            if (!shared_ok) return false;

            sugar::FrameSet app_fs{};
            sugar::PreprocessReport app_report{};
            const bool app_ok = sugar::app::preprocess_to_frameset(ds, fixture.csv_path, app_fs, app_report, err, cfg);
            t.check(app_ok, "app preprocess helper still succeeds through shared seam");
            t.check(app_ok && app_fs.by_entity.size() == shared_fs.by_entity.size(),
                "app preprocess helper matches shared entity count");
            t.check(app_ok && app_report.produced_frameset == shared_report.produced_frameset,
                "app preprocess helper matches shared frameset report");

            const std::string shared_primary = sugar::pick_primary_entity_key(shared_fs);
            const std::string app_primary = sugar::app::pick_primary_entity_key(app_fs);
            t.check(shared_primary == app_primary, "app and shared helpers agree on primary entity selection");

            std::shared_ptr<sugar::align::AlignedBundle> shared_bundle{};
            const bool shared_align_ok = sugar::align_frameset(shared_fs, cfg, shared_bundle, err);
            t.check(shared_align_ok, "run_pipeline align helper builds aligned bundle");
            t.check(shared_align_ok && shared_bundle && shared_bundle->series_keys.size() == shared_fs.by_entity.size(),
                "run_pipeline align helper preserves series cardinality");

            std::shared_ptr<sugar::align::AlignedBundle> app_bundle{};
            const bool app_align_ok = sugar::app::align_frameset(app_fs, cfg, app_bundle, err);
            t.check(app_align_ok, "app align helper still succeeds through shared seam");
            t.check(app_align_ok && app_bundle && shared_bundle &&
                app_bundle->series_keys == shared_bundle->series_keys,
                "app and shared align helpers agree on series ordering");
            return t.failed == 0;
        }

        bool test_dump_ops_dispatch_smoke(TestContext& t) {
            t.out << "[app_dispatch] dump-ops smoke\n";
            std::ostringstream out;
            const int rc = run_dump_ops_command(out);
            const std::string text = out.str();
            t.check(rc == 0, "dump-ops command returns success");
            t.check(text.find("ts.sma") != std::string::npos, "dump-ops output includes ts.sma");
            t.check(text.find("spatial.identity") != std::string::npos, "dump-ops output includes spatial.identity");
            t.check(text.find("spatial.radius_mean") != std::string::npos, "dump-ops output includes spatial.radius_mean");
            t.check(text.find("st.radius_mean") != std::string::npos, "dump-ops output includes st.radius_mean");
            t.check(text.find("required_topology_families") != std::string::npos,
                "dump-ops output includes topology family metadata");
            t.check(text.find("topology_binding_param") != std::string::npos,
                "dump-ops output includes topology binding metadata");
            t.check(text.find("default_locality_subject") != std::string::npos,
                "dump-ops output includes locality-subject metadata");
            t.check(text.find("primary_series") != std::string::npos,
                "dump-ops output exposes primary-series locality subject default");
            t.check(text.find("builtin.spatial.metric_2d") != std::string::npos,
                "dump-ops output includes built-in spatial topology default");
            t.check(text.find("builtin.spatiotemporal.composite") != std::string::npos,
                "dump-ops output includes built-in spatiotemporal topology default");
            t.check(text.find("`topology`") != std::string::npos,
                "dump-ops output exposes the locality topology binding param name");
            t.check(text.find("`subject_series`") != std::string::npos,
                "dump-ops output exposes the locality subject binding param name");
            t.check(text.find("caller-supplied `DataView` observations") != std::string::npos,
                "dump-ops contract text describes DataView-first custom runtime");
            t.check(text.find("Legacy embedded observation inputs (`series_inputs`) are optional compatibility metadata") != std::string::npos,
                "dump-ops contract text marks legacy embedded observation inputs as optional adapter metadata");
            t.check(text.find("not a primary app/runtime surface") != std::string::npos,
                "dump-ops contract text demotes legacy embedded observation inputs from primary runtime surface");
            t.check(text.find("requires `series_inputs`") == std::string::npos,
                "dump-ops contract text no longer claims series_inputs is universally required");
            return t.failed == 0;
        }

        bool test_prep_dump_dispatch_smoke(TestContext& t) {
            t.out << "[app_dispatch] prep-dump smoke\n";
            PanelFixturePaths fixture{};
            std::string fixture_err;
            const bool fixture_ok = create_panel_fixture(fixture, fixture_err);
            t.check(fixture_ok, "prep-dump fixture files are created");
            if (!fixture_ok) return false;

            AppCommand cmd{};
            cmd.kind = CommandKind::PrepDump;
            cmd.prep_dump.spec_path = fixture.spec_path;
            cmd.prep_dump.csv_path = fixture.csv_path;

            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_prep_dump_command(cmd, out, err);
            const std::string text = out.str();
            t.check(rc == 0, "prep-dump command returns success");
            t.check(err.str().empty(), "prep-dump command produces no error output");
            t.check(text.find("DataSpec summary") != std::string::npos, "prep-dump output includes DataSpec summary");
            return t.failed == 0;
        }

        bool test_prep_dump_spatial_output_is_row_domain_honest(TestContext& t) {
            t.out << "[app_dispatch] prep-dump spatial row-domain reporting\n";
            std::string spec_path;
            std::string csv_path;
            std::string fixture_err;
            const bool fixture_ok = create_spatial_only_fixture(spec_path, csv_path, fixture_err);
            t.check(fixture_ok, "spatial-only prep-dump fixture files are created");
            if (!fixture_ok) return false;

            AppCommand cmd{};
            cmd.kind = CommandKind::PrepDump;
            cmd.prep_dump.spec_path = spec_path;
            cmd.prep_dump.csv_path = csv_path;

            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_prep_dump_command(cmd, out, err);
            const std::string text = out.str();

            t.check(rc == 0, "spatial-only prep-dump command returns success");
            t.check(err.str().empty(), "spatial-only prep-dump command produces no error output");
            t.check(text.find("time_col: <none>") != std::string::npos,
                "spatial-only prep-dump reports missing time_col");
            t.check(text.find("time_kind: <none>") != std::string::npos,
                "spatial-only prep-dump reports missing time_kind");
            t.check(text.find("row_domain: 0..2") != std::string::npos,
                "spatial-only prep-dump reports row domain instead of time range");
            t.check(text.find("time: 0..2") == std::string::npos,
                "spatial-only prep-dump avoids fabricated time summary");
            return t.failed == 0;
        }

        bool test_prep_dump_reports_observation_capabilities(TestContext& t) {
            t.out << "[app_dispatch] prep-dump observation capability reporting\n";
            std::string spec_path;
            std::string csv_path;
            std::string fixture_err;
            const bool fixture_ok = create_spatial_only_fixture(spec_path, csv_path, fixture_err);
            t.check(fixture_ok, "spatial-only capability fixture files are created");
            if (!fixture_ok) return false;

            AppCommand cmd{};
            cmd.kind = CommandKind::PrepDump;
            cmd.prep_dump.spec_path = spec_path;
            cmd.prep_dump.csv_path = csv_path;

            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_prep_dump_command(cmd, out, err);
            const std::string text = out.str();

            t.check(rc == 0, "prep-dump capability command returns success");
            t.check(err.str().empty(), "prep-dump capability command produces no error output");
            t.check(text.find("Observation capability") != std::string::npos,
                "prep-dump output includes observation capability header");
            t.check(text.find("Row domain: valid") != std::string::npos,
                "prep-dump reports row-domain validity for spatial-only data");
            t.check(text.find("Temporal: invalid - no semantic time axis") != std::string::npos,
                "prep-dump explains temporal invalidity for spatial-only data");
            t.check(text.find("Spatial: valid") != std::string::npos,
                "prep-dump explains spatial validity for spatial-only data");
            t.check(text.find("Spatiotemporal: invalid - no semantic time axis") != std::string::npos,
                "prep-dump explains spatiotemporal invalidity for spatial-only data");
            t.check(text.find("Spatial panel: conditional - provide entity grouping schema") != std::string::npos,
                "prep-dump explains spatial-panel conditionality for spatial-only single-series data");
            t.check(text.find("builtin.spatial.metric_2d") != std::string::npos,
                "prep-dump exposes built-in spatial topology inventory");
            return t.failed == 0;
        }

        bool test_prep_dump_reports_panel_capability_incompatibility(TestContext& t) {
            t.out << "[app_dispatch] prep-dump panel capability incompatibility\n";
            std::string spec_path;
            std::string csv_path;
            std::string fixture_err;
            const bool fixture_ok = create_spatial_panel_fixture(spec_path, csv_path, fixture_err);
            t.check(fixture_ok, "spatial-panel capability fixture files are created");
            if (!fixture_ok) return false;

            AppCommand cmd{};
            cmd.kind = CommandKind::PrepDump;
            cmd.prep_dump.spec_path = spec_path;
            cmd.prep_dump.csv_path = csv_path;

            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_prep_dump_command(cmd, out, err);
            const std::string text = out.str();

            t.check(rc == 0, "spatial-panel prep-dump command returns success");
            t.check(err.str().empty(), "spatial-panel prep-dump command produces no error output");
            t.check(text.find("Spatial panel: valid") != std::string::npos,
                "prep-dump explains spatial panel validity for spatial-only panel data");
            t.check(text.find("Temporal panel: invalid - no shared per-entity time axis") != std::string::npos,
                "prep-dump explains temporal panel invalidity for spatial-only panel data");
            t.check(text.find("Spatiotemporal panel: invalid - no shared per-entity time axis") != std::string::npos,
                "prep-dump explains spatiotemporal panel invalidity for spatial-only panel data");
            return t.failed == 0;
        }

        bool test_runtime_graph_preview_spatial_output_is_row_domain_honest(TestContext& t) {
            t.out << "[runtime_graph_preview] spatial row-domain reporting\n";
            std::string spec_path;
            std::string csv_path;
            std::string fixture_err;
            const bool fixture_ok = create_spatial_only_fixture(spec_path, csv_path, fixture_err);
            t.check(fixture_ok, "spatial-only runtime-preview fixture files are created");
            if (!fixture_ok) return false;

            sugar::DataSpec ds{};
            std::string err;
            const bool load_ok = sugar::load_data_spec_file(spec_path, ds, err);
            t.check(load_ok, "spatial-only runtime-preview fixture DataSpec loads");
            if (!load_ok) return false;

            sugar::align::AlignConfig cfg{};
            cfg.calendar = sugar::align::CalendarPolicy::Intersection;
            cfg.missing = sugar::align::MissingPolicy::NaN;

            AlignedRuntimeInput runtime{};
            const bool runtime_ok = sugar::app::build_aligned_runtime_input(
                ds, csv_path, ds.primary_value_col, cfg, runtime, err);
            t.check(runtime_ok, "spatial-only runtime-preview fixture builds runtime input");
            if (!runtime_ok) return false;

            const sugar::DataView dv(*runtime.aligned_bundle);
            const auto capability_report = sugar::diagnose_observation_capabilities(
                sugar::make_observation_world_facts(runtime));
            std::ostringstream out;
            sugar::print_runtime_graph_preview(
                dv, runtime.primary_ref, runtime.run_manifest, "spatial-test", out, 8, &capability_report);
            const std::string text = out.str();

            t.check(text.find("Observation capability") != std::string::npos,
                "spatial-only runtime preview includes observation capability header");
            t.check(text.find("Row domain: valid") != std::string::npos,
                "spatial-only runtime preview reports row-domain validity");
            t.check(text.find("Temporal: invalid - no semantic time axis") != std::string::npos,
                "spatial-only runtime preview explains temporal invalidity");
            t.check(text.find("Spatial: valid") != std::string::npos,
                "spatial-only runtime preview explains spatial validity");
            t.check(text.find("Spatial panel: conditional - provide entity grouping schema") != std::string::npos,
                "spatial-only runtime preview explains spatial-panel conditionality");
            t.check(text.find("builtin.spatial.metric_2d") != std::string::npos,
                "spatial-only runtime preview exposes built-in spatial topology inventory");
            t.check(text.find("time_kind: <none>") != std::string::npos,
                "spatial-only runtime preview reports missing time kind");
            t.check(text.find("time_resolution: <none>") != std::string::npos,
                "spatial-only runtime preview reports missing time resolution");
            t.check(text.find("row_domain: 0 .. 2") != std::string::npos,
                "spatial-only runtime preview reports row domain");
            t.check(text.find("latest: 30.0000 @ row#2") != std::string::npos,
                "spatial-only runtime preview anchors latest value to row domain");
            t.check(text.find("time_range:") == std::string::npos,
                "spatial-only runtime preview avoids fabricated time range");
            t.check(text.find("rows: 0") == std::string::npos,
                "spatial-only runtime preview no longer collapses to zero rows");
            return t.failed == 0;
        }

        bool test_shared_runtime_rejects_non_temporal_multiseries_alignment(TestContext& t) {
            t.out << "[run_pipeline] non-temporal multi-series alignment policy is explicit\n";
            std::string spec_path;
            std::string csv_path;
            std::string fixture_err;
            const bool fixture_ok = create_spatial_panel_fixture(spec_path, csv_path, fixture_err);
            t.check(fixture_ok, "spatial-panel fixture files are created");
            if (!fixture_ok) return false;

            sugar::DataSpec ds{};
            std::string err;
            const bool load_ok = sugar::load_data_spec_file(spec_path, ds, err);
            t.check(load_ok, "spatial-panel fixture DataSpec loads");
            if (!load_ok) return false;

            sugar::align::AlignConfig cfg{};
            cfg.calendar = sugar::align::CalendarPolicy::Intersection;
            cfg.missing = sugar::align::MissingPolicy::NaN;

            sugar::ResolvedRuntimeInput runtime{};
            const bool runtime_ok = sugar::build_resolved_runtime_input(
                ds, csv_path, ds.primary_value_col, cfg, runtime, err);
            t.check(!runtime_ok, "shared runtime builder rejects non-temporal multi-series alignment without policy");
            t.check(err.find("non-temporal multi-series alignment requires explicit policy") != std::string::npos,
                "shared runtime builder reports explicit non-temporal alignment policy failure");
            return t.failed == 0;
        }

        bool test_align_dump_reports_observation_capabilities(TestContext& t) {
            t.out << "[app_dispatch] align-dump observation capability reporting\n";
            PanelFixturePaths fixture{};
            std::string fixture_err;
            const bool fixture_ok = create_panel_fixture(fixture, fixture_err);
            t.check(fixture_ok, "temporal-panel align-dump fixture files are created");
            if (!fixture_ok) return false;

            AppCommand cmd{};
            cmd.kind = CommandKind::AlignDump;
            cmd.align_dump.spec_path = fixture.spec_path;
            cmd.align_dump.csv_path = fixture.csv_path;
            cmd.align_dump.series_key_col = "symbol";
            cmd.align_dump.calendar = sugar::align::CalendarPolicy::Intersection;

            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_align_dump_command(cmd, out, err);
            const std::string text = out.str();

            t.check(rc == 0, "temporal-panel align-dump returns success");
            t.check(err.str().empty(), "temporal-panel align-dump produces no error output");
            t.check(text.find("Observation capability") != std::string::npos,
                "align-dump output includes observation capability header");
            t.check(text.find("Temporal panel: valid") != std::string::npos,
                "align-dump reports temporal panel validity");
            t.check(text.find("Spatial panel: conditional - provide spatial neighborhood/index schema") != std::string::npos,
                "align-dump reports spatial panel conditionality");
            t.check(text.find("Spatiotemporal panel: conditional - provide spatial neighborhood/index schema") != std::string::npos,
                "align-dump reports spatiotemporal panel conditionality");
            t.check(text.find("builtin.temporal.window") != std::string::npos,
                "align-dump exposes built-in temporal topology inventory");
            return t.failed == 0;
        }

        bool test_align_dump_rejects_non_temporal_multiseries_alignment(TestContext& t) {
            t.out << "[app_dispatch] align-dump rejects non-temporal multi-series alignment\n";
            std::string spec_path;
            std::string csv_path;
            std::string fixture_err;
            const bool fixture_ok = create_spatial_panel_fixture(spec_path, csv_path, fixture_err);
            t.check(fixture_ok, "spatial-panel align-dump fixture files are created");
            if (!fixture_ok) return false;

            AppCommand cmd{};
            cmd.kind = CommandKind::AlignDump;
            cmd.align_dump.spec_path = spec_path;
            cmd.align_dump.csv_path = csv_path;
            cmd.align_dump.series_key_col = "region";
            cmd.align_dump.calendar = sugar::align::CalendarPolicy::Intersection;

            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_align_dump_command(cmd, out, err);
            const std::string text = out.str();
            t.check(rc == 1, "spatial-panel align-dump returns failure for unsupported non-temporal multi-series alignment");
            t.check(text.find("Observation capability") != std::string::npos,
                "spatial-panel align-dump still reports observation capability before policy failure");
            t.check(text.find("Spatial panel: valid") != std::string::npos,
                "spatial-panel align-dump reports spatial panel validity before policy failure");
            t.check(text.find("Temporal panel: invalid - no shared per-entity time axis") != std::string::npos,
                "spatial-panel align-dump explains temporal panel incompatibility");
            t.check(err.str().find("non-temporal multi-series alignment requires explicit policy") != std::string::npos,
                "spatial-panel align-dump reports explicit non-temporal alignment policy failure");
            return t.failed == 0;
        }

        bool test_plan_capability_report_surfaces_topology_conditions(TestContext& t) {
            t.out << "[app_reporters] plan capability topology reporting\n";
            PanelFixturePaths fixture{};
            std::string fixture_err;
            const bool fixture_ok = create_panel_fixture(fixture, fixture_err);
            t.check(fixture_ok, "plan capability topology fixture files are created");
            if (!fixture_ok) return false;

            sugar::DataSpec ds{};
            std::string err;
            const bool load_ok = sugar::load_data_spec_file(fixture.spec_path, ds, err);
            t.check(load_ok, "plan capability topology fixture DataSpec loads");
            if (!load_ok) return false;

            sugar::align::AlignConfig cfg{};
            cfg.calendar = sugar::align::CalendarPolicy::Intersection;
            cfg.missing = sugar::align::MissingPolicy::NaN;

            sugar::ResolvedRuntimeInput runtime{};
            const bool runtime_ok = sugar::build_resolved_runtime_input(
                ds, fixture.csv_path, ds.primary_value_col, cfg, runtime, err);
            t.check(runtime_ok, "plan capability topology fixture builds runtime input");
            if (!runtime_ok) return false;

            sugar::PlanSpec spec{};
            spec.numeric_nodes = {
                sugar::NodeSpec{ "c", "core.close", {}, {} },
                sugar::NodeSpec{ "mean", "spatial.radius_mean", { "c" }, {
                    {"radius", 1.0}
                } },
            };

            std::ostringstream out;
            sugar::app::print_plan_capability_report(spec, runtime, out);
            const std::string text = out.str();

            t.check(text.find("topology_names: builtin.spatial.metric_2d") != std::string::npos,
                "plan capability report summarizes resolved topology names");
            t.check(text.find("topology=builtin.spatial.metric_2d") != std::string::npos,
                "plan capability invalid-node detail includes resolved topology name");
            t.check(text.find("missing required topology family: spatial_metric_2d") != std::string::npos,
                "plan capability report explains missing required topology family");
            t.check(text.find("provide spatial neighborhood/index schema") != std::string::npos,
                "plan capability topology-family failure preserves providable spatial hint");
            t.check(text.find("missing named topology: 'builtin.spatial.metric_2d'") != std::string::npos,
                "plan capability report explains missing built-in named topology when spatial topology is absent");
            return t.failed == 0;
        }

        bool test_plan_capability_report_surfaces_locality_subject_provenance(TestContext& t) {
            t.out << "[app_reporters] plan capability locality-subject provenance\n";
            std::string spec_path;
            std::string csv_path;
            std::string fixture_err;
            const bool fixture_ok = create_spatiotemporal_panel_fixture(spec_path, csv_path, fixture_err);
            t.check(fixture_ok, "spatiotemporal locality-subject fixture files are created");
            if (!fixture_ok) return false;

            sugar::DataSpec ds{};
            std::string err;
            const bool load_ok = sugar::load_data_spec_file(spec_path, ds, err);
            t.check(load_ok, "spatiotemporal locality-subject fixture DataSpec loads");
            if (!load_ok) return false;

            sugar::align::AlignConfig cfg{};
            cfg.calendar = sugar::align::CalendarPolicy::Intersection;
            cfg.missing = sugar::align::MissingPolicy::NaN;

            sugar::ResolvedRuntimeInput runtime{};
            const bool runtime_ok = sugar::build_resolved_runtime_input(
                ds, csv_path, ds.primary_value_col, cfg, runtime, err);
            t.check(runtime_ok, "spatiotemporal locality-subject fixture builds runtime input");
            if (!runtime_ok) return false;

            sugar::PlanSpec spec{};
            spec.numeric_nodes = {
                sugar::NodeSpec{ "c", "core.close", {}, {} },
                sugar::NodeSpec{ "mean", "spatial.radius_mean", { "c" }, {
                    {"radius", 0.2},
                    {"subject_series", std::string("B")}
                } },
            };

            std::ostringstream out;
            sugar::app::print_plan_capability_report(spec, runtime, out);
            const std::string text = out.str();

            t.check(text.find("status: valid") != std::string::npos,
                "plan capability report remains valid for explicit locality-subject binding");
            t.check(text.find("topology_usage:") != std::string::npos,
                "plan capability report prints topology usage summary");
            t.check(text.find("mean (spatial.radius_mean): topology=builtin.spatial.metric_2d") != std::string::npos,
                "plan capability topology usage summary includes resolved topology name");
            t.check(text.find("locality=series_key(B) -> series=B, entity=B") != std::string::npos,
                "plan capability topology usage summary includes resolved locality subject provenance");
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
            return t.failed == 0;
        }

        bool test_app_custom_surfaces_avoid_spec_only_runtime(TestContext& t) {
            t.out << "[app_sources] custom surfaces avoid spec-only runtime path\n";
            const fs::path repo_root = find_repo_root();
            t.check(!repo_root.empty(), "repo root is found for app source scan");
            if (repo_root.empty()) return false;

            const std::vector<fs::path> files = {
                repo_root / "app" / "app_interactive.cpp",
                repo_root / "app" / "app_commands.cpp",
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

        bool test_embedded_observation_runtime_resolves_relative_paths(TestContext& t) {
            t.out << "[run_pipeline] relative legacy embedded observation resolution\n";
            std::string plan_path;
            std::string fixture_err;
            const bool fixture_ok = create_relative_planspec_fixture(plan_path, fixture_err);
            t.check(fixture_ok, "relative PlanSpec fixture files are created");
            if (!fixture_ok) return false;

            sugar::PlanSpec spec{};
            std::string err;
            const bool load_ok = sugar::load_plan_spec_file(plan_path, spec, err);
            t.check(load_ok, "relative PlanSpec fixture loads");
            if (!load_ok) return false;

            std::string primary_col_name;
            const bool primary_ok = sugar::resolve_planspec_primary_col_name(spec, nullptr, primary_col_name, err);
            t.check(primary_ok, "PlanSpec primary column resolves");
            t.check(primary_ok && primary_col_name == "close", "PlanSpec primary column defaults to close");
            if (!primary_ok) return false;

            bool has_embedded = false;
            const bool has_ok = sugar::plan_spec_has_embedded_observation_inputs(spec, has_embedded, err);
            t.check(has_ok, "relative PlanSpec embedded observation compatibility is detectable");
            t.check(has_ok && has_embedded, "relative PlanSpec reports embedded observation compatibility");
            if (!has_ok || !has_embedded) return false;

            sugar::ResolvedRuntimeInput runtime{};
            const bool runtime_ok = sugar::build_embedded_plan_observation_compat_runtime(spec, runtime, err);
            t.check(runtime_ok, "relative PlanSpec embedded observation compatibility builds runtime input");
            t.check(runtime_ok && runtime.source_kind == sugar::RuntimeObservationSourceKind::LegacyEmbeddedPlanCompat,
                "relative PlanSpec runtime records legacy compatibility source kind");
            t.check(runtime_ok && runtime.frameset != nullptr, "relative PlanSpec runtime owns selected frameset");
            t.check(runtime_ok && runtime.aligned_bundle != nullptr, "relative PlanSpec runtime owns aligned bundle");
            t.check(runtime_ok && runtime.primary_ref.series == "main", "relative PlanSpec runtime keeps primary series key");
            t.check(runtime_ok && runtime.primary_entity_key == "AAA",
                "relative PlanSpec runtime keeps primary entity provenance");
            t.check(runtime_ok && runtime.primary_series_index != static_cast<std::size_t>(-1),
                "relative PlanSpec runtime resolves primary series index");
            t.check(runtime_ok && runtime.run_manifest.primary_series_key == "main",
                "relative PlanSpec runtime manifest keeps primary series key");
            t.check(runtime_ok && runtime.series_key_to_entity["main"] == "AAA",
                "relative PlanSpec runtime records series-to-entity provenance");
            t.check(runtime_ok && runtime.aligned_bundle->find_series_index("main") != static_cast<std::size_t>(-1),
                "relative PlanSpec runtime exposes aligned series key");
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

                t.check(body.find("codex_build/test_assets") == std::string::npos,
                    "graph sample avoids codex_build path leak: " + entry.path().filename().string());
                t.check(body.find("C:/Users/") == std::string::npos,
                    "graph sample avoids checked-in absolute Windows path: " + entry.path().filename().string());
                t.check(body.find("\"series_inputs\"") == std::string::npos,
                    "active graph sample avoids legacy embedded observation inputs: " + entry.path().filename().string());
            }

            t.check(json_count > 0, "graph sample scan found JSON files");
            return t.failed == 0;
        }

    } // namespace

    bool run_all(std::ostream& out) {
        out << std::unitbuf;
        out << "=== SugarBot App Tests ===\n";
        TestContext t{ out };

        const std::vector<std::function<bool(TestContext&)>> tests = {
            test_parse_dump_ops_alias,
            test_parse_contract_sections,
            test_parse_align_dump_requires_series_key,
            test_parse_panel_topk_validation,
            test_parse_interactive_data_spec_mode,
            test_build_aligned_runtime_input,
            test_interactive_runtime_auto_detects_daily_time_header_as_legacy,
            test_interactive_runtime_auto_detects_iso_time_header_as_iso,
            test_shared_runtime_normalization_helpers,
            test_dump_ops_dispatch_smoke,
            test_prep_dump_dispatch_smoke,
            test_prep_dump_spatial_output_is_row_domain_honest,
            test_prep_dump_reports_observation_capabilities,
            test_prep_dump_reports_panel_capability_incompatibility,
            test_runtime_graph_preview_spatial_output_is_row_domain_honest,
            test_shared_runtime_rejects_non_temporal_multiseries_alignment,
            test_align_dump_reports_observation_capabilities,
            test_align_dump_rejects_non_temporal_multiseries_alignment,
            test_plan_capability_report_surfaces_topology_conditions,
            test_plan_capability_report_surfaces_locality_subject_provenance,
            test_interactive_custom_uses_app_runtime_without_series_inputs,
            test_interactive_custom_conflict_defaults_to_app_runtime,
            test_panel_custom_uses_app_runtime_without_series_inputs,
            test_panel_template_reports_plan_compatibility,
            test_panel_custom_reports_missing_runtime_ref_before_execution,
            test_app_custom_surfaces_avoid_spec_only_runtime,
            test_embedded_observation_runtime_resolves_relative_paths,
            test_checked_in_graph_specs_avoid_sandbox_paths,
        };

        for (const auto& test : tests) {
            test(t);
        }

        out << "=== App Test Summary ===\n";
        out << "Passed: " << t.passed << "\n";
        out << "Failed: " << t.failed << "\n";
        return t.failed == 0;
    }

} // namespace sugar::app::tests
