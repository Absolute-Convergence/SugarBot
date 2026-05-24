#include "evaluation_report_internal.h"

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <map>
#include <ostream>
#include <sstream>
#include <string_view>
#include <vector>

namespace sugar::evaluation_report_internal {

    namespace {
        void print_param_list(
            const std::vector<SweepParameterSetting>& settings,
            std::ostream& out) {
            for (std::size_t i = 0; i < settings.size(); ++i) {
                if (i != 0) {
                    out << ", ";
                }
                out << settings[i].name << "="
                    << param_value_to_text(settings[i].value);
            }
        }

        void print_size_list(
            const std::vector<std::size_t>& values,
            std::ostream& out) {
            for (std::size_t i = 0; i < values.size(); ++i) {
                if (i != 0) {
                    out << ",";
                }
                out << values[i];
            }
        }

        void print_dynamic_execution_compact(
            const RunDynamicExecutionSummary& dynamic,
            std::ostream& out,
            std::string_view label) {
            if (!dynamic.enabled) {
                return;
            }
            out << "  " << label
                << ": mode=" << dynamic.mode
                << " total_decisions=" << dynamic.total_decisions
                << " fallback_decisions=" << dynamic.fallback_decisions
                << " suppressed_decisions=" << dynamic.suppressed_decisions
                << " inactive_output_rows=" << dynamic.inactive_output_rows
                << " inactive_node_rows=" << dynamic.inactive_node_rows
                << " max_active_subgraphs_per_row=" << dynamic.max_active_subgraphs_per_row
                << " max_active_subgraph_cap=" << dynamic.max_active_subgraph_cap
                << " bounded_by_declared_caps="
                << (dynamic.bounded_by_declared_caps ? "true" : "false")
                << " route_decision_digest=" << dynamic.route_decision_digest
                << "\n";
        }

        void print_run_compact(const RunReport& report, std::ostream& out) {
            out << "Run summary:\n"
                << "  outcome=" << run_outcome_state_name(report.outcome) << "\n"
                << "  execution: pnl=" << report.execution.pnl
                << " trades=" << report.execution.trades
                << " max_drawdown=" << report.execution.max_drawdown
                << " trade_hash=" << report.execution.trade_hash
                << " rows_evaluated=" << report.execution.rows_evaluated
                << " warmup_rows=" << report.execution.warmup_rows << "\n";
            if (report.objective.requested) {
                out << "  objective: rankability="
                    << run_rankability_state_name(report.objective.rankability);
                if (report.objective.rankability == RunRankabilityState::Rankable) {
                    out << " score=" << report.objective.score;
                }
                out << "\n";
            }
            print_dynamic_execution_compact(
                report.execution.dynamic_execution,
                out,
                "dynamic_execution");
        }

        void print_objective_components(
            const RunReport& report,
            std::ostream& out,
            std::string_view indent) {
            if (!report.objective.requested) {
                out << indent << "objective: <not requested>\n";
                return;
            }
            out << indent << "objective: "
                << describe_ranking_objective(report.objective.objective) << "\n"
                << indent << "rankability: "
                << run_rankability_state_name(report.objective.rankability) << "\n";
            if (report.objective.rankability == RunRankabilityState::Rankable) {
                out << indent << "score: " << report.objective.score << "\n";
            }
            out << indent << "components:\n";
            for (const auto& component : report.objective.components) {
                out << indent << "  - role="
                    << run_objective_component_role_name(component.role)
                    << " value=" << objective_value_source_name(component.value.source)
                    << ":" << component.value.name
                    << " present=" << (component.present ? "true" : "false")
                    << " finite=" << (component.finite ? "true" : "false");
                if (component.present && component.finite) {
                    out << " raw=" << component.raw_value
                        << " contribution=" << component.score_contribution;
                }
                if (component.role == RunObjectiveComponentRole::Penalty) {
                    out << " weight=" << component.weight;
                }
                if (component.role != RunObjectiveComponentRole::Penalty) {
                    out << " direction=" << objective_direction_name(component.direction);
                }
                out << "\n";
            }
        }

        void print_sweep_compact(const SweepReport& report, std::ostream& out) {
            const auto candidates = sweep_candidate_reports_for(report);
            out << "Sweep summary:\n"
                << "  engine=" << sweep_engine_name(report.requested_sweep.engine)
                << " objective=" << describe_ranking_objective(report.resolved_objective)
                << "\n"
                << "  outcome: canceled=" << (report.canceled ? "true" : "false")
                << " evals_run=" << report.evaluations_run
                << " compile_failures=" << report.compile_failures
                << " execution_failures=" << report.execution_failures
                << " candidate_space=" << report.candidate_space_size
                << " candidate_space_truncated="
                << (report.candidate_space_truncated ? "true" : "false")
                << " top_k=" << candidates.size()
                << " warnings=" << report.warnings.size() << "\n";
            out << "  candidate_outcomes:"
                << " completed=" << report.candidate_outcomes.completed
                << " completed_with_non_finite_metrics="
                << report.candidate_outcomes.completed_with_non_finite_metrics
                << " failed=" << report.candidate_outcomes.failed
                << " canceled=" << report.candidate_outcomes.canceled << "\n";
            out << "  candidate_rankability:"
                << " rankable=" << report.candidate_rankability.rankable
                << " missing_primary_value="
                << report.candidate_rankability.missing_primary_value
                << " non_finite_primary_value="
                << report.candidate_rankability.non_finite_primary_value
                << " missing_penalty_value="
                << report.candidate_rankability.missing_penalty_value
                << " non_finite_penalty_value="
                << report.candidate_rankability.non_finite_penalty_value << "\n";
            if (!candidates.empty()) {
                const auto& best = candidates.front();
                out << "  best rank=" << best.rank
                    << " candidate_index=" << best.candidate_index
                    << " score=" << best.score
                    << " outcome=" << run_outcome_state_name(best.run.outcome)
                    << " objective_rankability="
                    << run_rankability_state_name(best.run.objective.rankability)
                    << " pnl=" << best.run.execution.pnl
                    << " trades=" << best.run.execution.trades
                    << " max_drawdown=" << best.run.execution.max_drawdown
                    << " trade_hash=" << best.run.execution.trade_hash << "\n"
                    << "  best params: ";
                print_param_list(best.parameter_values, out);
                out << "\n";
                print_dynamic_execution_compact(
                    best.run.execution.dynamic_execution,
                    out,
                    "best_dynamic_execution");
            }
            if (report.proposal_trace.captured) {
                out << "  replay: policy=" << report.proposal_trace.policy_name
                    << " proposal_count=" << report.proposal_trace.proposal_count
                    << " observation_count=" << report.proposal_trace.observation_count
                    << " events_capped="
                    << (report.proposal_trace.events_capped ? "true" : "false")
                    << " exact_replay_from_report="
                    << (report.proposal_trace.exact_replay_from_report ? "true" : "false")
                    << "\n";
            }
            if (report.posterior.requested || report.posterior.computed) {
                out << "  posterior: requested="
                    << (report.posterior.requested ? "true" : "false")
                    << " computed=" << (report.posterior.computed ? "true" : "false")
                    << " status="
                    << sweep_posterior_diagnostics_status_name(report.posterior.status)
                    << " training_observation_count="
                    << report.posterior.training_observation_count
                    << " finite_observation_count="
                    << report.posterior.finite_observation_count
                    << " acquisition_ready="
                    << (report.posterior.acquisition_ready ? "true" : "false")
                    << "\n";
            }
            if (report.result_diversity.requested) {
                out << "  diversity: retained_candidate_pool="
                    << report.result_diversity.retained_candidate_pool
                    << " returned_unique_trade_hashes="
                    << report.result_diversity.returned_unique_trade_hashes
                    << " suppressed_duplicate_trade_hash="
                    << report.result_diversity.suppressed_duplicate_trade_hash
                    << " suppressed_basin="
                    << report.result_diversity.suppressed_basin
                    << " suppressed_min_trades="
                    << report.result_diversity.suppressed_min_trades
                    << "\n";
            }
        }

        void print_sweep_candidate_table(const SweepReport& report, std::ostream& out) {
            const auto candidates = sweep_candidate_reports_for(report);
            out << "Sweep candidate table:\n"
                << "rank\tcandidate_index\tscore\tpnl\tmax_drawdown\ttrades"
                << "\ttrade_hash\tobjective_rankability\tparams\n";
            for (const auto& candidate : candidates) {
                out << candidate.rank << "\t"
                    << candidate.candidate_index << "\t"
                    << candidate.score << "\t"
                    << candidate.run.execution.pnl << "\t"
                    << candidate.run.execution.max_drawdown << "\t"
                    << candidate.run.execution.trades << "\t"
                    << candidate.run.execution.trade_hash << "\t"
                    << run_rankability_state_name(candidate.run.objective.rankability)
                    << "\t";
                print_param_list(candidate.parameter_values, out);
                out << "\n";
            }
        }

        void print_sweep_candidate_table_unavailable(
            std::ostream& out,
            std::string_view report_kind) {
            out << "Sweep candidate table:\n"
                << "  available=false\n"
                << "  report_kind=" << report_kind << "\n"
                << "  reason=not_a_sweep_report\n"
                << "  next_step=enable `sweep.enabled=true` with `sweep.top_k` in the PlanSpec to produce ranked candidate rows, or use `--report compact` for the non-sweep summary\n";
        }

        void print_sweep_objective_breakdown(
            const SweepReport& report,
            std::ostream& out) {
            const auto candidates = sweep_candidate_reports_for(report);
            out << "Sweep objective breakdown:\n";
            if (candidates.empty()) {
                out << "  <no candidates>\n";
                return;
            }
            for (const auto& candidate : candidates) {
                out << "-- Rank " << candidate.rank
                    << " | candidate_index=" << candidate.candidate_index
                    << " | score=" << candidate.score << " --\n";
                print_objective_components(candidate.run, out, "  ");
            }
        }

        void print_sweep_diversity(const SweepReport& report, std::ostream& out) {
            const auto& d = report.result_diversity;
            out << "Sweep diversity diagnostics:\n"
                << "  requested=" << (d.requested ? "true" : "false") << "\n"
                << "  dedupe_trade_hash=" << (d.dedupe_trade_hash ? "true" : "false") << "\n"
                << "  basin_aware=" << (d.basin_aware ? "true" : "false") << "\n"
                << "  oversample_multiplier=" << d.oversample_multiplier << "\n"
                << "  max_per_basin=" << d.max_per_basin << "\n"
                << "  param_bucket_size=" << d.param_bucket_size << "\n"
                << "  min_param_distance=" << d.min_param_distance << "\n"
                << "  min_trades=" << d.min_trades << "\n"
                << "  retained_candidate_pool=" << d.retained_candidate_pool << "\n"
                << "  returned_unique_trade_hashes="
                << d.returned_unique_trade_hashes << "\n"
                << "  returned_duplicate_trade_hash_groups="
                << d.returned_duplicate_trade_hash_groups << "\n"
                << "  returned_largest_trade_hash_group="
                << d.returned_largest_trade_hash_group << "\n"
                << "  returned_basin_count=" << d.returned_basin_count << "\n"
                << "  suppressed_duplicate_trade_hash="
                << d.suppressed_duplicate_trade_hash << "\n"
                << "  suppressed_basin=" << d.suppressed_basin << "\n"
                << "  suppressed_min_trades=" << d.suppressed_min_trades << "\n";
            print_sweep_geometry(report, out);
            if (!report.warnings.empty()) {
                out << "Warnings:\n";
                for (const auto& warning : report.warnings) {
                    out << "  - " << warning << "\n";
                }
            }
        }

        void print_sweep_duplicate_trade_paths(
            const SweepReport& report,
            std::ostream& out) {
            if (!report.search_geometry.duplicate_trade_paths.empty()) {
                out << "Duplicate trade-path groups:\n";
                for (const auto& group : report.search_geometry.duplicate_trade_paths) {
                    out << "  trade_hash=" << group.trade_hash
                        << " count=" << group.candidate_count
                        << " returned=" << group.returned_candidate_count
                        << " best_candidate_index="
                        << group.best_candidate_index
                        << " best_rank=" << group.best_rank
                        << " ranks=";
                    print_size_list(group.ranks, out);
                    out << "\n";
                }
                return;
            }

            const auto candidates = sweep_candidate_reports_for(report);
            std::map<std::uint64_t, std::vector<std::size_t>> ranks_by_trade_hash;
            for (const auto& candidate : candidates) {
                ranks_by_trade_hash[candidate.run.execution.trade_hash].push_back(
                    candidate.rank);
            }

            out << "Duplicate trade-path groups:\n";
            bool printed = false;
            for (const auto& [trade_hash, ranks] : ranks_by_trade_hash) {
                if (ranks.size() < 2) {
                    continue;
                }
                printed = true;
                out << "  trade_hash=" << trade_hash
                    << " count=" << ranks.size()
                    << " ranks=";
                for (std::size_t i = 0; i < ranks.size(); ++i) {
                    if (i != 0) {
                        out << ",";
                    }
                    out << ranks[i];
                }
                out << "\n";
            }
            if (!printed) {
                out << "  <none among returned candidates>\n";
            }
        }

        void print_objective_vector_component(
            const ObjectiveVectorComponentSpec& objective,
            std::ostream& out) {
            out << objective_vector_component_label(objective)
                << " "
                << objective_direction_name(objective.direction)
                << " "
                << objective_value_source_name(objective.value.source)
                << ":" << objective.value.name;
            if (objective.value.source == ObjectiveValueSource::NumericNodeOutput) {
                out << " ["
                    << numeric_node_summary_mode_name(objective.value.summary_mode)
                    << "]";
            }
        }

        void print_pareto_objective_values(
            const SweepParetoSummary& pareto,
            const SweepParetoCandidateSummary& candidate,
            std::ostream& out) {
            for (std::size_t i = 0; i < candidate.objective_values.size(); ++i) {
                if (i != 0) {
                    out << ", ";
                }
                if (i < pareto.objectives.size()) {
                    out << objective_vector_component_label(pareto.objectives[i]) << "=";
                }
                const auto& value = candidate.objective_values[i];
                if (!value.present) {
                    out << "<missing>";
                }
                else if (!value.finite) {
                    out << "<non_finite>";
                }
                else {
                    out << value.value;
                }
            }
        }

        void print_pareto_objective_ranges(
            const std::vector<SweepParetoObjectiveRange>& ranges,
            std::ostream& out) {
            if (ranges.empty()) {
                return;
            }
            out << "  objective_ranges:\n";
            for (std::size_t i = 0; i < ranges.size(); ++i) {
                const auto& range = ranges[i];
                out << "    [" << i << "] " << range.label
                    << " raw_non_dominated=";
                if (range.raw_non_dominated_has_value) {
                    out << "[" << range.raw_non_dominated_min
                        << ", " << range.raw_non_dominated_max
                        << "] range=" << range.raw_non_dominated_range;
                }
                else {
                    out << "<none>";
                }
                out << " emitted_front=";
                if (range.emitted_front_has_value) {
                    out << "[" << range.emitted_front_min
                        << ", " << range.emitted_front_max
                        << "] range=" << range.emitted_front_range;
                }
                else {
                    out << "<none>";
                }
                out << "\n";
            }
        }

        void print_sweep_pareto_front(
            const SweepReport& report,
            std::ostream& out) {
            const auto& p = report.pareto;
            out << "Sweep Pareto front:\n"
                << "  requested=" << (p.requested ? "true" : "false") << "\n"
                << "  computed=" << (p.computed ? "true" : "false") << "\n"
                << "  source=" << sweep_pareto_summary_source_name(p.source) << "\n"
                << "  observed_candidates=" << p.observed_candidate_count << "\n"
                << "  unique_candidates=" << p.unique_candidate_count << "\n"
                << "  duplicate_observations=" << p.duplicate_observation_count << "\n"
                << "  retained_candidate_pool=" << p.retained_candidate_pool << "\n"
                << "  finite_candidates=" << p.finite_candidate_count << "\n"
                << "  incomplete_candidates=" << p.incomplete_candidate_count << "\n"
                << "  dominated_candidates=" << p.dominated_candidate_count << "\n"
                << "  non_dominated_candidates=" << p.non_dominated_candidate_count << "\n"
                << "  emitted_front_rows=" << p.emitted_front_row_count << "\n"
                << "  front_row_limit=" << p.front_row_limit
                << (p.front_row_limit == 0 ? " (uncapped)" : "") << "\n"
                << "  front_rows_capped=" << (p.front_rows_capped ? "true" : "false") << "\n"
                << "  output_depollution_requested="
                << (p.output_depollution_requested ? "true" : "false") << "\n"
                << "  output_depollution_source="
                << sweep_pareto_output_depollution_source_name(
                    p.output_depollution_source) << "\n"
                << "  output_depollution_used_result_diversity="
                << (p.output_depollution_used_result_diversity ? "true" : "false") << "\n"
                << "  emitted_unique_trade_hashes=" << p.emitted_unique_trade_hashes << "\n"
                << "  emitted_duplicate_trade_hash_groups="
                << p.emitted_duplicate_trade_hash_groups << "\n"
                << "  emitted_largest_trade_hash_group="
                << p.emitted_largest_trade_hash_group << "\n"
                << "  emitted_basin_count=" << p.emitted_basin_count << "\n"
                << "  suppressed_duplicate_trade_hash="
                << p.suppressed_duplicate_trade_hash << "\n"
                << "  suppressed_basin=" << p.suppressed_basin << "\n"
                << "  suppressed_min_trades=" << p.suppressed_min_trades << "\n"
                << "  suppressed_row_limit=" << p.suppressed_row_limit << "\n";
            if (p.output_depollution_requested) {
                out << "  output_depollution_dedupe_trade_hash="
                    << (p.output_depollution_dedupe_trade_hash ? "true" : "false") << "\n"
                    << "  output_depollution_basin_aware="
                    << (p.output_depollution_basin_aware ? "true" : "false") << "\n"
                    << "  output_depollution_max_per_basin="
                    << p.output_depollution_max_per_basin << "\n"
                    << "  output_depollution_param_bucket_size="
                    << p.output_depollution_param_bucket_size << "\n"
                    << "  output_depollution_min_param_distance="
                    << p.output_depollution_min_param_distance << "\n"
                    << "  output_depollution_min_trades="
                    << p.output_depollution_min_trades << "\n";
            }
            if (!p.objectives.empty()) {
                out << "  primary_sort_index=" << p.primary_sort_index << "\n"
                    << "  primary_sort_objective=";
                print_objective_vector_component(
                    p.objectives[std::min(p.primary_sort_index, p.objectives.size() - 1)],
                    out);
                out << "\n"
                    << "  objectives:\n";
                for (std::size_t i = 0; i < p.objectives.size(); ++i) {
                    out << "    [" << i << "] ";
                    print_objective_vector_component(p.objectives[i], out);
                    out << "\n";
                }
            }
            print_pareto_objective_ranges(p.objective_ranges, out);
            if (p.front.empty()) {
                out << "  front: <empty>\n";
                return;
            }
            out << "  front:\n";
            for (const auto& candidate : p.front) {
                out << "    rank=" << candidate.front_rank
                    << " candidate_index=" << candidate.candidate_index
                    << " scalar_score=" << candidate.scalar_score
                    << " dominates=" << candidate.dominates_count
                    << " objectives=[";
                print_pareto_objective_values(p, candidate, out);
                out << "] params=";
                print_param_list(candidate.parameter_values, out);
                out << "\n";
            }
        }

        std::string trace_digest_hex(std::uint64_t digest) {
            std::ostringstream text;
            text << "0x" << std::hex << std::setw(16)
                << std::setfill('0') << digest;
            return text.str();
        }

        void print_sweep_replay_trace_unavailable(
            std::ostream& out,
            std::string_view reason) {
            out << "Sweep replay trace:\n"
                << "  captured=false\n"
                << "  reason=" << reason << "\n";
            if (reason == "not_a_sweep_report") {
                out << "  note=trace projection reports sweep proposal/replay history; this report is a single-run or panel result with no sweep trace to replay\n"
                    << "  next_step=enable `sweep.enabled=true` in the PlanSpec for replay/proposal traces, or use `--report full`, `--report objective`, or `--report topology` for non-sweep diagnostics\n";
            }
            else if (reason == "trace_not_captured") {
                out << "  note=this sweep report did not capture a proposal trace for the selected sweep engine/configuration\n";
            }
        }

        void print_sweep_replay_trace(
            const SweepReport& report,
            std::ostream& out) {
            const auto& trace = report.proposal_trace;
            if (!trace.captured) {
                print_sweep_replay_trace_unavailable(out, "trace_not_captured");
                return;
            }

            out << "Sweep replay trace:\n"
                << "  policy=" << trace.policy_name << "\n"
                << "  event_limit=" << trace.event_limit << "\n"
                << "  total_events=" << trace.total_event_count << "\n"
                << "  proposals=" << trace.proposal_count << "\n"
                << "  observations=" << trace.observation_count << "\n"
                << "  promotions=" << trace.promotion_count << "\n"
                << "  rejections=" << trace.rejection_count << "\n"
                << "  events_capped=" << (trace.events_capped ? "true" : "false") << "\n"
                << "  full_event_stream_captured="
                << (trace.full_event_stream_captured ? "true" : "false") << "\n"
                << "  trace_digest="
                << (trace.trace_digest_available
                        ? trace_digest_hex(trace.trace_digest)
                        : std::string("<unavailable>")) << "\n"
                << "  event_order_valid="
                << (trace.event_order_valid ? "true" : "false") << "\n"
                << "  observation_payload_complete="
                << (trace.observation_payload_complete ? "true" : "false") << "\n"
                << "  exact_replay_from_report="
                << (trace.exact_replay_from_report ? "true" : "false") << "\n";

            if (!trace.effective_policy_config.empty()) {
                out << "  effective_policy_config:\n";
                for (const auto& entry : trace.effective_policy_config) {
                    out << "    " << entry.name << "=" << entry.value << "\n";
                }
            }
            if (!trace.event_order_violations.empty()) {
                out << "  event_order_violations:\n";
                for (const auto& violation : trace.event_order_violations) {
                    out << "    - " << violation << "\n";
                }
            }
            if (!trace.observation_payload_gaps.empty()) {
                out << "  observation_payload_gaps:\n";
                for (const auto& gap : trace.observation_payload_gaps) {
                    out << "    - " << gap << "\n";
                }
            }
            if (!trace.non_replayable_reasons.empty()) {
                out << "  non_replayable_reasons:\n";
                for (const auto& reason : trace.non_replayable_reasons) {
                    out << "    - " << reason << "\n";
                }
            }
            if (!trace.seeds.empty()) {
                out << "  seeds:\n";
                for (const auto& seed : trace.seeds) {
                    out << "    " << seed.source
                        << " stream=" << seed.stream_name
                        << " requested_seed=" << seed.requested_seed
                        << " explicit="
                        << (seed.requested_seed_explicit ? "true" : "false")
                        << " effective_seed=" << seed.effective_seed
                        << " deterministic="
                        << (seed.deterministic ? "true" : "false") << "\n";
                }
            }

            const std::size_t printed_events =
                std::min<std::size_t>(trace.events.size(), 12);
            if (printed_events == 0) {
                return;
            }
            out << "  event_sample:\n";
            for (std::size_t i = 0; i < printed_events; ++i) {
                const auto& event = trace.events[i];
                out << "    " << sweep_trace_event_kind_name(event.kind)
                    << " stage=" << sweep_progress_stage_name(event.stage)
                    << " candidate_index=" << event.candidate_index
                    << " proposal_order=" << event.proposal_order
                    << " batch_index=" << event.batch_index;
                if (event.has_parent_candidate) {
                    out << " parent_candidate_index="
                        << event.parent_candidate_index;
                }
                if (!event.reason.empty()) {
                    out << " reason=" << event.reason;
                }
                if (event.score_present) {
                    out << " score=";
                    if (event.score_finite) {
                        out << event.score;
                    }
                    else {
                        out << "<non_finite>";
                    }
                }
                if (event.run_outcome_present) {
                    out << " run_outcome="
                        << run_outcome_state_name(event.run_outcome);
                }
                if (event.run_rankability_present) {
                    out << " run_rankability="
                        << run_rankability_state_name(event.run_rankability);
                }
                out << "\n";
            }
            if (trace.events.size() > printed_events) {
                out << "    ... suppressed_events="
                    << (trace.events.size() - printed_events) << "\n";
            }
        }
    } // namespace

    void print_text_evaluation_report(
        const EvaluationReport& report,
        EvaluationReportProjection projection,
        std::ostream& out) {
        switch (report.kind) {
        case EvaluationReportKind::Run: {
            const RunReport run = report.run.value_or(RunReport{});
            if (projection == EvaluationReportProjection::Full) {
                print_run_report_summary(run, out);
            }
            else if (projection == EvaluationReportProjection::ObjectiveBreakdown) {
                print_objective_components(run, out, "");
            }
            else if (projection == EvaluationReportProjection::TopologyDiagnostics) {
                print_run_topology_diagnostics(run.provenance, out);
            }
            else if (projection == EvaluationReportProjection::ReplayTrace) {
                print_sweep_replay_trace_unavailable(out, "not_a_sweep_report");
            }
            else if (projection == EvaluationReportProjection::CandidateTable) {
                print_sweep_candidate_table_unavailable(out, "run");
            }
            else {
                print_run_compact(run, out);
            }
            return;
        }
        case EvaluationReportKind::Sweep: {
            const SweepReport sweep = report.sweep.value_or(SweepReport{});
            switch (projection) {
            case EvaluationReportProjection::Full:
                print_plan_sweep_report(sweep, out);
                return;
            case EvaluationReportProjection::Compact:
                print_sweep_compact(sweep, out);
                return;
            case EvaluationReportProjection::CandidateTable:
                print_sweep_candidate_table(sweep, out);
                return;
            case EvaluationReportProjection::ObjectiveBreakdown:
                print_sweep_objective_breakdown(sweep, out);
                return;
            case EvaluationReportProjection::DiversityDiagnostics:
                print_sweep_diversity(sweep, out);
                return;
            case EvaluationReportProjection::DuplicateTradePaths:
                print_sweep_duplicate_trade_paths(sweep, out);
                return;
            case EvaluationReportProjection::SearchGeometry:
                print_sweep_geometry(sweep, out);
                return;
            case EvaluationReportProjection::ParetoFront:
                print_sweep_pareto_front(sweep, out);
                return;
            case EvaluationReportProjection::PosteriorDiagnostics:
                print_plan_sweep_posterior_diagnostics(sweep, out);
                return;
            case EvaluationReportProjection::TopologyDiagnostics:
                print_run_topology_diagnostics(sweep.run_provenance, out);
                return;
            case EvaluationReportProjection::ReplayTrace:
                print_sweep_replay_trace(sweep, out);
                return;
            case EvaluationReportProjection::Json:
                return;
            }
            return;
        }
        case EvaluationReportKind::Panel: {
            const PanelReport panel = report.panel.value_or(PanelReport{});
            if (projection == EvaluationReportProjection::Full) {
                print_panel_report_summary(panel, out);
                return;
            }
            if (projection == EvaluationReportProjection::CandidateTable) {
                print_sweep_candidate_table_unavailable(out, "panel");
                return;
            }
            if (projection == EvaluationReportProjection::ObjectiveBreakdown) {
                out << "Panel objective breakdown:\n";
                for (std::size_t i = 0; i < panel.top_rows.size(); ++i) {
                    out << "-- Rank " << (i + 1)
                        << " | series=" << panel.top_rows[i].series_key
                        << " | entity=" << panel.top_rows[i].entity_key
                        << " --\n";
                    print_objective_components(panel.top_rows[i].run, out, "  ");
                }
                return;
            }
            if (projection == EvaluationReportProjection::TopologyDiagnostics) {
                print_run_topology_diagnostics(panel.run_provenance, out);
                return;
            }
            if (projection == EvaluationReportProjection::ReplayTrace) {
                print_sweep_replay_trace_unavailable(out, "not_a_sweep_report");
                return;
            }
            out << "Panel summary:\n"
                << "  entities_total=" << panel.entities_total
                << " ok=" << panel.entities_ok
                << " failed=" << panel.entities_failed << "\n"
                << "  ranking_objective="
                << describe_ranking_objective(panel.resolved_objective) << "\n";
            for (std::size_t i = 0; i < panel.top_rows.size(); ++i) {
                const auto& row = panel.top_rows[i];
                out << "  rank=" << (i + 1)
                    << " series=" << row.series_key
                    << " entity=" << row.entity_key
                    << " score=" << row.ranking_score
                    << " pnl=" << row.run.execution.pnl
                    << " trades=" << row.run.execution.trades << "\n";
            }
            return;
        }
        case EvaluationReportKind::None:
        case EvaluationReportKind::Ensemble:
        case EvaluationReportKind::DynamicDag:
            out << "Evaluation report kind "
                << evaluation_report_kind_name(report.kind)
                << " has no CLI projection yet.\n";
            return;
        }
    }

} // namespace sugar::evaluation_report_internal
