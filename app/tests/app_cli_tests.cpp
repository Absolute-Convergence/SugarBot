#include "tests/app_test_support.h"

#include <string>

#include "cli/app_cli.h"

namespace sugar::app::tests {

    namespace {

        bool test_parse_help_flag(TestContext& t) {
            t.out << "[app_cli] help parsing\n";
            char arg0[] = "prog";
            char arg1[] = "--help";
            char* argv[] = { arg0, arg1 };

            AppCommand cmd{};
            std::string err;
            const bool ok = parse_app_command(2, argv, cmd, err);
            t.check(ok, "parse_app_command accepts --help");
            t.check(err.empty(), "help parse has no error");
            t.check(cmd.kind == CommandKind::Help, "--help resolves to Help command");
            return t.failed == 0;
        }

        bool test_parse_onramp_flag(TestContext& t) {
            t.out << "[app_cli] onramp parsing\n";
            char arg0[] = "prog";
            char arg1[] = "--onramp";
            char* argv[] = { arg0, arg1 };

            AppCommand cmd{};
            std::string err;
            const bool ok = parse_app_command(2, argv, cmd, err);
            t.check(ok, "parse_app_command accepts --onramp");
            t.check(err.empty(), "onramp parse has no error");
            t.check(cmd.kind == CommandKind::Onramp, "--onramp resolves to Onramp command");

            char arg1b[] = "onramp";
            char* argv_bare[] = { arg0, arg1b };
            AppCommand bare_cmd{};
            err.clear();
            const bool bare_ok = parse_app_command(2, argv_bare, bare_cmd, err);
            t.check(bare_ok, "parse_app_command accepts bare onramp command");
            t.check(bare_cmd.kind == CommandKind::Onramp, "bare onramp resolves to Onramp command");
            return t.failed == 0;
        }

        bool test_parse_split_help_flags(TestContext& t) {
            t.out << "[app_cli] split help parsing\n";
            char arg0[] = "prog";
            char diagnostics_flag[] = "--help-diagnostics";
            char* diagnostics_argv[] = { arg0, diagnostics_flag };

            AppCommand diagnostics_cmd{};
            std::string err;
            const bool diagnostics_ok = parse_app_command(2, diagnostics_argv, diagnostics_cmd, err);
            t.check(diagnostics_ok, "parse_app_command accepts --help-diagnostics");
            t.check(err.empty(), "help-diagnostics parse has no error");
            t.check(diagnostics_cmd.kind == CommandKind::HelpDiagnostics,
                "--help-diagnostics resolves to HelpDiagnostics command");

            char reports_flag[] = "--help-reports";
            char* reports_argv[] = { arg0, reports_flag };
            AppCommand reports_cmd{};
            err.clear();
            const bool reports_ok = parse_app_command(2, reports_argv, reports_cmd, err);
            t.check(reports_ok, "parse_app_command accepts --help-reports");
            t.check(err.empty(), "help-reports parse has no error");
            t.check(reports_cmd.kind == CommandKind::HelpReports,
                "--help-reports resolves to HelpReports command");

            char help_arg[] = "help";
            char reports_arg[] = "reports";
            char* help_reports_argv[] = { arg0, help_arg, reports_arg };
            AppCommand help_reports_cmd{};
            err.clear();
            const bool help_reports_ok =
                parse_app_command(3, help_reports_argv, help_reports_cmd, err);
            t.check(help_reports_ok, "parse_app_command accepts `help reports`");
            t.check(help_reports_cmd.kind == CommandKind::HelpReports,
                "`help reports` resolves to HelpReports command");

            char diagnostics_arg[] = "diagnostics";
            char* help_diagnostics_argv[] = { arg0, help_arg, diagnostics_arg };
            AppCommand help_diagnostics_cmd{};
            err.clear();
            const bool help_diagnostics_ok =
                parse_app_command(3, help_diagnostics_argv, help_diagnostics_cmd, err);
            t.check(help_diagnostics_ok, "parse_app_command accepts `help diagnostics`");
            t.check(help_diagnostics_cmd.kind == CommandKind::HelpDiagnostics,
                "`help diagnostics` resolves to HelpDiagnostics command");
            return t.failed == 0;
        }

        bool test_parse_obs_report_command(TestContext& t) {
            t.out << "[app_cli] obs-report parsing\n";
            char arg0[] = "prog";
            char arg1[] = "--obs-report";
            char arg2[] = "layout.json";
            char arg3[] = "data.csv";
            char* argv[] = { arg0, arg1, arg2, arg3 };

            AppCommand cmd{};
            std::string err;
            const bool ok = parse_app_command(4, argv, cmd, err);
            t.check(ok, "obs-report command parses");
            t.check(err.empty(), "obs-report parse has no error");
            t.check(cmd.kind == CommandKind::ObsReport, "obs-report selects ObsReport command kind");
            t.check(cmd.obs_report.layout_path.find("layout.json") != std::string::npos,
                "obs-report parse records layout path");
            t.check(cmd.obs_report.csv_path.find("data.csv") != std::string::npos,
                "obs-report parse records csv path");

            char json_arg[] = "--json";
            char* json_argv[] = { arg0, arg1, arg2, arg3, json_arg };
            AppCommand json_cmd{};
            err.clear();
            const bool json_ok = parse_app_command(5, json_argv, json_cmd, err);
            t.check(json_ok, "obs-report accepts --json projection");
            t.check(err.empty(), "obs-report --json parse has no error");
            t.check(json_cmd.obs_report.json_output,
                "obs-report --json records machine-readable output mode");

            char report_arg[] = "--report";
            char report_value[] = "json";
            char* report_argv[] = { arg0, arg1, arg2, arg3, report_arg, report_value };
            AppCommand report_cmd{};
            err.clear();
            const bool report_ok = parse_app_command(6, report_argv, report_cmd, err);
            t.check(report_ok, "obs-report accepts --report json projection");
            t.check(report_cmd.obs_report.json_output,
                "obs-report --report json records machine-readable output mode");
            return t.failed == 0;
        }

        bool test_parse_plan_report_command(TestContext& t) {
            t.out << "[app_cli] plan-report parsing\n";
            char arg0[] = "prog";
            char arg1[] = "--plan-report";
            char arg2[] = "layout.json";
            char arg3[] = "logic.json";
            char arg4[] = "data.csv";
            char* argv[] = { arg0, arg1, arg2, arg3, arg4 };

            AppCommand cmd{};
            std::string err;
            const bool ok = parse_app_command(5, argv, cmd, err);
            t.check(ok, "plan-report command parses");
            t.check(err.empty(), "plan-report parse has no error");
            t.check(cmd.kind == CommandKind::PlanReport, "plan-report selects PlanReport command kind");
            t.check(cmd.plan_report.layout_path.find("layout.json") != std::string::npos,
                "plan-report parse records layout path");
            t.check(cmd.plan_report.logic_path.find("logic.json") != std::string::npos,
                "plan-report parse records logic path");
            t.check(cmd.plan_report.csv_path.find("data.csv") != std::string::npos,
                "plan-report parse records csv path");
            return t.failed == 0;
        }

        bool test_parse_feature_dump_command(TestContext& t) {
            t.out << "[app_cli] feature-dump parsing\n";
            char arg0[] = "prog";
            char arg1[] = "--feature-dump";
            char arg2[] = "layout.json";
            char arg3[] = "logic.json";
            char arg4[] = "data.csv";
            char* argv[] = { arg0, arg1, arg2, arg3, arg4 };

            AppCommand cmd{};
            std::string err;
            const bool ok = parse_app_command(5, argv, cmd, err);
            t.check(ok, "feature-dump command parses");
            t.check(err.empty(), "feature-dump parse has no error");
            t.check(cmd.kind == CommandKind::FeatureDump, "feature-dump selects FeatureDump command kind");
            t.check(cmd.feature_dump.layout_path.find("layout.json") != std::string::npos,
                "feature-dump parse records layout path");
            t.check(cmd.feature_dump.logic_path.find("logic.json") != std::string::npos,
                "feature-dump parse records logic path");
            t.check(cmd.feature_dump.csv_path.find("data.csv") != std::string::npos,
                "feature-dump parse records csv path");
            return t.failed == 0;
        }

        bool test_parse_tabular_format_flags(TestContext& t) {
            t.out << "[app_cli] tabular format flag parsing\n";

            char arg0[] = "prog";
            char run_arg[] = "run";
            char layout_arg[] = "layout.json";
            char logic_arg[] = "logic.json";
            char parquet_data_arg[] = "data.parquet";
            char input_format_arg[] = "--input-format";
            char parquet_value[] = "parquet";
            char* run_argv[] = {
                arg0,
                run_arg,
                layout_arg,
                logic_arg,
                parquet_data_arg,
                input_format_arg,
                parquet_value,
            };

            AppCommand run_cmd{};
            std::string err;
            const bool run_ok = parse_app_command(7, run_argv, run_cmd, err);
            t.check(run_ok, "run accepts --input-format parquet");
            t.check(err.empty(), "run input-format parse has no error");
            t.check(run_cmd.run.input_format == sugar::TabularFileFormat::Parquet,
                "run records parquet input format");

            char feature_arg[] = "--feature-dump";
            char output_format_arg[] = "--output-format";
            char output_arg[] = "--output";
            char output_path_arg[] = "features.parquet";
            char* feature_argv[] = {
                arg0,
                feature_arg,
                layout_arg,
                logic_arg,
                parquet_data_arg,
                input_format_arg,
                parquet_value,
                output_format_arg,
                parquet_value,
                output_arg,
                output_path_arg,
            };

            AppCommand feature_cmd{};
            err.clear();
            const bool feature_ok = parse_app_command(11, feature_argv, feature_cmd, err);
            t.check(feature_ok, "feature-dump accepts parquet input and output flags");
            t.check(err.empty(), "feature-dump parquet flag parse has no error");
            t.check(feature_cmd.feature_dump.input_format == sugar::TabularFileFormat::Parquet,
                "feature-dump records parquet input format");
            t.check(feature_cmd.feature_dump.output_format == sugar::TabularFileFormat::Parquet,
                "feature-dump records parquet output format");
            t.check(feature_cmd.feature_dump.output_path.find("features.parquet") != std::string::npos,
                "feature-dump records output path");

            char bad_value[] = "json";
            char* bad_argv[] = {
                arg0,
                feature_arg,
                layout_arg,
                logic_arg,
                parquet_data_arg,
                output_format_arg,
                bad_value,
            };
            AppCommand bad_cmd{};
            err.clear();
            const bool bad_ok = parse_app_command(7, bad_argv, bad_cmd, err);
            t.check(!bad_ok, "feature-dump rejects invalid output format");
            t.check(err == "--output-format must be one of auto | csv | parquet",
                "invalid output-format error is explicit");

            return t.failed == 0;
        }

        bool test_parquet_loader_requires_enabled_build(TestContext& t) {
            t.out << "[app_cli] parquet disabled-build diagnostic\n";
#if !defined(SUGAR_ENABLE_PARQUET)
            bool had_header = false;
            std::string err;
            const auto table = sugar::load_tabular_table(
                "missing_fixture.parquet",
                sugar::TabularFileFormat::Parquet,
                sugar::CsvHeaderPolicy::Required,
                had_header,
                err);
            t.check(table.headers.empty() && table.rows.empty(),
                "disabled Parquet load returns an empty table");
            t.check(!had_header, "disabled Parquet load reports no header");
            t.check(err.find("SUGAR_ENABLE_PARQUET=ON") != std::string::npos,
                "disabled Parquet load explains the required build flag");
#else
            t.check(true, "Parquet-enabled build skips disabled-build diagnostic");
#endif
            return t.failed == 0;
        }

        bool test_parse_dump_layout_command(TestContext& t) {
            t.out << "[app_cli] dump-layout parsing\n";
            char arg0[] = "prog";
            char arg1[] = "--dump-layout";
            char* argv[] = { arg0, arg1 };

            AppCommand cmd{};
            std::string err;
            const bool ok = parse_app_command(2, argv, cmd, err);
            t.check(ok, "dump-layout command parses");
            t.check(err.empty(), "dump-layout parse has no error");
            t.check(cmd.kind == CommandKind::DumpLayout, "dump-layout resolves to DumpLayout command");
            return t.failed == 0;
        }

        bool test_parse_dump_plan_aliases(TestContext& t) {
            t.out << "[app_cli] dump-plan alias parsing\n";
            char arg0[] = "prog";
            char arg1[] = "--dump-logic";
            char* argv[] = { arg0, arg1 };

            AppCommand cmd{};
            std::string err;
            const bool ok = parse_app_command(2, argv, cmd, err);
            t.check(ok, "parse_app_command accepts --dump-logic alias");
            t.check(err.empty(), "dump-plan alias parse has no error");
            t.check(cmd.kind == CommandKind::DumpPlan, "dump-logic alias resolves to DumpPlan command");
            return t.failed == 0;
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
            t.check(cmd.dump_ops.section == DumpOpsSection::Full,
                "dump-ops alias defaults to full section");
            return t.failed == 0;
        }

        bool test_parse_dump_ops_section(TestContext& t) {
            t.out << "[app_cli] dump-ops section parsing\n";
            char arg0[] = "prog";
            char arg1[] = "--dump-ops";
            char arg2[] = "ops";
            char* argv[] = { arg0, arg1, arg2 };

            AppCommand cmd{};
            std::string err;
            const bool ok = parse_app_command(3, argv, cmd, err);
            t.check(ok, "parse_app_command accepts --dump-ops ops");
            t.check(err.empty(), "dump-ops section parse has no error");
            t.check(cmd.kind == CommandKind::DumpOps, "dump-ops section resolves to DumpOps command");
            t.check(cmd.dump_ops.section == DumpOpsSection::Operations,
                "dump-ops ops resolves to operations section");
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
            t.check(err.find("--series-key-col <col> is required for --align-dump") != std::string::npos &&
                    err.find("identifies each series/entity") != std::string::npos,
                "align-dump failure message is explicit");
            return t.failed == 0;
        }

        bool test_parse_scaffold_command(TestContext& t) {
            t.out << "[app_cli] scaffold parsing\n";
            char arg0[] = "prog";
            char arg1[] = "--scaffold";
            char arg2[] = "data.csv";
            char* argv[] = { arg0, arg1, arg2 };

            AppCommand cmd{};
            std::string err;
            const bool ok = parse_app_command(3, argv, cmd, err);
            t.check(ok, "parse_app_command accepts --scaffold");
            t.check(err.empty(), "scaffold parse has no error");
            t.check(cmd.kind == CommandKind::ScaffoldLayout,
                "--scaffold resolves to ScaffoldLayout command");
            t.check(cmd.scaffold_layout.csv_path.find("data.csv") != std::string::npos,
                "scaffold parse records csv path");

            char bare_arg1[] = "scaffold";
            char* bare_argv[] = { arg0, bare_arg1, arg2 };
            AppCommand bare_cmd{};
            err.clear();
            const bool bare_ok = parse_app_command(3, bare_argv, bare_cmd, err);
            t.check(bare_ok, "parse_app_command accepts bare scaffold command");
            t.check(bare_cmd.kind == CommandKind::ScaffoldLayout,
                "bare scaffold resolves to ScaffoldLayout command");
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

        bool test_parse_rejects_unknown_interactive_flag(TestContext& t) {
            t.out << "[app_cli] unknown interactive flag rejection\n";
            char arg0[] = "prog";
            char arg1[] = "--xyz-unknown";
            char* argv[] = { arg0, arg1 };

            AppCommand cmd{};
            std::string err;
            const bool ok = parse_app_command(2, argv, cmd, err);
            t.check(!ok, "parse_app_command rejects unknown interactive flags");
            t.check(err == "unknown argument '--xyz-unknown'",
                "unknown interactive flag error is explicit");
            return t.failed == 0;
        }

        bool test_parse_interactive_format_requires_value(TestContext& t) {
            t.out << "[app_cli] interactive format requires value\n";
            char arg0[] = "prog";
            char arg1[] = "--format";
            char* argv[] = { arg0, arg1 };

            AppCommand cmd{};
            std::string err;
            const bool ok = parse_app_command(2, argv, cmd, err);
            t.check(!ok, "parse_app_command rejects --format without a value");
            t.check(err == "--format requires <auto|legacy_ohlcv_date|iso_ohlcv_time>",
                "--format missing-value error is explicit");
            return t.failed == 0;
        }

        bool test_parse_unified_run_command(TestContext& t) {
            t.out << "[app_cli] unified run parsing\n";
            char arg0[] = "prog";
            char arg1[] = "run";
            char arg2[] = "layout.json";
            char arg3[] = "logic.json";
            char arg4[] = "data.csv";
            char* argv[] = { arg0, arg1, arg2, arg3, arg4 };

            AppCommand cmd{};
            std::string err;
            const bool ok = parse_app_command(5, argv, cmd, err);
            t.check(ok, "unified run command parses");
            t.check(err.empty(), "unified run parse has no error");
            t.check(cmd.kind == CommandKind::Run, "unified run parse selects Run command kind");
            t.check(cmd.run.layout_path.find("layout.json") != std::string::npos,
                "unified run parse records layout path");
            t.check(cmd.run.logic_path.find("logic.json") != std::string::npos,
                "unified run parse records logic path");
            t.check(cmd.run.csv_path.find("data.csv") != std::string::npos,
                "unified run parse records csv path");
            t.check(cmd.run.report_projection == sugar::default_evaluation_report_projection(),
                "unified run parse uses the public default report projection");
            t.check(!sugar::evaluation_report_projection_is_machine_readable(
                    cmd.run.report_projection),
                "unified run default report projection is human-readable");

            char report_flag[] = "--report";
            char report_value[] = "table";
            char* report_argv[] = { arg0, arg1, arg2, arg3, arg4, report_flag, report_value };
            AppCommand report_cmd{};
            err.clear();
            const bool report_ok = parse_app_command(7, report_argv, report_cmd, err);
            t.check(report_ok, "unified run accepts report projection flag");
            t.check(err.empty(), "unified run report projection parse has no error");
            t.check(report_cmd.run.report_projection == sugar::EvaluationReportProjection::CandidateTable,
                "unified run parses table report projection");

            char default_value[] = "default";
            char* default_argv[] = { arg0, arg1, arg2, arg3, arg4, report_flag, default_value };
            AppCommand default_cmd{};
            err.clear();
            const bool default_ok = parse_app_command(7, default_argv, default_cmd, err);
            t.check(default_ok, "unified run accepts explicit default report projection");
            t.check(default_cmd.run.report_projection == sugar::default_evaluation_report_projection(),
                "explicit default report projection maps to the public default");

            char geometry_value[] = "geometry";
            char* geometry_argv[] = { arg0, arg1, arg2, arg3, arg4, report_flag, geometry_value };
            AppCommand geometry_cmd{};
            err.clear();
            const bool geometry_ok = parse_app_command(7, geometry_argv, geometry_cmd, err);
            t.check(geometry_ok, "unified run accepts geometry report projection");
            t.check(geometry_cmd.run.report_projection == sugar::EvaluationReportProjection::SearchGeometry,
                "unified run parses geometry report projection");
            t.check(!sugar::evaluation_report_projection_is_machine_readable(
                    geometry_cmd.run.report_projection),
                "geometry report projection stays human-readable");

            char pareto_value[] = "pareto";
            char* pareto_argv[] = { arg0, arg1, arg2, arg3, arg4, report_flag, pareto_value };
            AppCommand pareto_cmd{};
            err.clear();
            const bool pareto_ok = parse_app_command(7, pareto_argv, pareto_cmd, err);
            t.check(pareto_ok, "unified run accepts pareto report projection");
            t.check(pareto_cmd.run.report_projection == sugar::EvaluationReportProjection::ParetoFront,
                "unified run parses pareto report projection");
            t.check(!sugar::evaluation_report_projection_is_machine_readable(
                    pareto_cmd.run.report_projection),
                "pareto report projection stays human-readable");

            char posterior_value[] = "posterior";
            char* posterior_argv[] = { arg0, arg1, arg2, arg3, arg4, report_flag, posterior_value };
            AppCommand posterior_cmd{};
            err.clear();
            const bool posterior_ok = parse_app_command(7, posterior_argv, posterior_cmd, err);
            t.check(posterior_ok, "unified run accepts posterior report projection");
            t.check(posterior_cmd.run.report_projection ==
                    sugar::EvaluationReportProjection::PosteriorDiagnostics,
                "unified run parses posterior report projection");
            t.check(!sugar::evaluation_report_projection_is_machine_readable(
                    posterior_cmd.run.report_projection),
                "posterior report projection stays human-readable");

            char topology_value[] = "topology";
            char* topology_argv[] = { arg0, arg1, arg2, arg3, arg4, report_flag, topology_value };
            AppCommand topology_cmd{};
            err.clear();
            const bool topology_ok = parse_app_command(7, topology_argv, topology_cmd, err);
            t.check(topology_ok, "unified run accepts topology report projection");
            t.check(topology_cmd.run.report_projection ==
                    sugar::EvaluationReportProjection::TopologyDiagnostics,
                "unified run parses topology report projection");
            t.check(!sugar::evaluation_report_projection_is_machine_readable(
                    topology_cmd.run.report_projection),
                "topology report projection stays human-readable");

            char trace_value[] = "trace";
            char* trace_argv[] = { arg0, arg1, arg2, arg3, arg4, report_flag, trace_value };
            AppCommand trace_cmd{};
            err.clear();
            const bool trace_ok = parse_app_command(7, trace_argv, trace_cmd, err);
            t.check(trace_ok, "unified run accepts trace report projection");
            t.check(trace_cmd.run.report_projection ==
                    sugar::EvaluationReportProjection::ReplayTrace,
                "unified run parses trace report projection");
            t.check(!sugar::evaluation_report_projection_is_machine_readable(
                    trace_cmd.run.report_projection),
                "trace report projection stays human-readable");

            char json_value[] = "json";
            char* json_argv[] = { arg0, arg1, arg2, arg3, arg4, report_flag, json_value };
            AppCommand json_cmd{};
            err.clear();
            const bool json_ok = parse_app_command(7, json_argv, json_cmd, err);
            t.check(json_ok, "unified run accepts json report projection");
            t.check(sugar::evaluation_report_projection_is_machine_readable(
                    json_cmd.run.report_projection),
                "json report projection is explicitly machine-readable");

            char bad_value[] = "verbose";
            char* bad_argv[] = { arg0, arg1, arg2, arg3, arg4, report_flag, bad_value };
            AppCommand bad_cmd{};
            err.clear();
            const bool bad_ok = parse_app_command(7, bad_argv, bad_cmd, err);
            t.check(!bad_ok, "unified run rejects unknown report projection");
            t.check(err.find("--report must be one of") != std::string::npos,
                "unified run report projection error lists valid values");
            return t.failed == 0;
        }

    } // namespace

    void register_cli_tests(AppTestRegistry& tests) {
        tests.push_back(test_parse_help_flag);
        tests.push_back(test_parse_onramp_flag);
        tests.push_back(test_parse_split_help_flags);
        tests.push_back(test_parse_obs_report_command);
        tests.push_back(test_parse_plan_report_command);
        tests.push_back(test_parse_feature_dump_command);
        tests.push_back(test_parse_tabular_format_flags);
        tests.push_back(test_parquet_loader_requires_enabled_build);
        tests.push_back(test_parse_scaffold_command);
        tests.push_back(test_parse_dump_layout_command);
        tests.push_back(test_parse_dump_plan_aliases);
        tests.push_back(test_parse_dump_ops_alias);
        tests.push_back(test_parse_dump_ops_section);
        tests.push_back(test_parse_unified_run_command);
        tests.push_back(test_parse_contract_sections);
        tests.push_back(test_parse_align_dump_requires_series_key);
        tests.push_back(test_parse_panel_topk_validation);
        tests.push_back(test_parse_interactive_data_spec_mode);
        tests.push_back(test_parse_rejects_unknown_interactive_flag);
        tests.push_back(test_parse_interactive_format_requires_value);
    }

} // namespace sugar::app::tests
