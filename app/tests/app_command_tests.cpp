#include "tests/app_test_support.h"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>

#include "commands/app_commands.h"
#include "ingest/layout_spec.h"
#include "ops/op_registry.h"
#include "reporting/app_reporters.h"
#include "runtime/app_runtime.h"
#include "sweep/metrics.h"
#include "sweep/plan_sweep.h"
#include "sweep/runtime_graph_preview.h"

namespace sugar::app::tests {

    namespace {

        bool test_dump_ops_dispatch_smoke(TestContext& t) {
            t.out << "[app_dispatch] dump-ops smoke\n";
            AppCommand cmd{};
            cmd.kind = CommandKind::DumpOps;
            std::ostringstream out;
            const int rc = run_dump_ops_command(cmd, out);
            const std::string text = out.str();
            t.check(rc == 0, "dump-ops command returns success");
            t.check(text.find("ts.sma") != std::string::npos, "dump-ops output includes ts.sma");
            t.check(text.find("core.select") != std::string::npos, "dump-ops output includes core.select");
            t.check(text.find("ts.macd") != std::string::npos, "dump-ops output includes ts.macd");
            t.check(text.find("ts.bollinger_width") != std::string::npos, "dump-ops output includes ts.bollinger_width");
            t.check(text.find("ts.entropy") != std::string::npos, "dump-ops output includes ts.entropy");
            t.check(text.find("ts.autocorrelation") != std::string::npos, "dump-ops output includes ts.autocorrelation");
            t.check(text.find("ts.hurst") != std::string::npos, "dump-ops output includes ts.hurst");
            t.check(text.find("ts.regime_change") != std::string::npos, "dump-ops output includes ts.regime_change");
            t.check(text.find("ms.correlation") != std::string::npos, "dump-ops output includes ms.correlation");
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
            t.check(text.find("`--dump-layout`") != std::string::npos,
                "dump-ops output points users to the layout contract surface");
            t.check(text.find("`--dump-plan`") != std::string::npos,
                "dump-ops output points users to the plan contract surface");
            t.check(text.find("`--align-dump <layout.json> <csv> --series-key-col <col>`") != std::string::npos,
                "dump-ops output points users to align-dump for aligned-series discovery");
            t.check(text.find("authoring_note: requires multiple aligned series/entity lanes") != std::string::npos,
                "dump-ops output includes per-op cross-series authoring notes");
            t.check(text.find("use `ts.sub` instead") != std::string::npos,
                "dump-ops output suggests ts.sub for same-series subtraction");
            return t.failed == 0;
        }

        bool test_dump_ops_module_catalog_example_sections(TestContext& t) {
            t.out << "[app_dispatch] dump-ops module/catalog/example sections\n";

            AppCommand modules_cmd{};
            modules_cmd.kind = CommandKind::DumpOps;
            modules_cmd.dump_ops.section = DumpOpsSection::Modules;
            std::ostringstream modules_out;
            const int modules_rc = run_dump_ops_command(modules_cmd, modules_out);
            const std::string modules_text = modules_out.str();
            t.check(modules_rc == 0, "dump-ops modules command returns success");
            t.check(modules_text.find("SugarBot Operator Modules And Rules") != std::string::npos,
                "dump-ops modules prints module header");
            t.check(modules_text.find("Node And Evaluation Rules") != std::string::npos,
                "dump-ops modules prints node evaluation rules");
            t.check(modules_text.find("core.ref") != std::string::npos,
                "dump-ops modules explains reference surfaces");
            t.check(modules_text.find("same-series arithmetic belongs here") != std::string::npos,
                "dump-ops modules explains ts vs ms arithmetic rule");
            t.check(modules_text.find("foundational narrow spatial locality") != std::string::npos,
                "dump-ops modules marks spatial locality as foundational/narrow");
            t.check(modules_text.find("**ts.sma**") == std::string::npos,
                "dump-ops modules avoids per-op catalog detail");

            AppCommand ops_cmd{};
            ops_cmd.kind = CommandKind::DumpOps;
            ops_cmd.dump_ops.section = DumpOpsSection::Operations;
            std::ostringstream ops_out;
            const int ops_rc = run_dump_ops_command(ops_cmd, ops_out);
            const std::string ops_text = ops_out.str();
            t.check(ops_rc == 0, "dump-ops ops command returns success");
            t.check(ops_text.find("SugarBot Operator Catalog") != std::string::npos,
                "dump-ops ops prints catalog header");
            t.check(ops_text.find("**ts.sma**") != std::string::npos,
                "dump-ops ops includes operator detail");
            t.check(ops_text.find("Example 1:") == std::string::npos,
                "dump-ops ops avoids working examples");

            AppCommand examples_cmd{};
            examples_cmd.kind = CommandKind::DumpOps;
            examples_cmd.dump_ops.section = DumpOpsSection::Examples;
            std::ostringstream examples_out;
            const int examples_rc = run_dump_ops_command(examples_cmd, examples_out);
            const std::string examples_text = examples_out.str();
            t.check(examples_rc == 0, "dump-ops examples command returns success");
            t.check(examples_text.find("SugarBot Operator Working Examples") != std::string::npos,
                "dump-ops examples prints examples header");
            t.check(examples_text.find("Spatial Radius Mean") != std::string::npos,
                "dump-ops examples includes spatial example");
            t.check(examples_text.find("## Module:") == std::string::npos,
                "dump-ops examples avoids full op catalog");

            return t.failed == 0;
        }

        bool test_help_dispatch_smoke(TestContext& t) {
            t.out << "[app_dispatch] help smoke\n";
            std::ostringstream out;
            const int rc = run_help_command(out);
            const std::string text = out.str();

            t.check(rc == 0, "help command returns success");
            t.check(text.find("SugarBot CLI") != std::string::npos,
                "help output includes CLI header");
            t.check(text.find("--obs-report") != std::string::npos,
                "help output includes obs-report surface");
            t.check(text.find("--plan-report") != std::string::npos,
                "help output includes plan-report surface");
            t.check(text.find("--feature-dump") != std::string::npos,
                "help output includes feature-dump surface");
            t.check(text.find("run <layout.json> <logic.json> <data.csv>") != std::string::npos,
                "help output includes unified run surface");
            t.check(text.find("--dump-layout") != std::string::npos,
                "help output includes dump-layout surface");
            t.check(text.find("--dump-plan") != std::string::npos,
                "help output includes dump-plan surface");
            t.check(text.find("--dump-logic") != std::string::npos,
                "help output includes dump-plan alias");
            t.check(text.find("raw `DataSpec` object") != std::string::npos,
                "help output explains raw dataspec layout shape");
            t.check(text.find("`data_spec`") != std::string::npos,
                "help output warns about the unsupported data_spec wrapper");
            t.check(text.find("--align-dump layout.json data.csv --series-key-col <col>") != std::string::npos,
                "help output promotes align-dump as a planning tool");
            t.check(text.find("groups the CSV by that column") != std::string::npos,
                "help output explains why align-dump requires --series-key-col");
            t.check(text.find("Reports and diagnostics:") != std::string::npos,
                "help output includes a reporting/discovery section");
            t.check(text.find("final execution report for the selected path") != std::string::npos,
                "help output explains that run emits the final report surface");
            t.check(text.find("interactive mode exposes these same report surfaces") != std::string::npos,
                "help output ties report discovery into interactive mode");
            return t.failed == 0;
        }

        bool test_dump_layout_dispatch_smoke(TestContext& t) {
            t.out << "[app_dispatch] dump-layout smoke\n";
            std::ostringstream out;
            const int rc = run_dump_layout_command(out);
            const std::string text = out.str();

            t.check(rc == 0, "dump-layout command returns success");
            t.check(text.find("SugarBot Layout Contract") != std::string::npos,
                "dump-layout output includes header");
            t.check(text.find("Raw `DataSpec` object") != std::string::npos,
                "dump-layout output explains the raw dataspec root shape");
            t.check(text.find("Wrapped `LayoutSpec` object with `ingest`") != std::string::npos,
                "dump-layout output explains the wrapped layout shape");
            t.check(text.find("`data_spec` is not a supported wrapper key") != std::string::npos,
                "dump-layout output rejects the unsupported data_spec wrapper");
            t.check(text.find("`YyyyMmDd`") != std::string::npos,
                "dump-layout output includes valid time kinds");
            t.check(text.find("EqualLengthRowDomain") != std::string::npos,
                "dump-layout output includes non-temporal alignment policy values");
            return t.failed == 0;
        }

        bool test_dump_plan_dispatch_smoke(TestContext& t) {
            t.out << "[app_dispatch] dump-plan smoke\n";
            std::ostringstream out;
            const int rc = run_dump_plan_command(out);
            const std::string text = out.str();

            t.check(rc == 0, "dump-plan command returns success");
            t.check(text.find("SugarBot Plan Contract") != std::string::npos,
                "dump-plan output includes header");
            t.check(text.find("raw `PlanSpec` root object") != std::string::npos,
                "dump-plan output explains raw planspec root shape");
            t.check(text.find("run.scope") != std::string::npos,
                "dump-plan output documents run dispatch");
            t.check(text.find("primary_series") != std::string::npos,
                "dump-plan output includes primary-series scope");
            t.check(text.find("per_entity_ranked") != std::string::npos,
                "dump-plan output includes ranked scope");
            t.check(text.find("run.objective") != std::string::npos,
                "dump-plan output documents canonical run objective shape");
            t.check(text.find("`--align-dump`") != std::string::npos,
                "dump-plan output points users to align-dump before cross-series authoring");
            t.check(text.find("values become the aligned series/entity keys") != std::string::npos,
                "dump-plan output explains --series-key-col semantics");
            t.check(text.find("`ts.sub`") != std::string::npos,
                "dump-plan output includes same-series subtraction guidance");
            return t.failed == 0;
        }

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
            t.check(text.find("Input diagnostics") != std::string::npos,
                "obs-report output includes input diagnostics");
            t.check(text.find("Observation capability") != std::string::npos,
                "obs-report output includes observation capability");
            t.check(text.find("Plan compatibility") == std::string::npos,
                "obs-report output does not include plan compatibility");
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
                    "    \"value_cols\": [\"close\", \"volume\", \"ma\"],\n"
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
                    "time,close,volume,ma\n"
                    "20240101,10,0,NaN\n"
                    "20240102,11,0,NA\n";
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

        bool test_feature_dump_dispatch_smoke(TestContext& t) {
            t.out << "[app_dispatch] feature-dump smoke\n";
            RunFixturePaths fixture{};
            std::string fixture_err;
            const bool fixture_ok = create_unified_feature_dump_fixture(fixture, fixture_err);
            t.check(fixture_ok, "feature-dump fixture files are created");
            if (!fixture_ok) return false;

            AppCommand cmd{};
            cmd.kind = CommandKind::FeatureDump;
            cmd.feature_dump.layout_path = fixture.layout_path;
            cmd.feature_dump.logic_path = fixture.logic_path;
            cmd.feature_dump.csv_path = fixture.csv_path;

            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_feature_dump_command(cmd, out, err);
            const std::string text = out.str();

            t.check(rc == 0, "feature-dump command returns success");
            t.check(err.str().empty(), "feature-dump command produces no error output");
            t.check(text.find("series_key,entity_key,row_index,time_key,c,sma2") == 0,
                "feature-dump output starts with the expected CSV header");
            t.check(text.find("__single__,__single__,0,20240101,10") != std::string::npos,
                "feature-dump output includes the first row");
            t.check(text.find("__single__,__single__,1,20240102,12,11") != std::string::npos,
                "feature-dump output includes computed feature values");
            return t.failed == 0;
        }

        bool test_unified_run_coarse_fine_displays_top_level_topk(TestContext& t) {
            t.out << "[app_dispatch] coarse-fine sweep top_k display\n";
            RunFixturePaths fixture{};
            std::string fixture_err;
            const bool fixture_ok = create_unified_single_series_fixture(fixture, fixture_err);
            t.check(fixture_ok, "coarse-fine top_k fixture files are created");
            if (!fixture_ok) return false;

            const auto fixture_dir = app_fixture_dir(fixture_err);
            t.check(!fixture_dir.empty(), "coarse-fine top_k fixture dir is available");
            if (fixture_dir.empty()) return false;

            const auto logic_path = fixture_dir / "app_tests_tmp_coarse_fine_topk_logic.json";
            {
                std::ofstream logic_out(logic_path, std::ios::binary | std::ios::trunc);
                t.check(static_cast<bool>(logic_out), "coarse-fine top_k logic file opens");
                if (!logic_out) return false;
                logic_out <<
                    "{\n"
                    "  \"version\": 1,\n"
                    "  \"name\": \"app_tests_coarse_fine_topk\",\n"
                    "  \"trade_mode\": \"LongOnly\",\n"
                    "  \"params\": { \"threshold\": 0.0 },\n"
                    "  \"numeric_nodes\": [\n"
                    "    { \"id\": \"c\", \"op\": \"core.close\" },\n"
                    "    { \"id\": \"thr\", \"op\": \"core.const\", \"params\": { \"value\": { \"ref\": \"threshold\" } } }\n"
                    "  ],\n"
                    "  \"bool_nodes\": [\n"
                    "    { \"id\": \"entry\", \"op\": \"cmp.gt\", \"args\": [\"c\", \"thr\"] },\n"
                    "    { \"id\": \"exit\", \"op\": \"cmp.lt\", \"args\": [\"c\", \"thr\"] }\n"
                    "  ],\n"
                    "  \"bindings\": {\n"
                    "    \"entry_long\": \"entry\",\n"
                    "    \"exit_long\": \"exit\",\n"
                    "    \"entry_short\": \"\",\n"
                    "    \"exit_short\": \"\",\n"
                    "    \"allow_trade\": \"\"\n"
                    "  },\n"
                    "  \"sweep\": {\n"
                    "    \"enabled\": true,\n"
                    "    \"engine\": \"coarse_fine\",\n"
                    "    \"top_k\": 2,\n"
                    "    \"max_evals\": 4,\n"
                    "    \"params\": {\n"
                    "      \"threshold\": { \"values\": [0, 5, 10, 12], \"is_int\": true }\n"
                    "    },\n"
                    "    \"engine_config\": {\n"
                    "      \"coarse_fine\": {\n"
                    "        \"k_stage1\": 4,\n"
                    "        \"k_stage2\": 4,\n"
                    "        \"depollute_stage1\": false,\n"
                    "        \"dedupe_stage2\": true,\n"
                    "        \"suppression_hops\": 1,\n"
                    "        \"stage2_hops\": 1\n"
                    "      }\n"
                    "    }\n"
                    "  }\n"
                    "}\n";
            }

            AppCommand cmd{};
            cmd.kind = CommandKind::Run;
            cmd.run.layout_path = fixture.layout_path;
            cmd.run.logic_path = logic_path.string();
            cmd.run.csv_path = fixture.csv_path;

            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_unified_cli_command(cmd, out, err);
            const std::string text = out.str();

            t.check(rc == 0, "coarse-fine run command returns success");
            t.check(err.str().empty(), "coarse-fine run command produces no error output");
            t.check(text.find("Engine: coarse_fine") != std::string::npos,
                "coarse-fine run output reports the selected engine");
            t.check(text.find("TopK: 2") != std::string::npos,
                "coarse-fine run output is capped by top-level sweep.top_k");
            t.check(text.find("TopK: 4") == std::string::npos,
                "coarse-fine run output does not expose k_stage2 as the final TopK count");
            t.check(text.find("Ranked candidate reports:") != std::string::npos,
                "coarse-fine run output advertises the candidate report section");
            t.check(text.find("Run report:") != std::string::npos,
                "coarse-fine run output prints candidate details through the run report seam");
            return t.failed == 0;
        }

        bool test_plan_sweep_report_preserves_fractional_param_ranges(TestContext& t) {
            t.out << "[app_reporters] fractional sweep range display\n";

            sugar::PlanSweepReport report{};
            report.requested_sweep.enabled = true;
            report.requested_sweep.engine = sugar::PlanSweepEngineKind::MonteCarlo;
            report.requested_sweep.objective = "pnl";
            report.requested_sweep.top_k = 1;
            report.requested_sweep.max_evals = 3;
            report.resolved_objective.primary.name = "pnl";
            report.parameter_count = 1;
            report.total_combinations = 3;
            report.candidate_space_size = 3;
            report.evaluations_run = 3;

            sugar::SweepParameterRangeSummary threshold{};
            threshold.name = "threshold";
            threshold.requested_range.values = { 0.9, 1.1, 1.3 };
            threshold.requested_range.is_int = false;
            threshold.expanded_count = 3;
            report.parameter_ranges.push_back(std::move(threshold));

            std::ostringstream out;
            sugar::print_plan_sweep_report(report, out);
            const std::string text = out.str();

            t.check(text.find("Engine: monte_carlo") != std::string::npos,
                "sweep report prints the selected engine");
            t.check(text.find("threshold: [0.9, 1.1, 1.3]") != std::string::npos,
                "sweep report preserves fractional requested values");
            t.check(text.find("threshold: [1, 1, 1]") == std::string::npos,
                "sweep report no longer rounds fractional requested values to integers");

            const std::string plan_json =
                "{\n"
                "  \"version\": 1,\n"
                "  \"name\": \"fractional_sweep_parse\",\n"
                "  \"numeric_nodes\": [],\n"
                "  \"bool_nodes\": [],\n"
                "  \"bindings\": {\n"
                "    \"entry_long\": \"\",\n"
                "    \"exit_long\": \"\",\n"
                "    \"entry_short\": \"\",\n"
                "    \"exit_short\": \"\",\n"
                "    \"allow_trade\": \"\"\n"
                "  },\n"
                "  \"sweep\": {\n"
                "    \"enabled\": true,\n"
                "    \"params\": {\n"
                "      \"threshold\": { \"values\": [0.9, 1.1, 1.3] },\n"
                "      \"window\": { \"values\": [8, 16, 24] },\n"
                "      \"forced_int\": { \"values\": [0.9, 1.1], \"is_int\": true }\n"
                "    }\n"
                "  }\n"
                "}\n";
            sugar::PlanSpec spec{};
            std::string err;
            const bool parsed = sugar::parse_plan_spec_json(plan_json, spec, err);
            t.check(parsed, "PlanSpec parser accepts fractional sweep fixture");
            t.check(parsed && spec.sweep.has_value() &&
                    !spec.sweep->params.at("threshold").is_int,
                "PlanSpec parser infers float sweep params from decimal values");
            t.check(parsed && spec.sweep.has_value() &&
                    spec.sweep->params.at("window").is_int,
                "PlanSpec parser keeps integer sweep params integer by default");
            t.check(parsed && spec.sweep.has_value() &&
                    spec.sweep->params.at("forced_int").is_int,
                "PlanSpec parser honors explicit integer sweep params");
            return t.failed == 0;
        }

        bool test_plan_sweep_report_derives_qol_warnings(TestContext& t) {
            t.out << "[app_reporters] sweep QoL warning derivation\n";

            sugar::PlanSweepReport report{};
            report.requested_sweep.enabled = true;
            report.requested_sweep.engine = sugar::PlanSweepEngineKind::CoarseFine;
            report.requested_sweep.objective = "pnl";
            report.requested_sweep.top_k = 2;
            report.requested_sweep.max_evals = 10;
            report.resolved_objective.primary.source = sugar::ObjectiveValueSource::ReportedMetric;
            report.resolved_objective.primary.name = "pnl";
            report.resolved_objective.direction = sugar::ObjectiveDirection::Maximize;
            report.parameter_count = 3;
            report.total_combinations = 10;
            report.candidate_space_size = 10;
            report.evaluations_run = 10;

            sugar::SweepParameterRangeSummary breakout{};
            breakout.name = "breakout_n";
            breakout.requested_range.min = 12;
            breakout.requested_range.max = 30;
            breakout.requested_range.step = 6;
            breakout.requested_range.is_int = true;
            breakout.expanded_count = 4;
            report.parameter_ranges.push_back(std::move(breakout));

            sugar::SweepParameterRangeSummary fast{};
            fast.name = "n_fast";
            fast.requested_range.min = 10;
            fast.requested_range.max = 30;
            fast.requested_range.step = 1;
            fast.requested_range.is_int = true;
            fast.expanded_count = 21;
            report.parameter_ranges.push_back(std::move(fast));

            sugar::SweepParameterRangeSummary slow{};
            slow.name = "n_slow";
            slow.requested_range.min = 20;
            slow.requested_range.max = 40;
            slow.requested_range.step = 1;
            slow.requested_range.is_int = true;
            slow.expanded_count = 21;
            report.parameter_ranges.push_back(std::move(slow));

            for (std::size_t i = 0; i < 2; ++i) {
                sugar::SweepRankedCandidate candidate{};
                candidate.candidate_index = i;
                candidate.score = 10.0 - static_cast<double>(i);
                candidate.result.pnl = candidate.score;
                candidate.parameter_values = {
                    sugar::SweepParameterSetting{ "breakout_n", static_cast<std::int64_t>(12) },
                    sugar::SweepParameterSetting{ "n_fast", static_cast<std::int64_t>(27) },
                    sugar::SweepParameterSetting{ "n_slow", static_cast<std::int64_t>(20) },
                };
                report.top_candidates.push_back(std::move(candidate));
            }

            report.warnings = sugar::derive_plan_sweep_report_warnings(report);

            std::ostringstream out;
            sugar::print_plan_sweep_report(report, out);
            const std::string text = out.str();

            t.check(report.warnings.size() >= 2,
                "sweep report derives boundary and semantic warning text");
            t.check(text.find("sweep boundary concentration") != std::string::npos &&
                    text.find("breakout_n=12") != std::string::npos,
                "sweep report annotates returned candidates concentrated at a sweep boundary");
            t.check(text.find("semantic fast/slow warning") != std::string::npos &&
                    text.find("n_fast > n_slow") != std::string::npos,
                "sweep report annotates likely fast/slow parameter inversions");
            return t.failed == 0;
        }

        bool test_param_string_literal_error_hints_ref_syntax(TestContext& t) {
            t.out << "[op_registry] param string literal ref-syntax hint\n";

            const auto registry = sugar::make_default_registry();
            const auto* op = registry.get("ts.sma");
            t.check(op != nullptr, "ts.sma registry entry is available");
            if (op == nullptr) return false;

            sugar::ParamMap node_params{};
            node_params["n"] = std::string("n_fast");
            sugar::ParamMap plan_params{};
            plan_params["n_fast"] = static_cast<std::int64_t>(5);
            sugar::ResolvedParamMap resolved{};
            std::string err;
            const bool ok = sugar::resolve_and_validate_params(
                *op,
                node_params,
                plan_params,
                nullptr,
                resolved,
                err);

            t.check(!ok, "string literal numeric param is rejected");
            t.check(err.find("Use {\"ref\":\"n_fast\"}") != std::string::npos,
                "wrong-type error points at missing param-ref syntax");
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

        bool test_align_dump_honors_dataspec_non_temporal_policy(TestContext& t) {
            t.out << "[app_dispatch] align-dump honors DataSpec non-temporal multi-series policy\n";
            std::string spec_path;
            std::string csv_path;
            std::string fixture_err;
            const bool fixture_ok = create_spatial_panel_fixture(spec_path, csv_path, fixture_err, true);
            t.check(fixture_ok, "policy-enabled spatial-panel align-dump fixture files are created");
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

            t.check(rc == 0, "policy-enabled spatial-panel align-dump returns success");
            t.check(err.str().empty(), "policy-enabled spatial-panel align-dump produces no error output");
            t.check(text.find("Observation capability") != std::string::npos,
                "policy-enabled spatial-panel align-dump still reports observation capability");
            t.check(text.find("series_count=2") != std::string::npos,
                "policy-enabled spatial-panel align-dump reports aligned series count");
            t.check(text.find("series[0]=A") != std::string::npos || text.find("series[1]=A") != std::string::npos,
                "policy-enabled spatial-panel align-dump reports stable aligned series identities");
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

        bool test_plan_capability_report_hints_same_series_alternative_for_ms_diff(TestContext& t) {
            t.out << "[app_reporters] cross-series hint for ms.diff on single-series data\n";
            RunFixturePaths fixture{};
            std::string fixture_err;
            const bool fixture_ok = create_unified_single_series_fixture(fixture, fixture_err);
            t.check(fixture_ok, "single-series ms.diff hint fixture files are created");
            if (!fixture_ok) return false;

            sugar::LayoutSpec layout{};
            std::string load_err;
            const bool layout_ok = sugar::load_layout_spec_file(fixture.layout_path, layout, load_err);
            t.check(layout_ok, "single-series ms.diff hint layout loads");
            if (!layout_ok) return false;

            sugar::ResolvedObservationWorld world{};
            const bool world_ok = sugar::build_resolved_observation_world(layout, fixture.csv_path, world, load_err);
            t.check(world_ok, "single-series ms.diff hint observation world builds");
            if (!world_ok) return false;

            sugar::ResolvedRuntimeInput runtime{};
            const bool runtime_ok = sugar::bind_resolved_runtime_target(
                world,
                layout.ingest.primary_value_col,
                sugar::RuntimeTargetSpec{},
                runtime,
                load_err);
            t.check(runtime_ok, "single-series ms.diff hint runtime binds");
            if (!runtime_ok) return false;

            sugar::PlanSpec spec{};
            spec.numeric_nodes = {
                sugar::NodeSpec{ "c", "core.close", {}, {} },
                sugar::NodeSpec{ "spread", "ms.diff", { "c", "c" }, {} },
            };
            spec.bind = sugar::PlanBindings{};

            std::ostringstream out;
            sugar::app::print_plan_capability_report(spec, runtime, out);
            const std::string text = out.str();

            t.check(text.find("requires multiple aligned series") != std::string::npos,
                "plan capability report explains the cross-series requirement");
            t.check(text.find("--align-dump") != std::string::npos,
                "plan capability report points users to align-dump for series_count discovery");
            t.check(text.find("use ts.sub instead") != std::string::npos,
                "plan capability report suggests ts.sub for same-series subtraction");
            return t.failed == 0;
        }

        bool test_plan_capability_report_hints_bool_op_node_list(TestContext& t) {
            t.out << "[app_reporters] bool op in numeric_nodes hint\n";
            RunFixturePaths fixture{};
            std::string fixture_err;
            const bool fixture_ok = create_unified_single_series_fixture(fixture, fixture_err);
            t.check(fixture_ok, "bool-op node-list hint fixture files are created");
            if (!fixture_ok) return false;

            sugar::LayoutSpec layout{};
            std::string load_err;
            const bool layout_ok = sugar::load_layout_spec_file(fixture.layout_path, layout, load_err);
            t.check(layout_ok, "bool-op node-list hint layout loads");
            if (!layout_ok) return false;

            sugar::ResolvedObservationWorld world{};
            const bool world_ok = sugar::build_resolved_observation_world(layout, fixture.csv_path, world, load_err);
            t.check(world_ok, "bool-op node-list hint observation world builds");
            if (!world_ok) return false;

            sugar::ResolvedRuntimeInput runtime{};
            const bool runtime_ok = sugar::bind_resolved_runtime_target(
                world,
                layout.ingest.primary_value_col,
                sugar::RuntimeTargetSpec{},
                runtime,
                load_err);
            t.check(runtime_ok, "bool-op node-list hint runtime binds");
            if (!runtime_ok) return false;

            sugar::PlanSpec spec{};
            spec.numeric_nodes = {
                sugar::NodeSpec{ "c", "core.close", {}, {} },
                sugar::NodeSpec{ "z", "core.const", {}, { {"value", 0.0} } },
                sugar::NodeSpec{ "bad", "cmp.gt", { "c", "z" }, {} },
            };
            spec.bind = sugar::PlanBindings{};

            std::ostringstream out;
            sugar::app::print_plan_capability_report(spec, runtime, out);
            const std::string text = out.str();

            t.check(text.find("operator type mismatch") != std::string::npos,
                "plan capability report still classifies the node-list type mismatch");
            t.check(text.find("move this bool operator to `bool_nodes`") != std::string::npos,
                "plan capability report tells users which node list should contain the bool op");
            t.check(text.find("core.select") != std::string::npos,
                "plan capability report points core.select users at the bool-consuming numeric path");
            return t.failed == 0;
        }

        bool test_align_dump_accepts_wrapped_layout_spec(TestContext& t) {
            t.out << "[app_dispatch] align-dump accepts wrapped LayoutSpec\n";
            RunFixturePaths fixture{};
            std::string fixture_err;
            const bool fixture_ok = create_unified_panel_ranked_fixture(fixture, fixture_err);
            t.check(fixture_ok, "wrapped LayoutSpec align-dump fixture files are created");
            if (!fixture_ok) return false;

            AppCommand cmd{};
            cmd.kind = CommandKind::AlignDump;
            cmd.align_dump.spec_path = fixture.layout_path;
            cmd.align_dump.csv_path = fixture.csv_path;
            cmd.align_dump.series_key_col = "symbol";
            cmd.align_dump.calendar = sugar::align::CalendarPolicy::Intersection;

            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_align_dump_command(cmd, out, err);
            const std::string text = out.str();

            t.check(rc == 0, "wrapped LayoutSpec align-dump returns success");
            t.check(err.str().empty(), "wrapped LayoutSpec align-dump produces no error output");
            t.check(text.find("Alignment dump") != std::string::npos,
                "wrapped LayoutSpec align-dump reaches the aligned output surface");
            t.check(text.find("series_count=2") != std::string::npos,
                "wrapped LayoutSpec align-dump reports the aligned series count");
            t.check(text.find("builtin.temporal.window") != std::string::npos,
                "wrapped LayoutSpec align-dump preserves topology guidance");
            return t.failed == 0;
        }

        bool test_plan_capability_report_hints_bool_node_numeric_bridge(TestContext& t) {
            t.out << "[app_reporters] bool node used in numeric arg hint\n";
            RunFixturePaths fixture{};
            std::string fixture_err;
            const bool fixture_ok = create_unified_single_series_fixture(fixture, fixture_err);
            t.check(fixture_ok, "bool-node numeric-bridge fixture files are created");
            if (!fixture_ok) return false;

            sugar::LayoutSpec layout{};
            std::string load_err;
            const bool layout_ok = sugar::load_layout_spec_file(fixture.layout_path, layout, load_err);
            t.check(layout_ok, "bool-node numeric-bridge layout loads");
            if (!layout_ok) return false;

            sugar::ResolvedObservationWorld world{};
            const bool world_ok = sugar::build_resolved_observation_world(layout, fixture.csv_path, world, load_err);
            t.check(world_ok, "bool-node numeric-bridge observation world builds");
            if (!world_ok) return false;

            sugar::ResolvedRuntimeInput runtime{};
            const bool runtime_ok = sugar::bind_resolved_runtime_target(
                world,
                layout.ingest.primary_value_col,
                sugar::RuntimeTargetSpec{},
                runtime,
                load_err);
            t.check(runtime_ok, "bool-node numeric-bridge runtime binds");
            if (!runtime_ok) return false;

            sugar::PlanSpec spec{};
            spec.numeric_nodes = {
                sugar::NodeSpec{ "c", "core.close", {}, {} },
                sugar::NodeSpec{ "z", "core.const", {}, { {"value", 0.0} } },
                sugar::NodeSpec{ "bad", "ts.sma", { "flag" }, { {"n", 2.0} } },
            };
            spec.bool_nodes = {
                sugar::NodeSpec{ "flag", "cmp.gt", { "c", "z" }, {} },
            };
            spec.bind = sugar::PlanBindings{};

            std::ostringstream out;
            sugar::app::print_plan_capability_report(spec, runtime, out);
            const std::string text = out.str();

            t.check(text.find("flag is a bool node; to use bool output in a numeric context, use `core.select`") != std::string::npos,
                "plan capability report explains how to bridge bool nodes into numeric context");
            t.check(text.find("unknown numeric node: flag") == std::string::npos,
                "plan capability report avoids the misleading unknown-numeric-node wording for bool references");
            return t.failed == 0;
        }

        bool test_runtime_graph_preview_restores_stream_formatting_for_run_reports(TestContext& t) {
            t.out << "[app_reporters] runtime preview restores stream formatting\n";
            RunFixturePaths fixture{};
            std::string fixture_err;
            const bool fixture_ok = create_unified_single_series_fixture(fixture, fixture_err);
            t.check(fixture_ok, "runtime-preview formatting fixture files are created");
            if (!fixture_ok) return false;

            sugar::LayoutSpec layout{};
            std::string load_err;
            const bool layout_ok = sugar::load_layout_spec_file(fixture.layout_path, layout, load_err);
            t.check(layout_ok, "runtime-preview formatting layout loads");
            if (!layout_ok) return false;

            sugar::ResolvedObservationWorld world{};
            const bool world_ok = sugar::build_resolved_observation_world(layout, fixture.csv_path, world, load_err);
            t.check(world_ok, "runtime-preview formatting observation world builds");
            if (!world_ok) return false;

            sugar::ResolvedRuntimeInput runtime{};
            const bool runtime_ok = sugar::bind_resolved_runtime_target(
                world,
                layout.ingest.primary_value_col,
                sugar::RuntimeTargetSpec{},
                runtime,
                load_err);
            t.check(runtime_ok, "runtime-preview formatting runtime binds");
            if (!runtime_ok) return false;

            sugar::BacktestResult result{};
            result.pnl = 12.5;
            result.trades = 20;
            result.max_drawdown = 3.25;
            result.best_start_date = 20211123;
            result.has_best_start_date = true;
            result.trade_hash = 13693342942469747168ull;

            const sugar::DataView dv(*runtime.aligned_bundle);
            std::ostringstream out;
            sugar::print_runtime_graph_preview(
                dv,
                runtime.primary_ref,
                runtime.run_manifest,
                "formatting-test",
                out,
                8,
                nullptr);
            sugar::print_backtest_summary(result, out);
            const std::string text = out.str();

            t.check(text.find("Trades: 20\n") != std::string::npos,
                "run summary keeps integer trade counts after runtime preview output");
            t.check(text.find("trades [core_metric]: 20\n") != std::string::npos,
                "metric catalog keeps integer trade counts after runtime preview output");
            t.check(text.find("first_usable_signal_time_key [core_metric]: 20211123\n") != std::string::npos,
                "metric catalog prints time-key metrics without scientific notation");
            t.check(text.find("trade_hash [core_metric]: 13693342942469747168\n") != std::string::npos,
                "metric catalog preserves exact trade_hash integers instead of scientific notation");
            t.check(text.find("20.0000") == std::string::npos,
                "runtime preview formatting no longer leaks fixed precision into later run reports");
            return t.failed == 0;
        }

        bool test_unified_run_suppresses_internal_cross_debug_warning_by_default(TestContext& t) {
            t.out << "[app_dispatch] internal cross debug warning stays off by default\n";

            std::string fixture_err;
            const auto fixture_dir = app_fixture_dir(fixture_err);
            t.check(!fixture_dir.empty(), "cross-warning fixture dir is available");
            if (fixture_dir.empty()) return false;

            const auto layout_path = fixture_dir / "app_tests_tmp_cross_warning_layout.json";
            const auto logic_path = fixture_dir / "app_tests_tmp_cross_warning_logic.json";
            const auto csv_path = fixture_dir / "app_tests_tmp_cross_warning.csv";

            {
                std::ofstream layout_out(layout_path, std::ios::binary | std::ios::trunc);
                t.check(static_cast<bool>(layout_out), "cross-warning layout file opens");
                if (!layout_out) return false;
                layout_out <<
                    "{\n"
                    "  \"ingest\": {\n"
                    "    \"time_col\": \"row_id\",\n"
                    "    \"time_kind\": \"Index\",\n"
                    "    \"header_policy\": \"RequiredHeaders\",\n"
                    "    \"value_cols\": [\"close\"],\n"
                    "    \"primary_value_col\": \"close\",\n"
                    "    \"missing_tokens\": [\"\", \"NA\", \"null\", \"nan\", \"NaN\"],\n"
                    "    \"missing_default\": { \"kind\": \"NaN\" },\n"
                    "    \"sort_by_time\": true,\n"
                    "    \"dedupe\": \"Error\"\n"
                    "  }\n"
                    "}\n";
            }

            {
                std::ofstream logic_out(logic_path, std::ios::binary | std::ios::trunc);
                t.check(static_cast<bool>(logic_out), "cross-warning logic file opens");
                if (!logic_out) return false;
                logic_out <<
                    "{\n"
                    "  \"version\": 1,\n"
                    "  \"name\": \"app_tests_cross_warning_default_off\",\n"
                    "  \"trade_mode\": \"LongOnly\",\n"
                    "  \"numeric_nodes\": [\n"
                    "    { \"id\": \"c\", \"op\": \"core.close\" },\n"
                    "    { \"id\": \"hi\", \"op\": \"ts.roll_max\", \"args\": [\"c\"], \"params\": { \"n\": 5 } },\n"
                    "    { \"id\": \"z\", \"op\": \"core.const\", \"params\": { \"value\": 0.0 } }\n"
                    "  ],\n"
                    "  \"bool_nodes\": [\n"
                    "    { \"id\": \"entry\", \"op\": \"cmp.cross_up\", \"args\": [\"c\", \"hi\"] },\n"
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

            {
                std::ofstream csv_out(csv_path, std::ios::binary | std::ios::trunc);
                t.check(static_cast<bool>(csv_out), "cross-warning csv file opens");
                if (!csv_out) return false;
                csv_out << "row_id,close\n";
                for (int i = 0; i < 64; ++i) {
                    csv_out << (i + 1) << "," << (100 + i) << "\n";
                }
            }

            AppCommand cmd{};
            cmd.kind = CommandKind::Run;
            cmd.run.layout_path = layout_path.string();
            cmd.run.logic_path = logic_path.string();
            cmd.run.csv_path = csv_path.string();

            std::ostringstream out;
            std::ostringstream err;
            std::ostringstream captured_cerr;
            auto* const old_cerr = std::cerr.rdbuf(captured_cerr.rdbuf());
            const int rc = run_unified_cli_command(cmd, out, err);
            std::cerr.rdbuf(old_cerr);

            t.check(rc == 0, "cross-warning probe run succeeds");
            t.check(captured_cerr.str().find("[SugarBot][debug]") == std::string::npos,
                "internal cross debug warning does not leak to stderr by default");
            t.check(out.str().find("[SugarBot][debug]") == std::string::npos,
                "internal cross debug warning does not leak to stdout by default");
            t.check(err.str().find("[SugarBot][debug]") == std::string::npos,
                "internal cross debug warning does not leak through command error output by default");
            return t.failed == 0;
        }

        bool test_layout_parser_rejects_data_spec_wrapper_with_explicit_hint(TestContext& t) {
            t.out << "[layout_spec] unsupported data_spec wrapper hint\n";
            const std::string json =
                "{\n"
                "  \"data_spec\": {\n"
                "    \"time_col\": \"date\",\n"
                "    \"time_kind\": \"YyyyMmDd\",\n"
                "    \"value_cols\": [\"close\"],\n"
                "    \"primary_value_col\": \"close\"\n"
                "  }\n"
                "}\n";

            sugar::LayoutSpec layout{};
            std::string err;
            const bool ok = sugar::parse_layout_spec_json(json, layout, err);
            t.check(!ok, "layout parser rejects unsupported data_spec wrapper");
            t.check(err.find("raw DataSpec object") != std::string::npos,
                "layout parser error points users at raw dataspec root shape");
            t.check(err.find("`ingest`") != std::string::npos,
                "layout parser error points users at the supported ingest wrapper");
            return t.failed == 0;
        }

    } // namespace

    void register_command_tests(AppTestRegistry& tests) {
        tests.push_back(test_help_dispatch_smoke);
        tests.push_back(test_dump_layout_dispatch_smoke);
        tests.push_back(test_dump_plan_dispatch_smoke);
        tests.push_back(test_obs_report_dispatch_smoke);
        tests.push_back(test_obs_report_exposes_drop_row_diagnostics);
        tests.push_back(test_obs_report_warns_on_degenerate_value_columns);
        tests.push_back(test_obs_report_time_parse_failure_includes_time_kind_hint);
        tests.push_back(test_plan_report_dispatch_smoke);
        tests.push_back(test_plan_report_reports_multi_entity_missing_scope);
        tests.push_back(test_feature_dump_dispatch_smoke);
        tests.push_back(test_unified_run_coarse_fine_displays_top_level_topk);
        tests.push_back(test_plan_sweep_report_preserves_fractional_param_ranges);
        tests.push_back(test_plan_sweep_report_derives_qol_warnings);
        tests.push_back(test_param_string_literal_error_hints_ref_syntax);
        tests.push_back(test_dump_ops_dispatch_smoke);
        tests.push_back(test_dump_ops_module_catalog_example_sections);
        tests.push_back(test_prep_dump_dispatch_smoke);
        tests.push_back(test_prep_dump_spatial_output_is_row_domain_honest);
        tests.push_back(test_prep_dump_reports_observation_capabilities);
        tests.push_back(test_prep_dump_reports_panel_capability_incompatibility);
        tests.push_back(test_runtime_graph_preview_spatial_output_is_row_domain_honest);
        tests.push_back(test_align_dump_reports_observation_capabilities);
        tests.push_back(test_align_dump_accepts_wrapped_layout_spec);
        tests.push_back(test_align_dump_rejects_non_temporal_multiseries_alignment);
        tests.push_back(test_align_dump_honors_dataspec_non_temporal_policy);
        tests.push_back(test_plan_capability_report_surfaces_topology_conditions);
        tests.push_back(test_plan_capability_report_surfaces_locality_subject_provenance);
        tests.push_back(test_plan_capability_report_hints_same_series_alternative_for_ms_diff);
        tests.push_back(test_plan_capability_report_hints_bool_op_node_list);
        tests.push_back(test_plan_capability_report_hints_bool_node_numeric_bridge);
        tests.push_back(test_runtime_graph_preview_restores_stream_formatting_for_run_reports);
        tests.push_back(test_unified_run_suppresses_internal_cross_debug_warning_by_default);
        tests.push_back(test_layout_parser_rejects_data_spec_wrapper_with_explicit_hint);
    }

} // namespace sugar::app::tests
