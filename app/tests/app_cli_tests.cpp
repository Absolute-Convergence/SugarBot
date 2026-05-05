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
            return t.failed == 0;
        }

    } // namespace

    void register_cli_tests(AppTestRegistry& tests) {
        tests.push_back(test_parse_help_flag);
        tests.push_back(test_parse_obs_report_command);
        tests.push_back(test_parse_plan_report_command);
        tests.push_back(test_parse_feature_dump_command);
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
