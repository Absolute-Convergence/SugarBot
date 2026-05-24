#include "sweep/plan_sweep.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "plan_sweep_candidate_index.h"
#include "sweep/metrics.h"
#include "sweep/sweep.h"

namespace sugar {

    namespace {
        const char* engine_name(PlanSweepEngineKind kind) {
            switch (kind) {
            case PlanSweepEngineKind::CoarseFine: return "coarse_fine";
            case PlanSweepEngineKind::MonteCarlo: return "monte_carlo";
            case PlanSweepEngineKind::SuccessiveHalving: return "successive_halving";
            case PlanSweepEngineKind::BayesianPosterior: return "bayesian_posterior";
            case PlanSweepEngineKind::Exhaustive:
            default: return "exhaustive";
            }
        }

        std::optional<double> param_value_as_double(const ParamValue& value) {
            return std::visit([](const auto& current) -> std::optional<double> {
                using Current = std::decay_t<decltype(current)>;
                if constexpr (std::is_same_v<Current, std::int64_t>) {
                    return static_cast<double>(current);
                }
                else if constexpr (std::is_same_v<Current, double>) {
                    return current;
                }
                else {
                    return std::nullopt;
                }
            }, value);
        }

        std::optional<double> candidate_param_value(
            const SweepRankedCandidate& candidate,
            const std::string& name) {
            const auto it = std::find_if(
                candidate.parameter_values.begin(),
                candidate.parameter_values.end(),
                [&name](const SweepParameterSetting& setting) {
                    return setting.name == name;
                });
            if (it == candidate.parameter_values.end()) {
                return std::nullopt;
            }
            return param_value_as_double(it->value);
        }

        bool approximately_equal(double lhs, double rhs) {
            const double scale = std::max({ 1.0, std::abs(lhs), std::abs(rhs) });
            return std::abs(lhs - rhs) <= (scale * 1e-9);
        }

        std::string format_sweep_warning_value(double value, bool is_int) {
            std::ostringstream out;
            if (is_int) {
                out << static_cast<std::int64_t>(std::llround(value));
            }
            else {
                out << std::setprecision(12) << value;
            }
            return out.str();
        }

        std::vector<std::string> build_sweep_report_warnings(const PlanSweepReport& report) {
            std::vector<std::string> warnings;
            if (report.top_candidates.empty()) {
                return warnings;
            }

            if (report.top_candidates.size() >= 2) {
                for (const auto& range : report.parameter_ranges) {
                    double lower = range.requested_range.min;
                    double upper = range.requested_range.max;
                    if (!range.requested_range.values.empty()) {
                        const auto [mn, mx] = std::minmax_element(
                            range.requested_range.values.begin(),
                            range.requested_range.values.end());
                        lower = *mn;
                        upper = *mx;
                    }

                    std::size_t lower_count = 0;
                    std::size_t upper_count = 0;
                    for (const auto& candidate : report.top_candidates) {
                        const auto value = candidate_param_value(candidate, range.name);
                        if (!value.has_value()) {
                            continue;
                        }
                        if (approximately_equal(*value, lower)) {
                            ++lower_count;
                        }
                        if (approximately_equal(*value, upper)) {
                            ++upper_count;
                        }
                    }

                    const auto add_boundary_warning = [&](const char* side, double value) {
                        warnings.push_back(
                            "sweep boundary concentration: all " +
                            std::to_string(report.top_candidates.size()) +
                            " returned candidates use " + range.name + "=" +
                            format_sweep_warning_value(value, range.requested_range.is_int) +
                            " at the " + side +
                            " sweep bound; consider extending that range if the objective is still improving at the edge");
                    };

                    if (lower_count == report.top_candidates.size()) {
                        add_boundary_warning("lower", lower);
                    }
                    else if (upper_count == report.top_candidates.size()) {
                        add_boundary_warning("upper", upper);
                    }
                }
            }

            const std::pair<const char*, const char*> fast_slow_pairs[] = {
                { "n_fast", "n_slow" },
                { "fast_n", "slow_n" },
                { "fast", "slow" },
            };
            for (const auto& [fast_name, slow_name] : fast_slow_pairs) {
                std::size_t inverted = 0;
                for (const auto& candidate : report.top_candidates) {
                    const auto fast = candidate_param_value(candidate, fast_name);
                    const auto slow = candidate_param_value(candidate, slow_name);
                    if (fast.has_value() && slow.has_value() && *fast > *slow) {
                        ++inverted;
                    }
                }
                if (inverted > 0) {
                    warnings.push_back(
                        "semantic fast/slow warning: " + std::to_string(inverted) +
                        "/" + std::to_string(report.top_candidates.size()) +
                        " returned candidates have " + fast_name + " > " + slow_name +
                        "; if these params represent fast/slow windows, constrain the sweep ranges or swap the naming");
                }
            }

            return warnings;
        }

        void print_pareto_objective_label(
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

        void print_pareto_objective_ranges(
            const std::vector<SweepParetoObjectiveRange>& ranges,
            std::ostream& out) {
            if (ranges.empty()) {
                return;
            }
            out << "  objective ranges:\n";
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

        void print_pareto_summary(
            const SweepParetoSummary& pareto,
            std::ostream& out) {
            if (!pareto.requested) {
                return;
            }

            out << "Pareto front:\n"
                << "  requested: true\n"
                << "  computed: " << (pareto.computed ? "true" : "false") << "\n"
                << "  source: " << sweep_pareto_summary_source_name(pareto.source) << "\n"
                << "  observed candidates: " << pareto.observed_candidate_count << "\n"
                << "  unique candidates: " << pareto.unique_candidate_count << "\n"
                << "  duplicate observations: " << pareto.duplicate_observation_count << "\n"
                << "  retained candidate pool: " << pareto.retained_candidate_pool << "\n"
                << "  finite candidates: " << pareto.finite_candidate_count << "\n"
                << "  incomplete candidates: " << pareto.incomplete_candidate_count << "\n"
                << "  dominated candidates: " << pareto.dominated_candidate_count << "\n"
                << "  non-dominated candidates: "
                << pareto.non_dominated_candidate_count << "\n"
                << "  emitted front rows: " << pareto.emitted_front_row_count << "\n"
                << "  front row limit: " << pareto.front_row_limit
                << (pareto.front_row_limit == 0 ? " (uncapped)" : "") << "\n"
                << "  front rows capped: " << (pareto.front_rows_capped ? "true" : "false") << "\n"
                << "  output depollution requested: "
                << (pareto.output_depollution_requested ? "true" : "false") << "\n"
                << "  output depollution source: "
                << sweep_pareto_output_depollution_source_name(
                    pareto.output_depollution_source) << "\n"
                << "  output depollution used result diversity: "
                << (pareto.output_depollution_used_result_diversity ? "true" : "false") << "\n"
                << "  emitted unique trade hashes: " << pareto.emitted_unique_trade_hashes << "\n"
                << "  emitted duplicate trade-hash groups: "
                << pareto.emitted_duplicate_trade_hash_groups << "\n"
                << "  largest emitted trade-hash group: "
                << pareto.emitted_largest_trade_hash_group << "\n"
                << "  emitted basin count: " << pareto.emitted_basin_count << "\n"
                << "  suppressed duplicate trade hashes: "
                << pareto.suppressed_duplicate_trade_hash << "\n"
                << "  suppressed basin candidates: " << pareto.suppressed_basin << "\n"
                << "  suppressed low-trade candidates: "
                << pareto.suppressed_min_trades << "\n"
                << "  suppressed by row limit: " << pareto.suppressed_row_limit << "\n";
            if (pareto.output_depollution_requested) {
                out << "  output depollution dedupe trade hash: "
                    << (pareto.output_depollution_dedupe_trade_hash ? "true" : "false") << "\n"
                    << "  output depollution basin aware: "
                    << (pareto.output_depollution_basin_aware ? "true" : "false") << "\n"
                    << "  output depollution max per basin: "
                    << pareto.output_depollution_max_per_basin << "\n"
                    << "  output depollution param bucket size: "
                    << pareto.output_depollution_param_bucket_size << "\n"
                    << "  output depollution min param distance: "
                    << pareto.output_depollution_min_param_distance << "\n"
                    << "  output depollution min trades: "
                    << pareto.output_depollution_min_trades << "\n";
            }

            if (!pareto.objectives.empty()) {
                out << "  primary sort objective: ";
                print_pareto_objective_label(
                    pareto.objectives[std::min(
                        pareto.primary_sort_index,
                        pareto.objectives.size() - 1)],
                    out);
                out << "\n";
            }

            print_pareto_objective_ranges(pareto.objective_ranges, out);

            for (const auto& candidate : pareto.front) {
                out << "  front rank " << candidate.front_rank
                    << ": candidate_index=" << candidate.candidate_index
                    << " scalar_score=" << candidate.scalar_score
                    << " dominates=" << candidate.dominates_count
                    << " objectives=[";
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
                out << "] params=";
                for (std::size_t i = 0; i < candidate.parameter_values.size(); ++i) {
                    if (i != 0) {
                        out << ", ";
                    }
                    out << candidate.parameter_values[i].name << "="
                        << plan_sweep_param_value_to_display_string(
                            candidate.parameter_values[i].value);
                }
                out << "\n";
            }
        }

        void print_posterior_summary(
            const SweepPosteriorDiagnosticsSummary& posterior,
            std::ostream& out) {
            if (!posterior.requested) {
                return;
            }

            const auto acquisition_scored_count = std::count_if(
                posterior.proposal_reasons.begin(),
                posterior.proposal_reasons.end(),
                [](const SweepPosteriorProposalReason& reason) {
                    return reason.acquisition_score_present &&
                        reason.acquisition_score_finite;
                });
            const auto non_acquisition_count =
                posterior.proposal_reasons.size() -
                static_cast<std::size_t>(acquisition_scored_count);

            out << "Bayesian posterior diagnostics:\n"
                << "  status:\n"
                << "    requested: true\n"
                << "    computed: "
                << (posterior.computed ? "true" : "false") << "\n"
                << "    status: "
                << sweep_posterior_diagnostics_status_name(posterior.status)
                << "\n"
                << "  policy config:\n"
                << "    surrogate: "
                << plan_sweep_posterior_surrogate_kind_name(
                    posterior.surrogate) << "\n"
                << "    acquisition: "
                << plan_sweep_posterior_acquisition_kind_name(
                    posterior.acquisition) << "\n"
                << "    initial design size: "
                << posterior.initial_design_size << "\n"
                << "    iterations: " << posterior.iterations << "\n"
                << "    seed: " << posterior.seed << "\n"
                << "    exploration weight: "
                << posterior.exploration_weight << "\n"
                << "    normalize inputs: "
                << (posterior.normalize_inputs ? "true" : "false") << "\n"
                << "    retained top K: " << posterior.top_k << "\n"
                << "  training data:\n"
                << "    surrogate trained: "
                << (posterior.surrogate_trained ? "true" : "false") << "\n"
                << "    training observations: "
                << posterior.training_observation_count << "\n"
                << "    finite observations: "
                << posterior.finite_observation_count << "\n"
                << "    surrogate training rows: "
                << posterior.surrogate_training_observation_count << "\n"
                << "    invalid feature observations filtered: "
                << posterior.invalid_training_observation_count << "\n"
                << "  proposal summary:\n"
                << "    acquisition ready: "
                << (posterior.acquisition_ready ? "true" : "false") << "\n"
                << "    proposal decisions: "
                << posterior.proposal_reasons.size() << "\n"
                << "    acquisition-scored proposals: "
                << acquisition_scored_count << "\n"
                << "    exploration/fallback proposals: "
                << non_acquisition_count << "\n";
            if (!posterior.unsupported_reason.empty()) {
                out << "  unsupported reason: "
                    << posterior.unsupported_reason << "\n";
            }
            if (!posterior.diagnostics.empty()) {
                out << "  diagnostics:\n";
                for (const auto& diagnostic : posterior.diagnostics) {
                    out << "    " << diagnostic << "\n";
                }
            }
            if (!posterior.proposal_reasons.empty()) {
                out << "  proposal decision details:\n";
            }
            for (const auto& reason : posterior.proposal_reasons) {
                out << "    order " << reason.proposal_order
                    << ": candidate index " << reason.candidate_index
                    << ", acquisition score ";
                if (!reason.acquisition_score_present) {
                    out << "<missing>";
                }
                else if (!reason.acquisition_score_finite) {
                    out << "<non_finite>";
                }
                else {
                    out << reason.acquisition_score;
                }
                if (!reason.reason.empty()) {
                    out << ", reason " << reason.reason;
                }
                out << "\n";
            }
        }

        void print_trace_summary(
            const SweepTraceSummary& trace,
            std::ostream& out) {
            if (!trace.captured) {
                return;
            }

            const auto digest_hex = [](std::uint64_t digest) {
                std::ostringstream text;
                text << "0x" << std::hex << std::setw(16)
                    << std::setfill('0') << digest;
                return text.str();
            };

            out << "Proposal trace:\n"
                << "  policy: " << trace.policy_name << "\n"
                << "  event limit: " << trace.event_limit << "\n"
                << "  total events: " << trace.total_event_count << "\n"
                << "  proposals: " << trace.proposal_count << "\n"
                << "  observations: " << trace.observation_count << "\n"
                << "  promotions: " << trace.promotion_count << "\n"
                << "  rejections: " << trace.rejection_count << "\n"
                << "  events capped: " << (trace.events_capped ? "true" : "false") << "\n"
                << "  full event stream captured: "
                << (trace.full_event_stream_captured ? "true" : "false") << "\n"
                << "  trace digest: "
                << (trace.trace_digest_available ? digest_hex(trace.trace_digest) : "<unavailable>") << "\n"
                << "  event order valid: "
                << (trace.event_order_valid ? "true" : "false") << "\n"
                << "  observation payload complete: "
                << (trace.observation_payload_complete ? "true" : "false") << "\n"
                << "  exact replay from report: "
                << (trace.exact_replay_from_report ? "true" : "false") << "\n";
            if (!trace.effective_policy_config.empty()) {
                out << "  effective policy config:\n";
                for (const auto& entry : trace.effective_policy_config) {
                    out << "    " << entry.name << "=" << entry.value << "\n";
                }
            }
            if (!trace.event_order_violations.empty()) {
                out << "  event-order violations:\n";
                for (const auto& violation : trace.event_order_violations) {
                    out << "    - " << violation << "\n";
                }
            }
            if (!trace.observation_payload_gaps.empty()) {
                out << "  observation payload gaps:\n";
                for (const auto& gap : trace.observation_payload_gaps) {
                    out << "    - " << gap << "\n";
                }
            }
            if (!trace.non_replayable_reasons.empty()) {
                out << "  non-replayable reasons:\n";
                for (const auto& reason : trace.non_replayable_reasons) {
                    out << "    " << reason << "\n";
                }
            }
            if (!trace.seeds.empty()) {
                out << "  seeds:\n";
                for (const auto& seed : trace.seeds) {
                    out << "    " << seed.source
                        << " stream=" << seed.stream_name
                        << " requested_seed=" << seed.requested_seed
                        << " explicit=" << (seed.requested_seed_explicit ? "true" : "false")
                        << " effective_seed=" << seed.effective_seed
                        << " deterministic=" << (seed.deterministic ? "true" : "false")
                        << "\n";
                }
            }

            const std::size_t printed_events =
                std::min<std::size_t>(trace.events.size(), 8);
            if (printed_events == 0) {
                return;
            }
            out << "  first events:\n";
            for (std::size_t i = 0; i < printed_events; ++i) {
                const auto& event = trace.events[i];
                out << "    " << sweep_trace_event_kind_name(event.kind)
                    << " stage=" << sweep_progress_stage_name(event.stage)
                    << " candidate_index=" << event.candidate_index
                    << " proposal_order=" << event.proposal_order;
                if (event.has_parent_candidate) {
                    out << " parent_candidate_index="
                        << event.parent_candidate_index;
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
                    out << " outcome="
                        << run_outcome_state_name(event.run_outcome);
                }
                if (event.run_rankability_present) {
                    out << " rankability="
                        << run_rankability_state_name(event.run_rankability);
                }
                if (!event.reason.empty()) {
                    out << " reason=" << event.reason;
                }
                out << "\n";
            }
        }

        void print_string_list(
            const std::vector<std::string>& values,
            std::ostream& out) {
            if (values.empty()) {
                out << "<none>";
                return;
            }
            for (std::size_t i = 0; i < values.size(); ++i) {
                if (i != 0) {
                    out << ", ";
                }
                out << values[i];
            }
        }

        void print_topology_artifact(
            const RunTopologyArtifactSummary& artifact,
            std::ostream& out,
            std::string_view indent) {
            if (!artifact.present) {
                out << indent << "artifact: <none>\n";
                return;
            }

            out << indent << "artifact:";
            if (!artifact.stable_id.empty()) {
                out << " stable_id=" << artifact.stable_id;
            }
            if (!artifact.locator.empty()) {
                out << " locator=" << artifact.locator;
            }
            if (!artifact.format.empty()) {
                out << " format=" << artifact.format;
            }
            if (!artifact.digest.empty()) {
                out << " digest=" << artifact.digest;
            }
            if (!artifact.producer.empty()) {
                out << " producer=" << artifact.producer;
            }
            out << "\n";
        }
    } // namespace

    std::vector<std::string> derive_plan_sweep_report_warnings(
        const PlanSweepReport& report) {
        return build_sweep_report_warnings(report);
    }

    const char* sweep_posterior_diagnostics_status_name(
        SweepPosteriorDiagnosticsStatus status) {
        switch (status) {
        case SweepPosteriorDiagnosticsStatus::Unsupported:
            return "unsupported";
        case SweepPosteriorDiagnosticsStatus::Ready:
            return "ready";
        case SweepPosteriorDiagnosticsStatus::Computed:
            return "computed";
        case SweepPosteriorDiagnosticsStatus::NotRequested:
        default:
            return "not_requested";
        }
    }

    SweepCandidateReport build_sweep_candidate_report(
        const SweepRankedCandidate& candidate,
        const RankingObjectiveSpec* objective,
        const RunProvenanceSummary* provenance,
        std::size_t rank) {
        SweepCandidateReport report{};
        report.rank = rank;
        report.candidate_index = candidate.candidate_index;
        report.score = candidate.score;
        report.parameter_values = candidate.parameter_values;
        report.run = build_run_report(candidate.result, objective, provenance);
        return report;
    }

    std::vector<SweepCandidateReport> build_sweep_candidate_reports(
        const std::vector<SweepRankedCandidate>& candidates,
        const RankingObjectiveSpec* objective,
        const RunProvenanceSummary* provenance) {
        std::vector<SweepCandidateReport> reports;
        reports.reserve(candidates.size());
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            reports.push_back(build_sweep_candidate_report(
                candidates[i],
                objective,
                provenance,
                i + 1));
        }
        return reports;
    }

    void print_plan_sweep_posterior_diagnostics(
        const PlanSweepReport& report,
        std::ostream& out) {
        if (report.posterior.requested) {
            print_posterior_summary(report.posterior, out);
            return;
        }

        out << "Bayesian posterior diagnostics:\n"
            << "  status:\n"
            << "    requested: false\n"
            << "    computed: false\n"
            << "    status: not_requested\n"
            << "  engine: " << engine_name(report.requested_sweep.engine) << "\n";
    }

    void print_run_topology_diagnostics(
        const RunProvenanceSummary& provenance,
        std::ostream& out) {
        const std::size_t topology_count = provenance.topologies.size();
        const std::size_t topology_reported_count = std::min(
            topology_count,
            kRunTopologyDiagnosticsTopologyLimit);
        const std::size_t topology_suppressed_count =
            topology_count - topology_reported_count;
        const std::size_t usage_count = provenance.topology_usages.size();
        const std::size_t usage_reported_count = std::min(
            usage_count,
            kRunTopologyDiagnosticsUsageLimit);
        const std::size_t usage_suppressed_count =
            usage_count - usage_reported_count;

        out << "Topology diagnostics:\n"
            << "  status:\n"
            << "    primary_series: "
            << (provenance.primary_series_key.empty()
                    ? "<none>"
                    : provenance.primary_series_key)
            << "\n"
            << "    primary_selection_mode: "
            << (provenance.primary_selection_mode.empty()
                    ? "<unknown>"
                    : provenance.primary_selection_mode)
            << "\n"
            << "    has_time_axis: "
            << (provenance.has_time_axis ? "true" : "false") << "\n"
            << "    has_entity_axis: "
            << (provenance.has_entity_axis ? "true" : "false") << "\n"
            << "    has_spatial_axis: "
            << (provenance.has_spatial_axis ? "true" : "false") << "\n"
            << "    declared_topologies: "
            << topology_count << "\n"
            << "    declared_topologies_reported: "
            << topology_reported_count << "\n"
            << "    declared_topologies_suppressed: "
            << topology_suppressed_count << "\n"
            << "    topology_usages: "
            << usage_count << "\n"
            << "    topology_usages_reported: "
            << usage_reported_count << "\n"
            << "    topology_usages_suppressed: "
            << usage_suppressed_count << "\n";

        out << "  declared topologies:\n";
        if (provenance.topologies.empty()) {
            out << "    <none>\n";
        }
        for (std::size_t i = 0; i < topology_reported_count; ++i) {
            const auto& topology = provenance.topologies[i];
            out << "    - " << topology.name << "\n"
                << "      family: "
                << (topology.family.empty() ? "<unknown>" : topology.family) << "\n"
                << "      origin: "
                << (topology.origin.empty() ? "<unknown>" : topology.origin) << "\n"
                << "      bearing: "
                << (topology.bearing.empty() ? "<unknown>" : topology.bearing) << "\n"
                << "      domain_scope: "
                << (topology.domain_scope.empty()
                        ? "<unknown>"
                        : topology.domain_scope)
                << "\n"
                << "      query_kinds: ";
            print_string_list(topology.query_kinds, out);
            out << "\n"
                << "      directed: "
                << (topology.directed ? "true" : "false") << "\n"
                << "      weighted: "
                << (topology.weighted ? "true" : "false") << "\n"
                << "      node_count: " << topology.node_count << "\n"
                << "      edge_count: " << topology.edge_count << "\n"
                << "      time_valid_edges: "
                << topology.time_valid_edge_count << "\n"
                << "      versioned_edges: "
                << topology.versioned_edge_count << "\n"
                << "      semantic_embeddings: "
                << topology.semantic_embedding_count << "\n";
            print_topology_artifact(topology.artifact, out, "      ");
        }
        if (topology_suppressed_count > 0) {
            out << "    ... " << topology_suppressed_count
                << " declared topologies suppressed by report limit ("
                << kRunTopologyDiagnosticsTopologyLimit << ")\n";
        }

        out << "  topology usages:\n";
        if (provenance.topology_usages.empty()) {
            out << "    <none>\n";
        }
        for (std::size_t i = 0; i < usage_reported_count; ++i) {
            const auto& usage = provenance.topology_usages[i];
            out << "    - node="
                << (usage.subject_id.empty() ? "<unknown>" : usage.subject_id)
                << " op="
                << (usage.op_name.empty() ? "<unknown>" : usage.op_name)
                << " topology="
                << (usage.topology_name.empty()
                        ? "<none>"
                        : usage.topology_name)
                << "\n"
                << "      subject_kind: "
                << (usage.subject_kind.empty()
                        ? "<unknown>"
                        : usage.subject_kind)
                << "\n"
                << "      catalog_present: "
                << (usage.catalog_present ? "true" : "false") << "\n"
                << "      runtime_resolved: "
                << (usage.runtime_resolved ? "true" : "false") << "\n"
                << "      topology_origin: "
                << (usage.topology_origin.empty()
                        ? "<unknown>"
                        : usage.topology_origin)
                << "\n"
                << "      topology_bearing: "
                << (usage.topology_bearing.empty()
                        ? "<unknown>"
                        : usage.topology_bearing)
                << "\n"
                << "      required_families: ";
            print_string_list(usage.required_topology_families, out);
            out << "\n"
                << "      required_query_kinds: ";
            print_string_list(usage.required_topology_query_kinds, out);
            out << "\n"
                << "      locality_subject: ";
            if (usage.locality_subject_kind.empty()) {
                out << "<none>";
            }
            else {
                out << usage.locality_subject_kind << ":"
                    << (usage.locality_subject_value.empty()
                            ? "<default>"
                            : usage.locality_subject_value)
                    << " explicit="
                    << (usage.locality_subject_explicit ? "true" : "false");
            }
            out << "\n";
            if (!usage.resolved_series_key.empty() ||
                !usage.resolved_entity_key.empty()) {
                out << "      resolved_subject: series="
                    << (usage.resolved_series_key.empty()
                            ? "<none>"
                            : usage.resolved_series_key)
                    << " entity="
                    << (usage.resolved_entity_key.empty()
                            ? "<none>"
                            : usage.resolved_entity_key)
                    << "\n";
            }
            print_topology_artifact(usage.artifact, out, "      ");
        }
        if (usage_suppressed_count > 0) {
            out << "    ... " << usage_suppressed_count
                << " topology usages suppressed by report limit ("
                << kRunTopologyDiagnosticsUsageLimit << ")\n";
        }
    }

    void print_plan_sweep_report(
        const PlanSweepReport& report,
        std::ostream& out)
    {
        auto format_requested_value = [](double value) {
            std::ostringstream oss;
            oss << std::setprecision(12) << value;
            return oss.str();
        };

        auto print_requested_range = [&out, &format_requested_value](const SweepParameterRangeSummary& range) {
            out << "  " << range.name << ": ";
            if (!range.requested_range.values.empty()) {
                out << "[";
                for (std::size_t i = 0; i < range.requested_range.values.size(); ++i) {
                    if (i) out << ", ";
                    if (range.requested_range.is_int) {
                        out << static_cast<std::int64_t>(std::llround(range.requested_range.values[i]));
                    }
                    else {
                        std::ostringstream value_out;
                        value_out << std::setprecision(12) << range.requested_range.values[i];
                        out << value_out.str();
                    }
                }
                out << "]";
            }
            else {
                if (range.requested_range.is_int) {
                    out << static_cast<std::int64_t>(std::llround(range.requested_range.min))
                        << " .. " << static_cast<std::int64_t>(std::llround(range.requested_range.max))
                        << " step " << static_cast<std::int64_t>(std::llround(range.requested_range.step));
                }
                else {
                    out << format_requested_value(range.requested_range.min)
                        << " .. " << format_requested_value(range.requested_range.max)
                        << " step " << format_requested_value(range.requested_range.step);
                }
            }
            out << " (" << range.expanded_count << " values)\n";
        };

        auto print_candidate = [&out, &report](const SweepRankedCandidate& candidate, std::size_t rank) {
            out << "-- Rank " << rank << " | score=" << candidate.score << " --\n";
            out << "Params:\n";
            for (const auto& setting : candidate.parameter_values) {
                out << "  " << setting.name << ": "
                    << plan_sweep_param_value_to_display_string(setting.value) << "\n";
            }
            out << "Run report:\n";
            print_run_report_summary(
                build_run_report(candidate.result, &report.resolved_objective),
                out,
                "  ");
            out << "\n";
        };

        out << "\n=== PlanSpec Sweep Results ===\n";
        out << "Engine: " << engine_name(report.requested_sweep.engine) << "\n";
        out << "Objective: " << describe_ranking_objective(report.resolved_objective) << "\n";
        out << "Top K: " << report.requested_sweep.top_k << "\n";
        out << "Max evals: " << report.requested_sweep.max_evals << "\n";
        out << "Param dims: " << report.parameter_count << "\n";
        out << "Total combos: ";
        if (report.total_combinations_overflow) {
            out << "(overflow)\n";
        }
        else {
            out << report.total_combinations << "\n";
        }
        out << "Candidate space: " << report.candidate_space_size;
        if (report.candidate_space_truncated) {
            out << " (truncated)";
        }
        out << "\n";
        out << "Evals run: " << report.evaluations_run << "\n";
        out << "Execution failures: " << report.execution_failures << "\n";
        out << "Compile failures: " << report.compile_failures << "\n";
        if (report.canceled) {
            out << "Canceled: true\n";
        }
        print_run_provenance_summary(report.run_provenance, out);
        out << "Candidate outcomes:\n"
            << "  completed: " << report.candidate_outcomes.completed << "\n"
            << "  completed_with_non_finite_metrics: " << report.candidate_outcomes.completed_with_non_finite_metrics << "\n"
            << "  failed: " << report.candidate_outcomes.failed << "\n"
            << "  canceled: " << report.candidate_outcomes.canceled << "\n"
            << "Objective rankability:\n"
            << "  not_evaluated: " << report.candidate_rankability.not_evaluated << "\n"
            << "  rankable: " << report.candidate_rankability.rankable << "\n"
            << "  missing_primary_value: " << report.candidate_rankability.missing_primary_value << "\n"
            << "  non_finite_primary_value: " << report.candidate_rankability.non_finite_primary_value << "\n"
            << "  missing_penalty_value: " << report.candidate_rankability.missing_penalty_value << "\n"
            << "  non_finite_penalty_value: " << report.candidate_rankability.non_finite_penalty_value << "\n";
        out << "Result diversity:\n"
            << "  requested: " << (report.result_diversity.requested ? "true" : "false") << "\n"
            << "  retained candidate pool: " << report.result_diversity.retained_candidate_pool << "\n"
            << "  returned unique trade hashes: " << report.result_diversity.returned_unique_trade_hashes << "\n"
            << "  returned duplicate trade-hash groups: " << report.result_diversity.returned_duplicate_trade_hash_groups << "\n"
            << "  largest returned trade-hash group: " << report.result_diversity.returned_largest_trade_hash_group << "\n";
        if (report.result_diversity.requested) {
            out << "  dedupe trade hash: " << (report.result_diversity.dedupe_trade_hash ? "true" : "false") << "\n"
                << "  basin aware: " << (report.result_diversity.basin_aware ? "true" : "false") << "\n"
                << "  oversample multiplier: " << report.result_diversity.oversample_multiplier << "\n"
                << "  max per basin: " << report.result_diversity.max_per_basin << "\n"
                << "  param bucket size: " << report.result_diversity.param_bucket_size << "\n"
                << "  min param distance: " << report.result_diversity.min_param_distance << "\n"
                << "  min trades: " << report.result_diversity.min_trades << "\n"
                << "  suppressed duplicate trade hashes: " << report.result_diversity.suppressed_duplicate_trade_hash << "\n"
                << "  suppressed basin candidates: " << report.result_diversity.suppressed_basin << "\n"
                << "  suppressed low-trade candidates: " << report.result_diversity.suppressed_min_trades << "\n"
                << "  returned basin count: " << report.result_diversity.returned_basin_count << "\n";
        }
        if (!report.warnings.empty()) {
            out << "Sweep warnings:\n";
            for (const auto& warning : report.warnings) {
                out << "  - " << warning << "\n";
            }
        }
        if (!report.last_execution_failure.empty()) {
            out << "Last execution failure: " << report.last_execution_failure << "\n";
        }
        if (!report.last_compile_error.empty()) {
            out << "Last compile error: " << report.last_compile_error << "\n";
        }

        print_pareto_summary(report.pareto, out);
        print_posterior_summary(report.posterior, out);

        if (!report.stage_summaries.empty()) {
            out << "Stages:\n";
            for (const auto& stage : report.stage_summaries) {
                out << "  " << sweep_progress_stage_name(stage.stage)
                    << ": " << stage.done << "/" << stage.total << "\n";
            }
        }

        print_trace_summary(report.proposal_trace, out);

        if (!report.parameter_ranges.empty()) {
            out << "Sweep params:\n";
            for (const auto& range : report.parameter_ranges) {
                print_requested_range(range);
            }
        }

        switch (report.requested_sweep.engine) {
        case PlanSweepEngineKind::CoarseFine:
            out << "Engine settings:\n"
                << "  stage 1 K: " << report.requested_sweep.coarse_fine.k_stage1 << "\n"
                << "  stage 2 K: " << report.requested_sweep.coarse_fine.k_stage2 << "\n"
                << "  coarse step multiplier: " << report.requested_sweep.coarse_fine.coarse_step_multiplier << "\n"
                << "  stage 1 stride: " << report.requested_sweep.coarse_fine.stage1_stride << "\n"
                << "  suppression hops: " << report.requested_sweep.coarse_fine.suppression_hops << "\n"
                << "  stage 2 hops: " << report.requested_sweep.coarse_fine.stage2_hops << "\n"
                << "  depollute stage 1: " << (report.requested_sweep.coarse_fine.depollute_stage1 ? "true" : "false") << "\n"
                << "  dedupe stage 2: " << (report.requested_sweep.coarse_fine.dedupe_stage2 ? "true" : "false")
                << " (candidate-index dedupe, not trade-hash outcome dedupe)\n";
            break;
        case PlanSweepEngineKind::MonteCarlo:
            out << "Engine settings:\n"
                << "  draws: " << report.requested_sweep.monte_carlo.draws << "\n"
                << "  seed: " << report.requested_sweep.monte_carlo.seed << "\n"
                << "  dedupe: " << (report.requested_sweep.monte_carlo.dedupe ? "true" : "false")
                << " (parameter-index sample dedupe, not trade-hash outcome dedupe)\n"
                << "  monte carlo top K: " << report.requested_sweep.monte_carlo.top_k << "\n";
            break;
        case PlanSweepEngineKind::SuccessiveHalving:
            out << "Engine settings:\n"
                << "  rounds: " << report.requested_sweep.successive_halving.rounds << "\n"
                << "  reduction factor: " << report.requested_sweep.successive_halving.reduction_factor << "\n"
                << "  initial candidates: " << report.requested_sweep.successive_halving.initial_candidates << "\n"
                << "  initial stride: " << report.requested_sweep.successive_halving.initial_stride << "\n"
                << "  neighborhood hops: " << report.requested_sweep.successive_halving.neighborhood_hops << "\n"
                << "  dedupe candidates: " << (report.requested_sweep.successive_halving.dedupe_candidates ? "true" : "false") << "\n"
                << "  successive halving top K: " << report.requested_sweep.successive_halving.top_k << "\n";
            break;
        case PlanSweepEngineKind::BayesianPosterior:
            out << "Engine settings:\n"
                << "  surrogate: "
                << plan_sweep_posterior_surrogate_kind_name(
                    report.requested_sweep.bayesian_posterior.surrogate) << "\n"
                << "  acquisition: "
                << plan_sweep_posterior_acquisition_kind_name(
                    report.requested_sweep.bayesian_posterior.acquisition) << "\n"
                << "  initial design size: "
                << report.requested_sweep.bayesian_posterior.initial_design_size << "\n"
                << "  iterations: "
                << report.requested_sweep.bayesian_posterior.iterations << "\n"
                << "  seed: "
                << report.requested_sweep.bayesian_posterior.seed << "\n"
                << "  exploration weight: "
                << report.requested_sweep.bayesian_posterior.exploration_weight << "\n"
                << "  normalize inputs: "
                << (report.requested_sweep.bayesian_posterior.normalize_inputs ? "true" : "false") << "\n"
                << "  retained top K: "
                << report.requested_sweep.bayesian_posterior.top_k << "\n";
            break;
        case PlanSweepEngineKind::Exhaustive:
            break;
        }

        out << "TopK: " << report.top_candidates.size() << "\n";
        if (!report.top_candidates.empty()) {
            out << "Ranked candidate reports:\n\n";
        }
        for (std::size_t i = 0; i < report.top_candidates.size(); ++i) {
            print_candidate(report.top_candidates[i], i + 1);
        }

        if (!report.top_candidates.empty()) {
            out << "=== Best Candidate Run Report (Rank 1) ===\n";
            print_run_report_summary(
                build_run_report(
                    report.top_candidates.front().result,
                    &report.resolved_objective),
                out);
        }
    }

} // namespace sugar
