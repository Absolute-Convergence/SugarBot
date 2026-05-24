#include "contract_suite_runtime_bridge_sweep.h"
#include "contract_suite_runtime_shared.h"
#include "strategy_plan_internal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "op_registry_internal.h"
#include "ops/op_registry.h"
#include "sweep/evaluation_report.h"
#include "sweep/panel_runner.h"
#include "sweep/plan_capability.h"
#include "sweep/sweep.h"
#include "engine/plan_compiler.h"
#include "sweep/plan_sweep.h"
#include "engine/program_evaluator.h"
#include "ingest/utils.h"
#include "ingest/run_manifest.h"

namespace sugar::contract_suite_internal {

    bool run_compile_bridge_sweep_report_contracts(ContractAsserts& t) {
        {
            const std::string objective_json = R"JSON(
{
  "version": 1,
  "name": "contract_phase6_numeric_objective",
  "trade_mode": "LongOnly",
  "params": {
    "alpha": 1
  },
  "numeric_nodes": [
    { "id": "zero", "op": "core.const", "params": { "value": 0.0 } },
    { "id": "one", "op": "core.const", "params": { "value": 1.0 } },
    { "id": "score_value", "op": "core.const", "params": { "value": { "ref": "alpha" } } }
  ],
  "bool_nodes": [
    { "id": "entry", "op": "cmp.gt", "args": ["one", "zero"] }
  ],
  "bindings": {
    "entry_long": "entry",
    "exit_long": "",
    "entry_short": "",
    "exit_short": "",
    "allow_trade": ""
  },
  "sweep": {
    "enabled": true,
    "top_k": 3,
    "max_evals": 3,
    "objective": {
      "value": {
        "source": "numeric_node_output",
        "name": "score_value",
        "summary": "last_finite"
      },
      "direction": "maximize",
      "tie_breaks": [
        {
          "value": {
            "source": "reported_metric",
            "name": "pnl"
          },
          "direction": "maximize"
        }
      ]
    },
    "params": {
      "alpha": {
        "values": [1, 5, 3],
        "is_int": true
      }
    }
  }
}
)JSON";

            PlanSpec sweep_spec{};
            std::string parse_err;
            const bool parse_ok = parse_plan_spec_json(objective_json, sweep_spec, parse_err);
            t.expect(parse_ok, "phase 6 sweep objective object form parses");
            if (!parse_ok) {
                t.out << "    phase 6 objective parse error: " << parse_err << "\n";
                return false;
            }
            t.expect(sweep_spec.sweep.has_value() && sweep_spec.sweep->objective_spec.has_value(),
                "phase 6 sweep objective object form populates objective_spec");
            if (sweep_spec.sweep && sweep_spec.sweep->objective_spec) {
                t.expect(sweep_spec.sweep->objective_spec->primary.source == ObjectiveValueSource::NumericNodeOutput,
                    "phase 6 objective spec preserves numeric node source");
                t.expect(sweep_spec.sweep->objective_spec->primary.name == "score_value",
                    "phase 6 objective spec preserves named numeric node output");
                t.expect(sweep_spec.sweep->objective_spec->direction == ObjectiveDirection::Maximize,
                    "phase 6 objective spec preserves direction");
                t.expect(sweep_spec.sweep->objective_spec->tie_breaks.size() == 1,
                    "phase 6 objective spec preserves tie-break chain");
            }

            const std::string run_objective_json = R"JSON(
{
  "version": 1,
  "trade_mode": "LongOnly",
  "numeric_nodes": [
    { "id": "c", "op": "core.close" },
    { "id": "hold_days", "op": "core.const", "params": { "value": 7 } }
  ],
  "bool_nodes": [
    { "id": "entry", "op": "cmp.gt", "args": ["c", "c"] }
  ],
  "bindings": {
    "entry_long": "entry",
    "exit_long": "",
    "entry_short": "",
    "exit_short": "",
    "allow_trade": ""
  },
  "run": {
    "scope": "per_entity_ranked",
    "top_k": 2,
    "objective": {
      "value": {
        "source": "reported_metric",
        "name": "pnl"
      },
      "direction": "maximize",
      "tie_breaks": [
        {
          "value": {
            "source": "reported_metric",
            "name": "trades"
          },
          "direction": "minimize"
        }
      ]
    }
  }
}
)JSON";

            PlanSpec run_objective_spec{};
            parse_err.clear();
            const bool run_parse_ok = parse_plan_spec_json(run_objective_json, run_objective_spec, parse_err);
            t.expect(run_parse_ok, "phase 6 run objective object form parses");
            if (run_parse_ok && run_objective_spec.run && run_objective_spec.run->objective_spec) {
                t.expect(run_objective_spec.run->objective_spec->primary.source == ObjectiveValueSource::ReportedMetric,
                    "phase 6 run objective spec preserves reported metric source");
                t.expect(run_objective_spec.run->objective_spec->primary.name == "pnl",
                    "phase 6 run objective spec preserves primary metric name");
                t.expect(run_objective_spec.run->objective_spec->tie_breaks.size() == 1 &&
                        run_objective_spec.run->objective_spec->tie_breaks.front().value.name == "trades",
                    "phase 6 run objective spec preserves tie-break metadata");
            }

            std::vector<double> close;
            close.reserve(8);
            for (std::size_t i = 0; i < 8; ++i) {
                close.push_back(100.0 + static_cast<double>(i));
            }

            std::shared_ptr<align::AlignedBundle> bundle;
            ColumnRef primary{};
            const bool okdv = make_single_series_dataview_from_close(close, bundle, primary);
            t.expect(okdv && bundle != nullptr,
                "phase 6 numeric objective sweep fixture creates temporal DataView input");
            if (!okdv || !bundle) return false;

            const DataView dv(*bundle);
            PlanSweepRuntimeOptions opt{};
            std::vector<SweepProgress> progress_log;
            opt.progress_cb = [&](const SweepProgress& progress) {
                progress_log.push_back(progress);
            };
            RunManifest run_manifest{};
            run_manifest.preprocess.series_keys = { "A" };
            run_manifest.preprocess.has_time_axis = true;
            run_manifest.preprocess.time_kind = TimeKind::YyyyMmDd;
            run_manifest.preprocess.time_resolution = "daily";
            run_manifest.preprocess.alignment_applied = true;
            run_manifest.preprocess.alignment_calendar = align::CalendarPolicy::Intersection;
            run_manifest.preprocess.alignment_missing = align::MissingPolicy::NaN;
            run_manifest.preprocess.alignment_non_temporal_multi_series =
                align::NonTemporalMultiSeriesPolicy::Reject;
            run_manifest.preprocess.input_warnings.push_back("contract warning");
            run_manifest.primary_series_key = "A";
            run_manifest.primary_selection_mode = RuntimePrimarySelectionMode::ExplicitSeries;
            TopologySpec topology{};
            topology.name = builtin_temporal_topology_name();
            run_manifest.topologies.push_back(std::move(topology));
            run_manifest.topology_usages.push_back(TopologyUsageSnapshot{});
            const RunProvenanceSummary run_provenance =
                build_run_provenance_summary(run_manifest);
            std::ostringstream sweep_log;
            BacktestResult sweep_best{};
            PlanSweepReport sweep_report{};
            const bool sweep_ok = run_custom_plan_spec(
                dv,
                primary,
                sweep_spec,
                opt,
                sweep_log,
                &sweep_best,
                &sweep_report,
                &run_provenance);
            t.expect(sweep_ok, "phase 6 numeric objective sweep executes through custom runtime");
            if (sweep_ok) {
                double score_value = std::numeric_limits<double>::quiet_NaN();
                const bool score_ok = std::any_of(
                    sweep_best.reported_values.begin(),
                    sweep_best.reported_values.end(),
                    [&](const NamedReportedValue& value) {
                        if (value.name != "score_value") {
                            return false;
                        }
                        score_value = value.value;
                        return true;
                    });
                t.expect(score_ok, "phase 6 numeric objective sweep preserves the named reported value on the best candidate");
                if (score_ok) {
                    t.expect(nearly_equal(score_value, 5.0),
                        "phase 6 numeric objective sweep ranks by the named numeric node output");
                }
                const std::string sweep_msg = sweep_log.str();
                t.expect(sweep_msg.find("numeric_node_output:score_value [last_finite]") != std::string::npos,
                    "phase 6 sweep report prints the structured numeric objective description");
                t.expect(sweep_msg.find("Run provenance:") != std::string::npos,
                    "phase 6 sweep report prints shared run provenance when provided");
                t.expect(sweep_msg.find("primary_selection_mode: explicit_series") != std::string::npos,
                    "phase 6 sweep report prints human-readable primary-selection provenance");
                t.expect(sweep_msg.find("Proposal trace:") != std::string::npos &&
                        sweep_msg.find("policy: exhaustive_topk") != std::string::npos,
                    "milestone E foundation full sweep report prints proposal trace summary");
                {
                    std::ostringstream trace_projection;
                    print_evaluation_report(
                        make_sweep_evaluation_report(sweep_report),
                        EvaluationReportProjection::ReplayTrace,
                        trace_projection);
                    const std::string trace_text = trace_projection.str();
                    t.expect(trace_text.find("Sweep replay trace:") != std::string::npos &&
                            trace_text.find("policy=exhaustive_topk") != std::string::npos &&
                            trace_text.find("trace_digest=0x") != std::string::npos &&
                            trace_text.find("exact_replay_from_report=true") != std::string::npos &&
                            trace_text.find("event_sample:") != std::string::npos &&
                            trace_text.find("proposal stage=") != std::string::npos &&
                            trace_text.find("observation stage=") != std::string::npos &&
                            trace_text.find("=== Plan Sweep Report ===") == std::string::npos,
                        "milestone E focused trace projection exposes replay diagnostics without full sweep prose");
                }
                t.expect(sweep_msg.find("alpha: 5") != std::string::npos,
                    "phase 6 sweep report ranks the highest objective-valued parameter first");
                t.expect(sweep_report.parameter_count == 1,
                    "phase 6 structured sweep report preserves parameter dimension count");
                t.expect(sweep_report.total_combinations == 3 && !sweep_report.total_combinations_overflow,
                    "phase 6 structured sweep report preserves exact total combination count");
                t.expect(sweep_report.candidate_space_size == 3 && !sweep_report.candidate_space_truncated,
                    "phase 6 structured sweep report preserves candidate space size without false truncation");
                t.expect(sweep_report.evaluations_run == 3,
                    "phase 6 structured sweep report preserves executed evaluation count");
                t.expect(sweep_report.proposal_trace.captured &&
                        sweep_report.proposal_trace.policy_name == "exhaustive_topk" &&
                        sweep_report.proposal_trace.proposal_count == 3 &&
                        sweep_report.proposal_trace.observation_count == 3 &&
                        sweep_report.proposal_trace.total_event_count == 6 &&
                        !sweep_report.proposal_trace.events_capped,
                    "milestone E foundation records exhaustive proposal and observation counts");
                t.expect(sweep_report.proposal_trace.events.size() >= 2 &&
                        sweep_report.proposal_trace.events[0].kind == SweepTraceEventKind::Proposal &&
                        sweep_report.proposal_trace.events[0].candidate_index == 0 &&
                        sweep_report.proposal_trace.events[0].proposal_order == 1 &&
                        sweep_report.proposal_trace.events[1].kind == SweepTraceEventKind::Observation &&
                        sweep_report.proposal_trace.events[1].candidate_index == 0 &&
                        sweep_report.proposal_trace.events[1].proposal_order == 1 &&
                        sweep_report.proposal_trace.events[1].run_outcome_present &&
                        sweep_report.proposal_trace.events[1].run_outcome ==
                            RunOutcomeState::Completed &&
                        sweep_report.proposal_trace.events[1].run_rankability_present &&
                        sweep_report.proposal_trace.events[1].run_rankability ==
                            RunRankabilityState::Rankable &&
                        sweep_report.proposal_trace.observation_payload_complete,
                    "milestone E foundation preserves candidate proposal order and observation payloads in the trace prefix");
                t.expect(sweep_report.run_provenance.primary_series_key == "A" &&
                        sweep_report.run_provenance.primary_selection_mode == "explicit_series",
                    "phase 6 structured sweep report preserves human-readable run provenance");
                t.expect(sweep_report.candidate_outcomes.completed == 3 &&
                        sweep_report.candidate_outcomes.completed_with_non_finite_metrics == 0 &&
                        sweep_report.candidate_outcomes.failed == 0 &&
                        sweep_report.candidate_outcomes.canceled == 0,
                    "phase 6 structured sweep report preserves candidate outcome counts");
                t.expect(sweep_report.candidate_rankability.rankable == 3 &&
                        sweep_report.candidate_rankability.not_evaluated == 0,
                    "phase 6 structured sweep report preserves candidate rankability counts");
                const bool has_alpha_range =
                    sweep_report.parameter_ranges.size() == 1 &&
                    sweep_report.parameter_ranges.front().name == "alpha" &&
                    sweep_report.parameter_ranges.front().expanded_count == 3;
                t.expect(has_alpha_range,
                    "phase 6 structured sweep report preserves named parameter range summaries");
                const bool has_ranking_stage = std::any_of(
                    sweep_report.stage_summaries.begin(),
                    sweep_report.stage_summaries.end(),
                    [](const SweepStageSummary& stage) {
                        return stage.stage == SweepProgressStage::Ranking &&
                            stage.done == 1 &&
                            stage.total == 1;
                    });
                t.expect(has_ranking_stage,
                    "phase 6 structured sweep report preserves ranking stage completion");
                const bool has_report_stage = std::any_of(
                    sweep_report.stage_summaries.begin(),
                    sweep_report.stage_summaries.end(),
                    [](const SweepStageSummary& stage) {
                        return stage.stage == SweepProgressStage::ReportAssembly &&
                            stage.done == 1 &&
                            stage.total == 1;
                    });
                t.expect(has_report_stage,
                    "phase 6 structured sweep report preserves report-assembly stage completion");
                const bool best_has_alpha_value =
                    !sweep_report.top_candidates.empty() &&
                    !sweep_report.top_candidates.front().parameter_values.empty() &&
                    sweep_report.top_candidates.front().parameter_values.front().name == "alpha" &&
                    std::holds_alternative<std::int64_t>(sweep_report.top_candidates.front().parameter_values.front().value) &&
                    std::get<std::int64_t>(sweep_report.top_candidates.front().parameter_values.front().value) == 5;
                t.expect(best_has_alpha_value,
                    "phase 6 structured sweep report preserves human-readable best-candidate parameter values");
                t.expect(sweep_report.candidate_reports.size() == sweep_report.top_candidates.size(),
                    "milestone B sweep report builds candidate report envelopes beside legacy ranked candidates");
                if (!sweep_report.candidate_reports.empty() && !sweep_report.top_candidates.empty()) {
                    const SweepCandidateReport& candidate_report = sweep_report.candidate_reports.front();
                    const SweepRankedCandidate& ranked_candidate = sweep_report.top_candidates.front();
                    t.expect(candidate_report.rank == 1 &&
                            candidate_report.candidate_index == ranked_candidate.candidate_index &&
                            nearly_equal(candidate_report.score, ranked_candidate.score),
                        "milestone B sweep candidate report preserves rank, candidate index, and scalar score");
                    t.expect(candidate_report.parameter_values.size() == ranked_candidate.parameter_values.size() &&
                            !candidate_report.parameter_values.empty() &&
                            candidate_report.parameter_values.front().name == "alpha",
                        "milestone B sweep candidate report preserves parameter values for table projections");
                    t.expect(candidate_report.run.objective.requested &&
                            candidate_report.run.objective.rankability == RunRankabilityState::Rankable &&
                            candidate_report.run.provenance.primary_series_key == "A",
                        "milestone B sweep candidate report wraps the candidate execution result in a run report envelope");
                }
                const RunReport best_report = build_run_report(
                    sweep_best,
                    &sweep_report.resolved_objective,
                    &run_provenance);
                t.expect(best_report.objective.requested,
                    "phase 6 run report preserves requested ranking objective");
                t.expect(best_report.objective.rankability == RunRankabilityState::Rankable,
                    "phase 6 run report classifies finite objective results as rankable");
                t.expect(nearly_equal(best_report.objective.score, 5.0),
                    "phase 6 run report preserves evaluated objective score");
                t.expect(best_report.provenance.primary_series_key == "A" &&
                        best_report.provenance.topology_usage_count == 1,
                    "phase 6 run report preserves shared provenance without pushing it into BacktestResult");
                const auto* reported_metric = find_metric_value(best_report, "score_value");
                t.expect(
                    reported_metric != nullptr &&
                    reported_metric->kind == ResultMetricKind::ReportedValue &&
                    nearly_equal(reported_metric->value, 5.0),
                    "phase 6 result metric catalog preserves reported numeric-node values");
                const auto* row_metric = find_metric_value(best_report, "rows_evaluated");
                t.expect(
                    row_metric != nullptr &&
                    row_metric->kind == ResultMetricKind::CoreMetric &&
                    nearly_equal(row_metric->value, 8.0),
                    "phase 6 result metric catalog preserves executor row coverage");
                const auto* signal_metric = find_metric_value(best_report, "entry_long_signals");
                t.expect(
                    signal_metric != nullptr &&
                    signal_metric->kind == ResultMetricKind::SignalCount &&
                    signal_metric->value > 0.0,
                    "phase 6 result metric catalog preserves signal-count metrics");
                const bool has_primary_component =
                    !best_report.objective.components.empty() &&
                    best_report.objective.components.front().role == RunObjectiveComponentRole::Primary &&
                    best_report.objective.components.front().value.name == "score_value" &&
                    best_report.objective.components.front().present &&
                    best_report.objective.components.front().finite &&
                    nearly_equal(best_report.objective.components.front().raw_value, 5.0) &&
                    nearly_equal(best_report.objective.components.front().score_contribution, 5.0);
                t.expect(has_primary_component,
                    "milestone B run report exposes primary objective breakdown components without changing BacktestResult");
                const bool has_tie_break_component = std::any_of(
                    best_report.objective.components.begin(),
                    best_report.objective.components.end(),
                    [](const RunObjectiveComponent& component) {
                        return component.role == RunObjectiveComponentRole::TieBreak &&
                            component.value.name == "pnl";
                    });
                t.expect(has_tie_break_component,
                    "milestone B run report preserves tie-break objective components for future breakdown projections");
                const EvaluationReport run_envelope = make_run_evaluation_report(best_report);
                {
                    std::ostringstream direct_full;
                    std::ostringstream projected_full;
                    print_run_report_summary(best_report, direct_full);
                    print_evaluation_report(
                        run_envelope,
                        EvaluationReportProjection::Full,
                        projected_full);
                    t.expect(direct_full.str() == projected_full.str(),
                        "milestone B full run projection preserves the existing text report exactly");
                }
                {
                    std::ostringstream json;
                    print_evaluation_report(
                        run_envelope,
                        EvaluationReportProjection::Json,
                        json);
                    const std::string text = json.str();
                    t.expect(text.find("\"metric_catalog\":[") != std::string::npos &&
                            text.find("\"name\":\"trade_hash\",\"kind\":\"core_metric\",\"value\":") != std::string::npos &&
                            text.find("\"name\":\"score_value\",\"kind\":\"reported_value\",\"value\":5") != std::string::npos,
                        "milestone B raw JSON projection preserves metric catalog entries with exact integral metrics");
                    t.expect(text.find("\"provenance\":{") != std::string::npos &&
                            text.find("\"primary_series_key\":\"A\"") != std::string::npos,
                        "milestone B raw JSON projection preserves run provenance");
                }
                {
                    PanelReport panel_report{};
                    panel_report.resolved_objective = sweep_report.resolved_objective;
                    panel_report.entities_total = 1;
                    panel_report.entities_ok = 1;
                    PanelEntityReport row{};
                    row.series_key = "A";
                    row.entity_key = "A";
                    row.ranking_score = best_report.objective.score;
                    row.run = best_report;
                    panel_report.top_rows.push_back(row);

                    std::ostringstream direct_full;
                    std::ostringstream projected_full;
                    print_panel_report_summary(panel_report, direct_full);
                    print_evaluation_report(
                        make_panel_evaluation_report(panel_report),
                        EvaluationReportProjection::Full,
                        projected_full);
                    t.expect(direct_full.str() == projected_full.str(),
                        "milestone B full panel projection preserves the existing text report exactly");
                }
                const EvaluationReport sweep_envelope = make_sweep_evaluation_report(sweep_report);
                t.expect(sweep_envelope.kind == EvaluationReportKind::Sweep &&
                        sweep_envelope.sweep.has_value() &&
                        !sweep_envelope.run.has_value() &&
                        !sweep_envelope.panel.has_value(),
                    "milestone B evaluation report wraps sweep output without inventing hosted or interactive schema");
                t.expect(std::string(evaluation_report_kind_name(sweep_envelope.kind)) == "sweep",
                    "milestone B evaluation report kind has stable CLI-facing text");
                t.expect(default_evaluation_report_projection() == EvaluationReportProjection::Full &&
                        !evaluation_report_projection_is_machine_readable(default_evaluation_report_projection()) &&
                        evaluation_report_projection_is_machine_readable(EvaluationReportProjection::Json) &&
                        evaluation_report_projection_usage().find("geometry") != std::string_view::npos &&
                        evaluation_report_projection_usage().find("posterior") != std::string_view::npos &&
                        evaluation_report_projection_usage().find("trace") != std::string_view::npos,
                    "milestone B report projection defaults are explicit and human-readable");
                {
                    std::ostringstream direct_full;
                    std::ostringstream projected_full;
                    print_plan_sweep_report(sweep_report, direct_full);
                    print_evaluation_report(
                        sweep_envelope,
                        EvaluationReportProjection::Full,
                        projected_full);
                    t.expect(direct_full.str() == projected_full.str(),
                        "milestone B full sweep projection preserves the existing text report exactly");
                }
                {
                    std::ostringstream compact;
                    print_evaluation_report(
                        sweep_envelope,
                        EvaluationReportProjection::Compact,
                        compact);
                    const std::string text = compact.str();
                    t.expect(text.find("Sweep summary:") != std::string::npos &&
                            text.find("best params:") != std::string::npos,
                        "milestone B compact sweep projection exposes summary and best params");
                }
                {
                    std::ostringstream json;
                    print_evaluation_report(
                        sweep_envelope,
                        EvaluationReportProjection::Json,
                        json);
                    const std::string text = json.str();
                    t.expect(text.find("{\"schema_version\":\"sugarbot.evaluation_report.v1\"") == 0 &&
                            text.find("\"parameter_ranges\":[") != std::string::npos &&
                            text.find("\"stage_summaries\":[") != std::string::npos,
                        "milestone B sweep JSON projection exposes schema, parameter ranges, and stage history");
                    t.expect(text.find("\"proposal_trace\":{") != std::string::npos &&
                            text.find("\"policy_name\":\"exhaustive_topk\"") != std::string::npos &&
                            text.find("\"proposal_count\":3") != std::string::npos &&
                            text.find("\"observation_count\":3") != std::string::npos &&
                            text.find("\"kind\":\"proposal\"") != std::string::npos,
                        "milestone E foundation sweep JSON projection exposes proposal trace history");
                    t.expect(text.find("\"candidate_outcomes\":{") != std::string::npos &&
                            text.find("\"candidate_rankability\":{") != std::string::npos &&
                            text.find("\"result_diversity\":{") != std::string::npos,
                        "milestone B sweep JSON projection exposes candidate diagnostics and diversity summary");
                    t.expect(text.find("\"search_geometry\":{") != std::string::npos &&
                            text.find("\"source\":\"retained_candidate_pool\"") != std::string::npos &&
                            text.find("\"basins\":[") != std::string::npos &&
                            text.find("\"local_robustness\":[") != std::string::npos &&
                            text.find("\"best_params\":{") != std::string::npos &&
                            text.find("\"score_range\":") != std::string::npos,
                        "milestone C sweep JSON projection exposes search-space geometry");
                    t.expect(text.find("\"candidate_space_truncated\":") != std::string::npos &&
                            text.find("\"compile_failures\":") != std::string::npos &&
                            text.find("\"execution_failures\":") != std::string::npos,
                        "milestone B sweep JSON projection exposes execution accounting fields");
                }
                {
                    RunProvenanceSummary large_topology_provenance{};
                    large_topology_provenance.primary_series_key = "A";
                    large_topology_provenance.primary_selection_mode = "explicit_series";
                    large_topology_provenance.has_time_axis = true;
                    for (std::size_t i = 0;
                         i < kRunTopologyDiagnosticsTopologyLimit + 2;
                         ++i) {
                        RunTopologySummary topology_summary{};
                        topology_summary.name = "topology." + std::to_string(i);
                        topology_summary.family = "relational_graph";
                        topology_summary.bearing = "query_bearing";
                        topology_summary.domain_scope = "row_domain";
                        topology_summary.query_kinds = { "neighbor_set" };
                        topology_summary.node_count = 10 + i;
                        topology_summary.edge_count = 20 + i;
                        large_topology_provenance.topology_names.push_back(topology_summary.name);
                        large_topology_provenance.topologies.push_back(std::move(topology_summary));
                    }
                    for (std::size_t i = 0;
                         i < kRunTopologyDiagnosticsUsageLimit + 3;
                         ++i) {
                        RunTopologyUsageSummary usage{};
                        usage.subject_kind = "numeric_node";
                        usage.subject_id = "usage." + std::to_string(i);
                        usage.op_name = "topology.neighbor_mean";
                        usage.topology_name = "topology.0";
                        usage.topology_origin = "layout";
                        usage.topology_bearing = "query_bearing";
                        usage.required_topology_query_kinds = { "neighbor_set" };
                        usage.catalog_present = true;
                        usage.runtime_resolved = true;
                        large_topology_provenance.topology_usages.push_back(std::move(usage));
                    }
                    large_topology_provenance.topology_usage_count =
                        large_topology_provenance.topology_usages.size();

                    RunReport canceled_report{};
                    canceled_report.outcome = RunOutcomeState::Canceled;
                    canceled_report.provenance = large_topology_provenance;

                    std::ostringstream topology_text;
                    print_evaluation_report(
                        make_run_evaluation_report(canceled_report),
                        EvaluationReportProjection::TopologyDiagnostics,
                        topology_text);
                    const std::string text = topology_text.str();
                    t.expect(text.find("Topology diagnostics:") != std::string::npos &&
                            text.find("declared_topologies_reported: 32") != std::string::npos &&
                            text.find("declared_topologies_suppressed: 2") != std::string::npos &&
                            text.find("topology_usages_reported: 64") != std::string::npos &&
                            text.find("topology_usages_suppressed: 3") != std::string::npos,
                        "topology diagnostics text reports explicit large-report capping");
                    t.expect(text.find("topology.31") != std::string::npos &&
                            text.find("topology.32") == std::string::npos &&
                            text.find("usage.63") != std::string::npos &&
                            text.find("usage.64") == std::string::npos,
                        "topology diagnostics text keeps capped entries out of the focused report");

                    std::ostringstream json;
                    print_evaluation_report(
                        make_run_evaluation_report(canceled_report),
                        EvaluationReportProjection::Json,
                        json);
                    const std::string json_text = json.str();
                    t.expect(json_text.find("\"outcome\":\"canceled\"") != std::string::npos &&
                            json_text.find("\"topologies_reported_count\":32") != std::string::npos &&
                            json_text.find("\"topologies_suppressed_count\":2") != std::string::npos &&
                            json_text.find("\"topology_usages_reported_count\":64") != std::string::npos &&
                            json_text.find("\"topology_usages_suppressed_count\":3") != std::string::npos,
                        "topology diagnostics JSON is cancellation-safe and reports capping counts");
                    t.expect(json_text.find("\"name\":\"topology.31\"") != std::string::npos &&
                            json_text.find("\"name\":\"topology.32\"") == std::string::npos &&
                            json_text.find("\"subject_id\":\"usage.63\"") != std::string::npos &&
                            json_text.find("\"subject_id\":\"usage.64\"") == std::string::npos,
                        "topology diagnostics JSON caps large topology arrays deterministically");
                }
                {
                    auto make_candidate = [](std::size_t rank, std::uint64_t trade_hash) {
                        BacktestResult result{};
                        result.trade_hash = trade_hash;
                        result.trades = 1;
                        SweepCandidateReport candidate{};
                        candidate.rank = rank;
                        candidate.candidate_index = rank + 100;
                        candidate.run = build_run_report(result);
                        return candidate;
                    };

                    SweepReport duplicate_report{};
                    duplicate_report.candidate_reports = {
                        make_candidate(1, 777),
                        make_candidate(2, 888),
                        make_candidate(3, 777)
                    };
                    std::ostringstream duplicates;
                    print_evaluation_report(
                        make_sweep_evaluation_report(duplicate_report),
                        EvaluationReportProjection::DuplicateTradePaths,
                        duplicates);
                    t.expect(duplicates.str().find("trade_hash=777 count=2 ranks=1,3") != std::string::npos,
                        "milestone B duplicate trade-path projection groups returned candidates by trade hash");
                }
                const bool progress_has_live_leader = std::any_of(
                    progress_log.begin(),
                    progress_log.end(),
                    [](const SweepProgress& progress) {
                        return progress.stage == SweepProgressStage::CandidateEvaluation &&
                            !progress.current_best.empty() &&
                            !progress.current_best.front().parameter_values.empty();
                    });
                t.expect(progress_has_live_leader,
                    "phase 6 sweep progress exposes a live ranked leader snapshot with parameter values");
            }

            PlanSpec monte_carlo_spec{};
            monte_carlo_spec.version = 1;
            monte_carlo_spec.name = "contract_phase6_monte_carlo_budget";
            monte_carlo_spec.trade_mode = TradeMode::LongOnly;
            monte_carlo_spec.params["alpha"] = static_cast<std::int64_t>(1);
            monte_carlo_spec.numeric_nodes = {
                NodeSpec{ "zero", "core.const", {}, ParamMap{ {"value", 0.0} } },
                NodeSpec{ "one", "core.const", {}, ParamMap{ {"value", 1.0} } },
                NodeSpec{ "score_value", "core.const", {}, ParamMap{ {"value", ParamRef{ "alpha" }} } }
            };
            monte_carlo_spec.bool_nodes = {
                NodeSpec{ "entry", "cmp.gt", { "one", "zero" }, {} }
            };
            monte_carlo_spec.bind.entry_long = "entry";

            PlanSweepSpec monte_carlo_cfg{};
            monte_carlo_cfg.enabled = true;
            monte_carlo_cfg.engine = PlanSweepEngineKind::MonteCarlo;
            monte_carlo_cfg.top_k = 2;
            monte_carlo_cfg.max_evals = 3;
            monte_carlo_cfg.monte_carlo.draws = 10;
            monte_carlo_cfg.monte_carlo.seed = 12345;
            monte_carlo_cfg.monte_carlo.dedupe = true;
            monte_carlo_cfg.objective_spec = RankingObjectiveSpec{};
            monte_carlo_cfg.objective_spec->primary.source = ObjectiveValueSource::NumericNodeOutput;
            monte_carlo_cfg.objective_spec->primary.name = "score_value";
            monte_carlo_cfg.objective_spec->direction = ObjectiveDirection::Maximize;
            SweepParamRange monte_carlo_alpha_range{};
            monte_carlo_alpha_range.min = 1.0;
            monte_carlo_alpha_range.max = 10.0;
            monte_carlo_alpha_range.step = 1.0;
            monte_carlo_alpha_range.is_int = true;
            monte_carlo_cfg.params["alpha"] = monte_carlo_alpha_range;
            monte_carlo_spec.sweep = monte_carlo_cfg;

            std::vector<SweepProgress> monte_carlo_progress_log;
            PlanSweepRuntimeOptions monte_carlo_opt{};
            monte_carlo_opt.progress_cb = [&](const SweepProgress& progress) {
                monte_carlo_progress_log.push_back(progress);
            };

            std::ostringstream monte_carlo_log;
            BacktestResult monte_carlo_best{};
            PlanSweepReport monte_carlo_report{};
            const bool monte_carlo_ok = run_custom_plan_spec(
                dv,
                primary,
                monte_carlo_spec,
                monte_carlo_opt,
                monte_carlo_log,
                &monte_carlo_best,
                &monte_carlo_report);
            t.expect(monte_carlo_ok, "phase 6 monte carlo objective sweep executes through custom runtime");
            if (monte_carlo_ok) {
                const bool saw_monte_carlo_progress = std::any_of(
                    monte_carlo_progress_log.begin(),
                    monte_carlo_progress_log.end(),
                    [](const SweepProgress& progress) {
                        return progress.stage == SweepProgressStage::MonteCarloSampling &&
                            progress.stage_done == 3 &&
                            progress.stage_total == 3;
                    });
                t.expect(saw_monte_carlo_progress,
                    "phase 6 progress stream reports effective monte carlo draws after max_evals capping");
                t.expect(monte_carlo_report.candidate_space_size == 10 && !monte_carlo_report.candidate_space_truncated,
                    "phase 6 structured sweep report preserves the full monte carlo search space");
                t.expect(monte_carlo_report.evaluations_run == 3,
                    "phase 6 structured sweep report preserves capped monte carlo evaluation count");
                t.expect(monte_carlo_report.proposal_trace.captured &&
                        monte_carlo_report.proposal_trace.policy_name == "monte_carlo" &&
                        monte_carlo_report.proposal_trace.proposal_count == 3 &&
                        monte_carlo_report.proposal_trace.observation_count == 3 &&
                        monte_carlo_report.proposal_trace.seeds.size() == 1 &&
                        monte_carlo_report.proposal_trace.seeds.front().source ==
                            "monte_carlo" &&
                        monte_carlo_report.proposal_trace.seeds.front().stream_name ==
                            "candidate_proposal" &&
                        monte_carlo_report.proposal_trace.seeds.front().requested_seed_explicit &&
                        monte_carlo_report.proposal_trace.seeds.front().requested_seed == 12345 &&
                        monte_carlo_report.proposal_trace.seeds.front().effective_seed == 12345,
                    "milestone E foundation records Monte Carlo seed and proposal trace");
                {
                    std::ostringstream json;
                    print_evaluation_report(
                        make_sweep_evaluation_report(monte_carlo_report),
                        EvaluationReportProjection::Json,
                        json);
                    const std::string text = json.str();
                    t.expect(
                        text.find("\"seeds\":[") != std::string::npos &&
                            text.find("\"source\":\"monte_carlo\"") != std::string::npos &&
                            text.find("\"stream_name\":\"candidate_proposal\"") !=
                                std::string::npos &&
                            text.find("\"requested_seed\":12345") !=
                                std::string::npos &&
                            text.find("\"effective_seed\":12345") !=
                                std::string::npos,
                        "milestone E2 JSON projection exposes named Monte Carlo random-stream seed metadata");
                    t.expect(
                        monte_carlo_log.str().find(
                            "monte_carlo stream=candidate_proposal") !=
                            std::string::npos,
                        "milestone E2 full text projection exposes named Monte Carlo random-stream seed metadata");
                }
                t.expect(monte_carlo_report.proposal_trace.events.size() >= 2 &&
                        monte_carlo_report.proposal_trace.events[0].stage ==
                            SweepProgressStage::MonteCarloSampling &&
                        monte_carlo_report.proposal_trace.events[0].proposal_order == 1 &&
                        monte_carlo_report.proposal_trace.events[1].kind ==
                            SweepTraceEventKind::Observation &&
                        monte_carlo_report.proposal_trace.events[1].run_outcome_present &&
                        monte_carlo_report.proposal_trace.events[1].run_outcome ==
                            RunOutcomeState::Completed &&
                        monte_carlo_report.proposal_trace.events[1].run_rankability_present &&
                        monte_carlo_report.proposal_trace.events[1].run_rankability ==
                            RunRankabilityState::Rankable &&
                        monte_carlo_report.proposal_trace.observation_payload_complete,
                    "milestone E foundation preserves Monte Carlo proposal order and observation payloads");
                const bool has_full_alpha_range =
                    monte_carlo_report.parameter_ranges.size() == 1 &&
                    monte_carlo_report.parameter_ranges.front().name == "alpha" &&
                    monte_carlo_report.parameter_ranges.front().expanded_count == 10;
                t.expect(has_full_alpha_range,
                    "phase 6 structured sweep report preserves full declared range expansion under monte carlo");
                const bool report_has_monte_carlo_stage = std::any_of(
                    monte_carlo_report.stage_summaries.begin(),
                    monte_carlo_report.stage_summaries.end(),
                    [](const SweepStageSummary& stage) {
                        return stage.stage == SweepProgressStage::MonteCarloSampling &&
                            stage.done == 3 &&
                            stage.total == 3;
                    });
                t.expect(report_has_monte_carlo_stage,
                    "phase 6 structured sweep report preserves effective monte carlo draw totals");
            }

            const std::string default_draws_json = R"JSON(
{
  "version": 1,
  "name": "contract_monte_carlo_default_draws_follow_max_evals",
  "trade_mode": "LongOnly",
  "params": { "alpha": 1 },
  "numeric_nodes": [
    { "id": "zero", "op": "core.const", "params": { "value": 0.0 } },
    { "id": "one", "op": "core.const", "params": { "value": 1.0 } },
    { "id": "score_value", "op": "core.const", "params": { "value": { "ref": "alpha" } } }
  ],
  "bool_nodes": [
    { "id": "entry", "op": "cmp.gt", "args": ["one", "zero"] }
  ],
  "bindings": { "entry_long": "entry", "exit_long": "", "entry_short": "", "exit_short": "", "allow_trade": "" },
  "sweep": {
    "enabled": true,
    "engine": "monte_carlo",
    "top_k": 2,
    "max_evals": 7,
    "objective": { "value": { "source": "numeric_node_output", "name": "score_value" }, "direction": "maximize" },
    "params": {
      "alpha": { "min": 1, "max": 10, "step": 1, "is_int": true }
    }
  }
}
)JSON";
            PlanSpec default_draws_spec{};
            std::string default_draws_err{};
            const bool default_draws_parsed =
                parse_plan_spec_json(default_draws_json, default_draws_spec, default_draws_err);
            t.expect(default_draws_parsed,
                "PlanSpec parser accepts monte_carlo sweep with omitted draws");
            if (default_draws_parsed) {
                t.expect(default_draws_spec.sweep.has_value() &&
                        default_draws_spec.sweep->monte_carlo.draws == 7 &&
                        !default_draws_spec.sweep->monte_carlo.draws_explicit,
                    "omitted monte_carlo draws defaults to the user-facing max_evals budget");

                std::ostringstream default_draws_log;
                BacktestResult default_draws_best{};
                PlanSweepReport default_draws_report{};
                const bool default_draws_ok = run_custom_plan_spec(
                    dv,
                    primary,
                    default_draws_spec,
                    PlanSweepRuntimeOptions{},
                    default_draws_log,
                    &default_draws_best,
                    &default_draws_report);
                t.expect(default_draws_ok,
                    "monte_carlo sweep with omitted draws executes through custom runtime");
                if (default_draws_ok) {
                    t.expect(default_draws_report.evaluations_run == 7,
                        "omitted monte_carlo draws runs max_evals evaluations instead of the old 10000 default ceiling");
                    t.expect(default_draws_report.requested_sweep.monte_carlo.draws == 7,
                        "structured monte_carlo report shows the effective default draw count");
                }
            }

            const std::string coarse_multiplier_json = R"JSON(
{
  "version": 1,
  "name": "contract_coarse_step_multiplier_budget",
  "trade_mode": "LongOnly",
  "params": { "alpha": 1, "beta": 1, "gamma": 1 },
  "numeric_nodes": [
    { "id": "zero", "op": "core.const", "params": { "value": 0.0 } },
    { "id": "one", "op": "core.const", "params": { "value": 1.0 } },
    { "id": "score_value", "op": "core.const", "params": { "value": { "ref": "alpha" } } }
  ],
  "bool_nodes": [
    { "id": "entry", "op": "cmp.gt", "args": ["one", "zero"] }
  ],
  "bindings": { "entry_long": "entry", "exit_long": "", "entry_short": "", "exit_short": "", "allow_trade": "" },
  "sweep": {
    "enabled": true,
    "engine": "coarse_fine",
    "top_k": 2,
    "max_evals": 40,
    "objective": { "value": { "source": "numeric_node_output", "name": "score_value" }, "direction": "maximize" },
    "params": {
      "alpha": { "min": 1, "max": 4, "step": 1, "is_int": true },
      "beta": { "min": 1, "max": 4, "step": 1, "is_int": true },
      "gamma": { "min": 1, "max": 4, "step": 1, "is_int": true }
    },
    "engine_config": {
      "coarse_fine": {
        "k_stage1": 4,
        "k_stage2": 2,
        "coarse_step_multiplier": 2,
        "depollute_stage1": false,
        "dedupe_stage2": true
      }
    }
  }
}
)JSON";
            PlanSpec coarse_multiplier_spec{};
            std::string coarse_multiplier_err{};
            const bool coarse_multiplier_parsed =
                parse_plan_spec_json(coarse_multiplier_json, coarse_multiplier_spec, coarse_multiplier_err);
            t.expect(coarse_multiplier_parsed,
                "PlanSpec parser accepts coarse_step_multiplier for coarse_fine sweeps");
            if (coarse_multiplier_parsed) {
                std::vector<SweepProgress> coarse_multiplier_progress;
                PlanSweepRuntimeOptions coarse_multiplier_opt{};
                coarse_multiplier_opt.progress_cb = [&](const SweepProgress& progress) {
                    coarse_multiplier_progress.push_back(progress);
                };

                std::ostringstream coarse_multiplier_log;
                BacktestResult coarse_multiplier_best{};
                PlanSweepReport coarse_multiplier_report{};
                const bool coarse_multiplier_ok = run_custom_plan_spec(
                    dv,
                    primary,
                    coarse_multiplier_spec,
                    coarse_multiplier_opt,
                    coarse_multiplier_log,
                    &coarse_multiplier_best,
                    &coarse_multiplier_report);
                t.expect(coarse_multiplier_ok,
                    "coarse_fine sweep with coarse_step_multiplier executes through custom runtime");
                if (coarse_multiplier_ok) {
                    t.expect(coarse_multiplier_report.requested_sweep.coarse_fine.coarse_step_multiplier == 2,
                        "structured coarse_fine report preserves the configured coarse_step_multiplier");
                    t.expect(coarse_multiplier_report.evaluations_run == 40,
                        "coarse_fine sweep uses the remaining max_evals budget for local refinement");
                    t.expect(coarse_multiplier_report.requested_sweep.coarse_fine.stage2_hops > 1,
                        "coarse_fine sweep derives a wider refinement radius when the default would underuse max_evals");
                    const bool coarse_stage_was_strided = std::any_of(
                        coarse_multiplier_report.stage_summaries.begin(),
                        coarse_multiplier_report.stage_summaries.end(),
                        [](const SweepStageSummary& stage) {
                            return stage.stage == SweepProgressStage::CoarseSearch &&
                                stage.done == 8 &&
                                stage.total == 8;
                        });
                    t.expect(coarse_stage_was_strided,
                        "coarse_step_multiplier samples every Nth value per dimension instead of scanning the full fine grid");
                    t.expect(coarse_multiplier_report.proposal_trace.captured &&
                            coarse_multiplier_report.proposal_trace.policy_name == "coarse_fine" &&
                            coarse_multiplier_report.proposal_trace.proposal_count ==
                                coarse_multiplier_report.evaluations_run &&
                            coarse_multiplier_report.proposal_trace.observation_count ==
                                coarse_multiplier_report.evaluations_run &&
                            coarse_multiplier_report.proposal_trace.promotion_count > 0,
                        "milestone E foundation records coarse/fine proposals, observations, and stage promotions");
                    const bool has_parented_refinement_event = std::any_of(
                        coarse_multiplier_report.proposal_trace.events.begin(),
                        coarse_multiplier_report.proposal_trace.events.end(),
                        [](const SweepTraceEvent& event) {
                            return event.stage == SweepProgressStage::LocalRefinement &&
                                event.kind == SweepTraceEventKind::Proposal &&
                                event.has_parent_candidate;
                        });
                    t.expect(has_parented_refinement_event,
                        "milestone E foundation records local-refinement proposal parent seeds");
                    t.expect(coarse_multiplier_log.str().find("coarse step multiplier: 2") != std::string::npos,
                        "coarse_fine sweep output makes the effective coarse multiplier visible");
                }
            }

            const std::string successive_halving_json = R"JSON(
{
  "version": 1,
  "name": "contract_successive_halving_adaptive_trace",
  "trade_mode": "LongOnly",
  "params": { "alpha": 1, "beta": 1 },
  "numeric_nodes": [
    { "id": "zero", "op": "core.const", "params": { "value": 0.0 } },
    { "id": "one", "op": "core.const", "params": { "value": 1.0 } },
    { "id": "score_value", "op": "core.const", "params": { "value": { "ref": "alpha" } } }
  ],
  "bool_nodes": [
    { "id": "entry", "op": "cmp.gt", "args": ["one", "zero"] }
  ],
  "bindings": { "entry_long": "entry", "exit_long": "", "entry_short": "", "exit_short": "", "allow_trade": "" },
  "sweep": {
    "enabled": true,
    "engine": "successive_halving",
    "top_k": 2,
    "max_evals": 16,
    "objective": { "value": { "source": "numeric_node_output", "name": "score_value" }, "direction": "maximize" },
    "params": {
      "alpha": { "min": 1, "max": 4, "step": 1, "is_int": true },
      "beta": { "min": 1, "max": 4, "step": 1, "is_int": true }
    },
    "engine_config": {
      "successive_halving": {
        "rounds": 3,
        "reduction_factor": 2,
        "initial_candidates": 8,
        "neighborhood_hops": 1,
        "dedupe_candidates": true
      }
    }
  }
}
)JSON";
            PlanSpec successive_halving_spec{};
            std::string successive_halving_err{};
            const bool successive_halving_parsed =
                parse_plan_spec_json(
                    successive_halving_json,
                    successive_halving_spec,
                    successive_halving_err);
            t.expect(successive_halving_parsed,
                "milestone E PlanSpec parser accepts successive_halving sweeps");
            if (successive_halving_parsed) {
                std::vector<SweepProgress> successive_halving_progress;
                PlanSweepRuntimeOptions successive_halving_opt{};
                successive_halving_opt.progress_cb = [&](const SweepProgress& progress) {
                    successive_halving_progress.push_back(progress);
                };

                std::ostringstream successive_halving_log;
                BacktestResult successive_halving_best{};
                PlanSweepReport successive_halving_report{};
                const bool successive_halving_ok = run_custom_plan_spec(
                    dv,
                    primary,
                    successive_halving_spec,
                    successive_halving_opt,
                    successive_halving_log,
                    &successive_halving_best,
                    &successive_halving_report);
                t.expect(successive_halving_ok,
                    "milestone E successive_halving sweep executes through custom runtime");
                if (successive_halving_ok) {
                    t.expect(successive_halving_report.requested_sweep.engine ==
                            PlanSweepEngineKind::SuccessiveHalving &&
                            successive_halving_report.requested_sweep.successive_halving.rounds == 3 &&
                            successive_halving_report.requested_sweep.successive_halving.reduction_factor == 2 &&
                            successive_halving_report.requested_sweep.successive_halving.initial_candidates == 8,
                        "milestone E successive_halving report preserves configured adaptive knobs");
                    t.expect(successive_halving_report.evaluations_run == 16,
                        "milestone E successive_halving uses max_evals as the adaptive evaluation budget");
                    const bool saw_adaptive_progress = std::any_of(
                        successive_halving_progress.begin(),
                        successive_halving_progress.end(),
                        [](const SweepProgress& progress) {
                            return progress.stage == SweepProgressStage::AdaptiveSelection;
                        });
                    t.expect(saw_adaptive_progress,
                        "milestone E successive_halving progress distinguishes adaptive selection");
                    const bool has_adaptive_stage_summary = std::any_of(
                        successive_halving_report.stage_summaries.begin(),
                        successive_halving_report.stage_summaries.end(),
                        [](const SweepStageSummary& stage) {
                            return stage.stage == SweepProgressStage::AdaptiveSelection &&
                                stage.done == 16 &&
                                stage.total == 16;
                        });
                    t.expect(has_adaptive_stage_summary,
                        "milestone E successive_halving report records adaptive stage completion");
                    t.expect(successive_halving_report.proposal_trace.captured &&
                            successive_halving_report.proposal_trace.policy_name == "successive_halving" &&
                            successive_halving_report.proposal_trace.proposal_count ==
                                successive_halving_report.evaluations_run &&
                            successive_halving_report.proposal_trace.observation_count ==
                                successive_halving_report.evaluations_run &&
                            successive_halving_report.proposal_trace.promotion_count > 0 &&
                            successive_halving_report.proposal_trace.rejection_count > 0,
                        "milestone E successive_halving records proposals, observations, promotions, and rejections");
                    const bool has_parented_adaptive_proposal = std::any_of(
                        successive_halving_report.proposal_trace.events.begin(),
                        successive_halving_report.proposal_trace.events.end(),
                        [](const SweepTraceEvent& event) {
                            return event.kind == SweepTraceEventKind::Proposal &&
                                event.stage == SweepProgressStage::AdaptiveSelection &&
                                event.has_parent_candidate;
                        });
                    t.expect(has_parented_adaptive_proposal,
                        "milestone E successive_halving links later proposals to promoted parent seeds");
                    t.expect(successive_halving_report.top_candidates.size() == 2 &&
                            successive_halving_report.top_candidates.front().score == 4.0,
                        "milestone E successive_halving returns the best observed adaptive candidates");
                    t.expect(successive_halving_log.str().find("Engine: successive_halving") != std::string::npos &&
                            successive_halving_log.str().find("rejections:") != std::string::npos,
                        "milestone E successive_halving full report exposes adaptive trace counts");
                }
            }

            const std::string trade_hash_diversity_json = R"JSON(
{
  "version": 1,
  "name": "contract_trade_hash_result_diversity",
  "trade_mode": "LongOnly",
  "params": { "alpha": 1, "beta": 1 },
  "numeric_nodes": [
    { "id": "zero", "op": "core.const", "params": { "value": 0.0 } },
    { "id": "one", "op": "core.const", "params": { "value": 1.0 } },
    { "id": "score_value", "op": "core.const", "params": { "value": { "ref": "alpha" } } }
  ],
  "bool_nodes": [
    { "id": "entry", "op": "cmp.gt", "args": ["one", "zero"] }
  ],
  "bindings": { "entry_long": "entry", "exit_long": "", "entry_short": "", "exit_short": "", "allow_trade": "" },
  "sweep": {
    "enabled": true,
    "top_k": 3,
    "max_evals": 16,
    "objective": { "value": { "source": "numeric_node_output", "name": "score_value" }, "direction": "maximize" },
    "result_diversity": {
      "dedupe_trade_hash": true,
      "oversample_multiplier": 8,
      "min_trades": 1
    },
    "params": {
      "alpha": { "min": 1, "max": 4, "step": 1, "is_int": true },
      "beta": { "min": 1, "max": 4, "step": 1, "is_int": true }
    }
  }
}
)JSON";
            PlanSpec trade_hash_diversity_spec{};
            std::string trade_hash_diversity_err{};
            const bool trade_hash_diversity_parsed =
                parse_plan_spec_json(
                    trade_hash_diversity_json,
                    trade_hash_diversity_spec,
                    trade_hash_diversity_err);
            t.expect(trade_hash_diversity_parsed,
                "PlanSpec parser accepts final result-diversity controls");
            if (trade_hash_diversity_parsed) {
                std::ostringstream diversity_log;
                BacktestResult diversity_best{};
                PlanSweepReport diversity_report{};
                const bool diversity_ok = run_custom_plan_spec(
                    dv,
                    primary,
                    trade_hash_diversity_spec,
                    PlanSweepRuntimeOptions{},
                    diversity_log,
                    &diversity_best,
                    &diversity_report);
                t.expect(diversity_ok,
                    "trade-hash result-diversity sweep executes through custom runtime");
                if (diversity_ok) {
                    t.expect(diversity_report.result_diversity.requested &&
                            diversity_report.result_diversity.dedupe_trade_hash,
                        "structured sweep report records requested trade-hash result diversity");
                    t.expect(diversity_report.result_diversity.retained_candidate_pool > trade_hash_diversity_spec.sweep->top_k,
                        "result diversity retains an oversampled candidate pool before final Top-K filtering");
                    t.expect(diversity_report.top_candidates.size() == 1 &&
                            diversity_report.result_diversity.returned_unique_trade_hashes == 1,
                        "trade-hash result diversity collapses repeated final outcomes to one representative");
                    t.expect(diversity_report.result_diversity.suppressed_duplicate_trade_hash > 0,
                        "structured sweep report counts suppressed duplicate trade-hash candidates");
                    t.expect(diversity_report.search_geometry.retained_candidate_pool ==
                            diversity_report.result_diversity.retained_candidate_pool,
                        "milestone C search geometry records the retained candidate pool size");
                    t.expect(diversity_report.search_geometry.duplicate_trade_paths.size() == 1 &&
                            diversity_report.search_geometry.duplicate_trade_paths.front().candidate_count ==
                                diversity_report.search_geometry.retained_candidate_pool &&
                            diversity_report.search_geometry.duplicate_trade_paths.front().returned_candidate_count == 1,
                        "milestone C search geometry groups duplicate trade paths across the retained pool");
                    t.expect(diversity_log.str().find("dedupe trade hash: true") != std::string::npos,
                        "sweep report makes trade-hash result diversity visible");
                    {
                        std::ostringstream duplicates;
                        print_evaluation_report(
                            make_sweep_evaluation_report(diversity_report),
                            EvaluationReportProjection::DuplicateTradePaths,
                            duplicates);
                        const std::string duplicate_text = duplicates.str();
                        t.expect(duplicate_text.find("returned=1") != std::string::npos &&
                                duplicate_text.find("best_candidate_index=") != std::string::npos,
                            "milestone C duplicate-path projection uses retained-pool geometry facts");
                    }
                }
            }

            const std::string basin_diversity_json = R"JSON(
{
  "version": 1,
  "name": "contract_basin_result_diversity",
  "trade_mode": "LongOnly",
  "params": { "alpha": 1, "beta": 1 },
  "numeric_nodes": [
    { "id": "zero", "op": "core.const", "params": { "value": 0.0 } },
    { "id": "one", "op": "core.const", "params": { "value": 1.0 } },
    { "id": "score_value", "op": "core.const", "params": { "value": { "ref": "alpha" } } }
  ],
  "bool_nodes": [
    { "id": "entry", "op": "cmp.gt", "args": ["one", "zero"] }
  ],
  "bindings": { "entry_long": "entry", "exit_long": "", "entry_short": "", "exit_short": "", "allow_trade": "" },
  "sweep": {
    "enabled": true,
    "top_k": 4,
    "max_evals": 16,
    "objective": { "value": { "source": "numeric_node_output", "name": "score_value" }, "direction": "maximize" },
    "result_diversity": {
      "dedupe_trade_hash": false,
      "basin_aware": true,
      "oversample_multiplier": 8,
      "max_per_basin": 1,
      "param_bucket_size": 2
    },
    "params": {
      "alpha": { "min": 1, "max": 4, "step": 1, "is_int": true },
      "beta": { "min": 1, "max": 4, "step": 1, "is_int": true }
    }
  }
}
)JSON";
            PlanSpec basin_diversity_spec{};
            std::string basin_diversity_err{};
            const bool basin_diversity_parsed =
                parse_plan_spec_json(
                    basin_diversity_json,
                    basin_diversity_spec,
                    basin_diversity_err);
            t.expect(basin_diversity_parsed,
                "PlanSpec parser accepts basin-aware result-diversity controls");
            if (basin_diversity_parsed) {
                std::ostringstream basin_log;
                BacktestResult basin_best{};
                PlanSweepReport basin_report{};
                const bool basin_ok = run_custom_plan_spec(
                    dv,
                    primary,
                    basin_diversity_spec,
                    PlanSweepRuntimeOptions{},
                    basin_log,
                    &basin_best,
                    &basin_report);
                t.expect(basin_ok,
                    "basin-aware result-diversity sweep executes through custom runtime");
                if (basin_ok) {
                    t.expect(basin_report.result_diversity.requested &&
                            basin_report.result_diversity.basin_aware &&
                            !basin_report.result_diversity.dedupe_trade_hash,
                        "structured sweep report records basin-aware result diversity separately from trade-hash dedupe");
                    t.expect(basin_report.result_diversity.returned_basin_count == basin_report.top_candidates.size() &&
                            basin_report.top_candidates.size() == 4,
                        "basin-aware result diversity returns at most one candidate per bucketed parameter basin");
                    t.expect(basin_report.result_diversity.suppressed_basin > 0,
                        "structured sweep report counts candidates suppressed by basin diversity");
                    t.expect(basin_report.search_geometry.source == SweepSearchGeometrySource::RetainedCandidatePool &&
                            basin_report.search_geometry.basin_bucket_size == 2 &&
                            basin_report.search_geometry.basins.size() == 4,
                        "milestone C search geometry summarizes deterministic parameter-space basins");
                    const bool basin_counts_are_stable = std::all_of(
                        basin_report.search_geometry.basins.begin(),
                        basin_report.search_geometry.basins.end(),
                        [](const SweepBasinSummary& basin) {
                            return basin.candidate_count == 4 &&
                                basin.returned_candidate_count == 1 &&
                                nearly_equal(basin.retained_pool_density, 0.25);
                        });
                    t.expect(basin_counts_are_stable,
                        "milestone C basin summaries preserve basin size and retained-pool density");
                    const bool basin_best_params_are_stable = std::all_of(
                        basin_report.search_geometry.basins.begin(),
                        basin_report.search_geometry.basins.end(),
                        [](const SweepBasinSummary& basin) {
                            return !basin.best_parameter_values.empty() &&
                                basin.mean_score <= basin.best_score &&
                                basin.worst_score <= basin.mean_score &&
                                basin.score_range >= 0.0;
                        });
                    t.expect(basin_best_params_are_stable,
                        "milestone C basin summaries preserve best params and score spread");
                    t.expect(basin_report.search_geometry.local_robustness.size() ==
                            basin_report.top_candidates.size() &&
                            !basin_report.search_geometry.local_robustness.empty() &&
                            basin_report.search_geometry.local_robustness.front().retained_neighbor_count > 0,
                        "milestone C search geometry records local robustness around retained winners");
                    t.expect(basin_log.str().find("basin aware: true") != std::string::npos,
                        "sweep report makes basin-aware result diversity visible");
                    {
                        std::ostringstream diversity;
                        print_evaluation_report(
                            make_sweep_evaluation_report(basin_report),
                            EvaluationReportProjection::DiversityDiagnostics,
                            diversity);
                        const std::string diversity_text = diversity.str();
                        t.expect(diversity_text.find("Search geometry:") != std::string::npos &&
                                diversity_text.find("Top basin summaries") != std::string::npos &&
                                diversity_text.find("Local robustness around returned winners:") != std::string::npos,
                            "milestone C diversity projection prints basin and local-robustness geometry");
                    }
                }
            }
        }

        return true;
    }

} // namespace sugar::contract_suite_internal
