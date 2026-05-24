#include "tests/app_test_support.h"

#include <fstream>
#include <ios>
#include <iostream>
#include <sstream>
#include <string>

#include "commands/app_commands.h"
#include "sweep/metrics.h"

namespace sugar::app::tests {

    namespace {

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

            cmd.run.report_projection = sugar::EvaluationReportProjection::CandidateTable;
            std::ostringstream table_out;
            std::ostringstream table_err;
            const int table_rc = run_unified_cli_command(cmd, table_out, table_err);
            const std::string table_text = table_out.str();
            t.check(table_rc == 0, "coarse-fine table projection returns success");
            t.check(table_err.str().empty(), "coarse-fine table projection produces no error output");
            t.check(table_text.find("Sweep candidate table:") != std::string::npos,
                "coarse-fine table projection prints the candidate table section");
            t.check(table_text.find("rank\tcandidate_index\tscore\tpnl\tmax_drawdown\ttrades\ttrade_hash\tobjective_rankability\tparams") != std::string::npos,
                "coarse-fine table projection prints stable tabular columns");
            t.check(table_text.find("Ranked candidate reports:") == std::string::npos,
                "coarse-fine table projection suppresses the full ranked-candidate prose");

            cmd.run.report_projection = sugar::EvaluationReportProjection::Compact;
            std::ostringstream compact_out;
            std::ostringstream compact_err;
            const int compact_rc = run_unified_cli_command(cmd, compact_out, compact_err);
            const std::string compact_text = compact_out.str();
            t.check(compact_rc == 0, "coarse-fine compact projection returns success");
            t.check(compact_err.str().empty(), "coarse-fine compact projection produces no error output");
            t.check(compact_text.find("Sweep summary:") != std::string::npos,
                "coarse-fine compact projection prints compact sweep summary");
            t.check(compact_text.find("best params:") != std::string::npos,
                "coarse-fine compact projection includes best-candidate params");
            t.check(compact_text.find("Ranked candidate reports:") == std::string::npos,
                "coarse-fine compact projection suppresses the full ranked-candidate prose");
            t.check(compact_text.find("Sweep candidate table:") == std::string::npos &&
                    compact_text.find("rank\tcandidate_index\tscore\tpnl") == std::string::npos,
                "coarse-fine compact projection is not the tabular candidate projection");

            cmd.run.report_projection = sugar::EvaluationReportProjection::ObjectiveBreakdown;
            std::ostringstream objective_out;
            std::ostringstream objective_err;
            const int objective_rc = run_unified_cli_command(cmd, objective_out, objective_err);
            const std::string objective_text = objective_out.str();
            t.check(objective_rc == 0, "coarse-fine objective projection returns success");
            t.check(objective_err.str().empty(), "coarse-fine objective projection produces no error output");
            t.check(objective_text.find("Sweep objective breakdown:") != std::string::npos,
                "coarse-fine objective projection prints objective breakdown section");
            t.check(objective_text.find("components:") != std::string::npos,
                "coarse-fine objective projection prints objective components");

            cmd.run.report_projection = sugar::EvaluationReportProjection::DiversityDiagnostics;
            std::ostringstream diversity_out;
            std::ostringstream diversity_err;
            const int diversity_rc = run_unified_cli_command(cmd, diversity_out, diversity_err);
            const std::string diversity_text = diversity_out.str();
            t.check(diversity_rc == 0, "coarse-fine diversity projection returns success");
            t.check(diversity_err.str().empty(), "coarse-fine diversity projection produces no error output");
            t.check(diversity_text.find("Sweep diversity diagnostics:") != std::string::npos,
                "coarse-fine diversity projection prints diversity diagnostics section");
            t.check(diversity_text.find("requested=false") != std::string::npos,
                "coarse-fine diversity projection preserves unrequested diversity state");

            cmd.run.report_projection = sugar::EvaluationReportProjection::SearchGeometry;
            std::ostringstream geometry_out;
            std::ostringstream geometry_err;
            const int geometry_rc = run_unified_cli_command(cmd, geometry_out, geometry_err);
            const std::string geometry_text = geometry_out.str();
            t.check(geometry_rc == 0, "coarse-fine geometry projection returns success");
            t.check(geometry_err.str().empty(), "coarse-fine geometry projection produces no error output");
            t.check(geometry_text.find("Search geometry:") != std::string::npos,
                "coarse-fine geometry projection prints search-geometry section");
            t.check(geometry_text.find("source=retained_candidate_pool") != std::string::npos,
                "coarse-fine geometry projection prints retained-pool source");
            t.check(geometry_text.find("Top basin summaries") != std::string::npos,
                "coarse-fine geometry projection prints basin summaries");
            t.check(geometry_text.find("best_params=") != std::string::npos &&
                    geometry_text.find("score_range=") != std::string::npos,
                "coarse-fine geometry projection prints basin best params and score spread");
            t.check(geometry_text.find("Local robustness around returned winners:") != std::string::npos,
                "coarse-fine geometry projection prints local robustness");
            t.check(geometry_text.find("Sweep diversity diagnostics:") == std::string::npos,
                "coarse-fine geometry projection suppresses diversity configuration block");

            cmd.run.report_projection = sugar::EvaluationReportProjection::DuplicateTradePaths;
            std::ostringstream duplicates_out;
            std::ostringstream duplicates_err;
            const int duplicates_rc = run_unified_cli_command(cmd, duplicates_out, duplicates_err);
            const std::string duplicates_text = duplicates_out.str();
            t.check(duplicates_rc == 0, "coarse-fine duplicate-path projection returns success");
            t.check(duplicates_err.str().empty(), "coarse-fine duplicate-path projection produces no error output");
            t.check(duplicates_text.find("Duplicate trade-path groups:") != std::string::npos,
                "coarse-fine duplicate-path projection prints duplicate trade-path section");
            t.check(duplicates_text.find("Ranked candidate reports:") == std::string::npos,
                "coarse-fine duplicate-path projection suppresses the full ranked-candidate prose");

            cmd.run.report_projection = sugar::EvaluationReportProjection::PosteriorDiagnostics;
            std::ostringstream posterior_out;
            std::ostringstream posterior_err;
            const int posterior_rc = run_unified_cli_command(cmd, posterior_out, posterior_err);
            const std::string posterior_text = posterior_out.str();
            t.check(posterior_rc == 0, "coarse-fine posterior projection returns success");
            t.check(posterior_err.str().empty(),
                "coarse-fine posterior projection produces no error output");
            t.check(posterior_text.find("Bayesian posterior diagnostics:") != std::string::npos &&
                    posterior_text.find("requested: false") != std::string::npos &&
                    posterior_text.find("status: not_requested") != std::string::npos &&
                    posterior_text.find("engine: coarse_fine") != std::string::npos,
                "coarse-fine posterior projection reports posterior diagnostics as not requested");
            t.check(posterior_text.find("=== Plan Sweep Report ===") == std::string::npos &&
                    posterior_text.find("TopK:") == std::string::npos &&
                    posterior_text.find("{\"schema_version\"") == std::string::npos,
                "coarse-fine posterior projection stays focused on posterior diagnostics");

            cmd.run.report_projection = sugar::EvaluationReportProjection::ReplayTrace;
            std::ostringstream trace_out;
            std::ostringstream trace_err;
            const int trace_rc = run_unified_cli_command(cmd, trace_out, trace_err);
            const std::string trace_text = trace_out.str();
            t.check(trace_rc == 0, "coarse-fine trace projection returns success");
            t.check(trace_err.str().empty(), "coarse-fine trace projection produces no error output");
            t.check(trace_text.find("Sweep replay trace:") != std::string::npos &&
                    trace_text.find("policy=coarse_fine") != std::string::npos &&
                    trace_text.find("trace_digest=0x") != std::string::npos &&
                    trace_text.find("exact_replay_from_report=true") != std::string::npos,
                "coarse-fine trace projection prints replay metadata");
            t.check(trace_text.find("event_sample:") != std::string::npos &&
                    trace_text.find("proposal stage=") != std::string::npos &&
                    trace_text.find("observation stage=") != std::string::npos,
                "coarse-fine trace projection prints proposal and observation samples");
            t.check(trace_text.find("=== Plan Sweep Report ===") == std::string::npos &&
                    trace_text.find("TopK:") == std::string::npos &&
                    trace_text.find("{\"schema_version\"") == std::string::npos,
                "coarse-fine trace projection stays focused on replay diagnostics");

            cmd.run.report_projection = sugar::EvaluationReportProjection::Json;
            std::ostringstream json_out;
            std::ostringstream json_err;
            const int json_rc = run_unified_cli_command(cmd, json_out, json_err);
            const std::string json_text = json_out.str();
            t.check(json_rc == 0, "coarse-fine json projection returns success");
            t.check(json_err.str().empty(), "coarse-fine json projection produces no error output");
            t.check(json_text.find("{\"schema_version\":\"sugarbot.evaluation_report.v1\"") == 0,
                "coarse-fine json projection starts with the report JSON object");
            t.check(json_text.find("\"kind\":\"sweep\"") != std::string::npos,
                "coarse-fine json projection identifies sweep reports");
            t.check(json_text.find("\"candidates\":[") != std::string::npos,
                "coarse-fine json projection serializes candidate report envelopes");
            t.check(json_text.find("\"parameter_ranges\":[") != std::string::npos,
                "coarse-fine json projection serializes sweep parameter ranges");
            t.check(json_text.find("\"stage_summaries\":[") != std::string::npos,
                "coarse-fine json projection serializes sweep stage summaries");
            t.check(json_text.find("\"candidate_outcomes\":{") != std::string::npos,
                "coarse-fine json projection serializes candidate outcome counts");
            t.check(json_text.find("\"candidate_rankability\":{") != std::string::npos,
                "coarse-fine json projection serializes candidate rankability counts");
            t.check(json_text.find("\"dedupe_trade_hash\":") != std::string::npos,
                "coarse-fine json projection serializes result-diversity configuration");
            t.check(json_text.find("\"metric_catalog\":[") != std::string::npos,
                "coarse-fine json projection serializes nested run metric catalogs");
            t.check(json_text.find("\"name\":\"trade_hash\"") != std::string::npos,
                "coarse-fine json projection preserves exact trade-hash metric entries");
            t.check(json_text.find("\"provenance\":{") != std::string::npos,
                "coarse-fine json projection serializes nested run provenance");
            t.check(json_text.find("\"source\":\"retained_candidate_pool\"") != std::string::npos,
                "coarse-fine json projection serializes search-geometry source");
            t.check(json_text.find("\"best_params\":{") != std::string::npos &&
                    json_text.find("\"score_range\":") != std::string::npos,
                "coarse-fine json projection serializes basin best params and score spread");
            return t.failed == 0;
        }

        bool test_unified_run_bayesian_posterior_json_reports_computed_diagnostics(TestContext& t) {
            t.out << "[app_dispatch] bayesian-posterior computed json report\n";
            RunFixturePaths fixture{};
            std::string fixture_err;
            const bool fixture_ok = create_unified_single_series_fixture(fixture, fixture_err);
            t.check(fixture_ok, "bayesian-posterior fixture files are created");
            if (!fixture_ok) return false;

            const auto fixture_dir = app_fixture_dir(fixture_err);
            t.check(!fixture_dir.empty(), "bayesian-posterior fixture dir is available");
            if (fixture_dir.empty()) return false;

            const auto logic_path =
                fixture_dir / "app_tests_tmp_bayesian_posterior_logic.json";
            {
                std::ofstream logic_out(logic_path, std::ios::binary | std::ios::trunc);
                t.check(static_cast<bool>(logic_out), "bayesian-posterior logic file opens");
                if (!logic_out) return false;
                logic_out <<
                    "{\n"
                    "  \"version\": 1,\n"
                    "  \"name\": \"app_tests_bayesian_posterior_behavior\",\n"
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
                    "    \"engine\": \"bayesian_posterior\",\n"
                    "    \"top_k\": 2,\n"
                    "    \"max_evals\": 8,\n"
                    "    \"params\": {\n"
                    "      \"threshold\": { \"values\": [0, 5, 10, 12], \"is_int\": true }\n"
                    "    },\n"
                    "    \"engine_config\": {\n"
                    "      \"bayesian_posterior\": {\n"
                    "        \"acquisition\": \"probability_of_improvement\",\n"
                    "        \"initial_design_size\": 3,\n"
                    "        \"iterations\": 5,\n"
                    "        \"seed\": 41,\n"
                    "        \"normalize_inputs\": true,\n"
                    "        \"top_k\": 2\n"
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
            cmd.run.report_projection = sugar::EvaluationReportProjection::Json;

            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_unified_cli_command(cmd, out, err);
            const std::string text = out.str();

            t.check(rc == 0,
                "bayesian-posterior json projection returns success");
            t.check(err.str().empty(),
                "bayesian-posterior json projection keeps stderr quiet");
            t.check(text.find("{\"schema_version\":\"sugarbot.evaluation_report.v1\"") == 0,
                "bayesian-posterior json projection starts with report JSON");
            t.check(text.find("\"engine\":\"bayesian_posterior\"") != std::string::npos &&
                    text.find("\"evaluations_run\":4") != std::string::npos,
                "bayesian-posterior json projection reports executed evaluations");
            t.check(text.find("\"posterior\":{") != std::string::npos &&
                    text.find("\"computed\":true") != std::string::npos &&
                    text.find("\"status\":\"computed\"") != std::string::npos,
                "bayesian-posterior json projection preserves computed posterior diagnostics");
            t.check(text.find("\"acquisition\":\"probability_of_improvement\"") != std::string::npos &&
                    text.find("\"initial_design_size\":3") != std::string::npos &&
                    text.find("\"iterations\":1") != std::string::npos &&
                    text.find("\"seed\":41") != std::string::npos,
                "bayesian-posterior json projection preserves effective typed config");
            t.check(text.find("\"surrogate_trained\":true") != std::string::npos &&
                    text.find("\"training_observation_count\":4") != std::string::npos &&
                    text.find("\"finite_observation_count\":4") != std::string::npos &&
                    text.find("\"surrogate_training_observation_count\":4") != std::string::npos &&
                    text.find("\"invalid_training_observation_count\":0") != std::string::npos &&
                    text.find("\"acquisition_ready\":true") != std::string::npos &&
                    text.find("\"proposal_reasons\":[") != std::string::npos,
                "bayesian-posterior json projection reports surrogate and proposal diagnostics");
            t.check(text.find("PlanSpec sweep error:") == std::string::npos &&
                    text.find("[Info] noninteractive artifact run active.") == std::string::npos,
                "bayesian-posterior json projection avoids text preamble");

            AppCommand full_cmd = cmd;
            full_cmd.run.report_projection = sugar::EvaluationReportProjection::Full;
            std::ostringstream full_out;
            std::ostringstream full_err;
            const int full_rc = run_unified_cli_command(full_cmd, full_out, full_err);
            const std::string full_text = full_out.str();

            t.check(full_rc == 0,
                "bayesian-posterior full projection returns success");
            t.check(full_err.str().empty(),
                "bayesian-posterior full projection keeps stderr quiet");
            t.check(full_text.find("Bayesian posterior diagnostics:") != std::string::npos &&
                    full_text.find("  status:\n") != std::string::npos &&
                    full_text.find("  policy config:\n") != std::string::npos &&
                    full_text.find("  training data:\n") != std::string::npos &&
                    full_text.find("  proposal summary:\n") != std::string::npos,
                "bayesian-posterior full projection groups posterior diagnostics for human review");
            t.check(full_text.find("finite observations: 4") != std::string::npos &&
                    full_text.find("surrogate training rows: 4") != std::string::npos &&
                    full_text.find("invalid feature observations filtered: 0") != std::string::npos &&
                    full_text.find("proposal decision details:") != std::string::npos,
                "bayesian-posterior full projection exposes posterior training and proposal accounting");

            AppCommand posterior_cmd = cmd;
            posterior_cmd.run.report_projection =
                sugar::EvaluationReportProjection::PosteriorDiagnostics;
            std::ostringstream posterior_out;
            std::ostringstream posterior_err;
            const int posterior_rc =
                run_unified_cli_command(posterior_cmd, posterior_out, posterior_err);
            const std::string posterior_text = posterior_out.str();

            t.check(posterior_rc == 0,
                "bayesian-posterior focused projection returns success");
            t.check(posterior_err.str().empty(),
                "bayesian-posterior focused projection keeps stderr quiet");
            t.check(posterior_text.find("Bayesian posterior diagnostics:") != std::string::npos &&
                    posterior_text.find("  policy config:\n") != std::string::npos &&
                    posterior_text.find("  training data:\n") != std::string::npos &&
                    posterior_text.find("  proposal summary:\n") != std::string::npos &&
                    posterior_text.find("proposal decision details:") != std::string::npos,
                "bayesian-posterior focused projection is human-readable");
            t.check(posterior_text.find("=== Plan Sweep Report ===") == std::string::npos &&
                    posterior_text.find("TopK:") == std::string::npos &&
                    posterior_text.find("{\"schema_version\"") == std::string::npos,
                "bayesian-posterior focused projection stays focused on posterior diagnostics");
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

        bool test_single_run_trace_projection_explains_absence(TestContext& t) {
            t.out << "[app_run] single-run trace projection guidance\n";
            RunFixturePaths fixture{};
            std::string fixture_err;
            const bool fixture_ok = create_unified_single_series_fixture(fixture, fixture_err);
            t.check(fixture_ok, "single-run trace fixture files are created");
            if (!fixture_ok) return false;

            AppCommand cmd{};
            cmd.kind = CommandKind::Run;
            cmd.run.layout_path = fixture.layout_path;
            cmd.run.logic_path = fixture.logic_path;
            cmd.run.csv_path = fixture.csv_path;
            cmd.run.report_projection = sugar::EvaluationReportProjection::ReplayTrace;

            std::ostringstream out;
            std::ostringstream err;
            const int rc = run_unified_cli_command(cmd, out, err);
            const std::string text = out.str();

            t.check(rc == 0, "single-run trace projection returns success");
            t.check(err.str().empty(), "single-run trace projection keeps stderr quiet");
            t.check(text.find("Sweep replay trace:") != std::string::npos &&
                    text.find("captured=false") != std::string::npos,
                "single-run trace projection prints an explicit unavailable trace section");
            t.check(text.find("single-run") != std::string::npos &&
                    text.find("enable `sweep.enabled=true`") != std::string::npos,
                "single-run trace projection explains the missing sweep trace and next step");

            cmd.run.report_projection = sugar::EvaluationReportProjection::CandidateTable;
            std::ostringstream table_out;
            std::ostringstream table_err;
            const int table_rc = run_unified_cli_command(cmd, table_out, table_err);
            const std::string table_text = table_out.str();

            t.check(table_rc == 0, "single-run table projection returns success with guidance");
            t.check(table_err.str().empty(), "single-run table projection keeps stderr quiet");
            t.check(table_text.find("Sweep candidate table:") != std::string::npos &&
                    table_text.find("available=false") != std::string::npos &&
                    table_text.find("report_kind=run") != std::string::npos,
                "single-run table projection explains that candidate rows require a sweep report");
            t.check(table_text.find("Run summary:") == std::string::npos,
                "single-run table projection no longer aliases compact output");
            return t.failed == 0;
        }

    } // namespace

    void register_run_projection_tests(AppTestRegistry& tests) {
        tests.push_back(test_unified_run_coarse_fine_displays_top_level_topk);
        tests.push_back(test_unified_run_bayesian_posterior_json_reports_computed_diagnostics);
        tests.push_back(test_unified_run_suppresses_internal_cross_debug_warning_by_default);
        tests.push_back(test_single_run_trace_projection_explains_absence);
    }

} // namespace sugar::app::tests
