#include "evaluation_report_internal.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

namespace sugar::evaluation_report_internal {

    namespace {
        std::string escape_json(std::string_view text) {
            std::string out;
            out.reserve(text.size() + 8);
            for (const char ch : text) {
                switch (ch) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    out += ch;
                    break;
                }
            }
            return out;
        }
    }

    void append_json_string(std::ostream& out, std::string_view text) {
        out << '"' << escape_json(text) << '"';
    }

    void append_json_number(std::ostream& out, double value) {
        if (!std::isfinite(value)) {
            out << "null";
            return;
        }
        out << std::setprecision(17) << value;
    }

    void append_json_string_array(
        std::ostream& out,
        const std::vector<std::string>& values) {
        out << "[";
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (i != 0) {
                out << ",";
            }
            append_json_string(out, values[i]);
        }
        out << "]";
    }

    void append_json_size_array(
        std::ostream& out,
        const std::vector<std::size_t>& values) {
        out << "[";
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (i != 0) {
                out << ",";
            }
            out << values[i];
        }
        out << "]";
    }

    namespace {
        void append_json_param_value(std::ostream& out, const ParamValue& value) {
            std::visit([&out](const auto& current) {
                using Current = std::decay_t<decltype(current)>;
                if constexpr (std::is_same_v<Current, std::nullptr_t>) {
                    out << "null";
                }
                else if constexpr (std::is_same_v<Current, bool>) {
                    out << (current ? "true" : "false");
                }
                else if constexpr (std::is_same_v<Current, std::int64_t>) {
                    out << current;
                }
                else if constexpr (std::is_same_v<Current, double>) {
                    append_json_number(out, current);
                }
                else if constexpr (std::is_same_v<Current, std::string>) {
                    append_json_string(out, current);
                }
                else {
                    out << "{\"ref\":";
                    append_json_string(out, current.name);
                    out << "}";
                }
            }, value);
        }
    }

    void append_json_parameter_settings(
        std::ostream& out,
        const std::vector<SweepParameterSetting>& settings) {
        out << "{";
        for (std::size_t p = 0; p < settings.size(); ++p) {
            if (p != 0) {
                out << ",";
            }
            append_json_string(out, settings[p].name);
            out << ":";
            append_json_param_value(out, settings[p].value);
        }
        out << "}";
    }

    namespace {
        void append_json_metric_catalog(
            std::ostream& out,
            const BacktestMetricCatalog& catalog) {
            out << "\"metric_catalog\":[";
            for (std::size_t i = 0; i < catalog.values.size(); ++i) {
                if (i != 0) {
                    out << ",";
                }
                const auto& metric = catalog.values[i];
                out << "{";
                out << "\"name\":";
                append_json_string(out, metric.name);
                out << ",\"kind\":";
                append_json_string(out, result_metric_kind_name(metric.kind));
                out << ",\"value\":";
                if (metric.has_exact_u64_value) {
                    out << metric.exact_u64_value;
                }
                else {
                    append_json_number(out, metric.value);
                }
                out << ",\"is_finite\":" << (metric.is_finite ? "true" : "false");
                out << "}";
            }
            out << "],\"non_finite_metric_names\":";
            append_json_string_array(out, catalog.non_finite_metric_names);
        }

        void append_json_topology_artifact(
            std::ostream& out,
            const RunTopologyArtifactSummary& artifact) {
            out << "{";
            out << "\"present\":" << (artifact.present ? "true" : "false");
            out << ",\"stable_id\":";
            append_json_string(out, artifact.stable_id);
            out << ",\"locator\":";
            append_json_string(out, artifact.locator);
            out << ",\"format\":";
            append_json_string(out, artifact.format);
            out << ",\"digest\":";
            append_json_string(out, artifact.digest);
            out << ",\"producer\":";
            append_json_string(out, artifact.producer);
            out << "}";
        }

        void append_json_topology_summary(
            std::ostream& out,
            const RunTopologySummary& topology) {
            out << "{";
            out << "\"name\":";
            append_json_string(out, topology.name);
            out << ",\"origin\":";
            append_json_string(out, topology.origin);
            out << ",\"bearing\":";
            append_json_string(out, topology.bearing);
            out << ",\"family\":";
            append_json_string(out, topology.family);
            out << ",\"domain_scope\":";
            append_json_string(out, topology.domain_scope);
            out << ",\"query_kinds\":";
            append_json_string_array(out, topology.query_kinds);
            out << ",\"directed\":" << (topology.directed ? "true" : "false");
            out << ",\"weighted\":" << (topology.weighted ? "true" : "false");
            out << ",\"node_count\":" << topology.node_count;
            out << ",\"edge_count\":" << topology.edge_count;
            out << ",\"time_valid_edge_count\":"
                << topology.time_valid_edge_count;
            out << ",\"versioned_edge_count\":"
                << topology.versioned_edge_count;
            out << ",\"semantic_embedding_count\":"
                << topology.semantic_embedding_count;
            out << ",\"artifact\":";
            append_json_topology_artifact(out, topology.artifact);
            out << "}";
        }

        void append_json_topology_usage(
            std::ostream& out,
            const RunTopologyUsageSummary& usage) {
            out << "{";
            out << "\"subject_kind\":";
            append_json_string(out, usage.subject_kind);
            out << ",\"subject_id\":";
            append_json_string(out, usage.subject_id);
            out << ",\"op_name\":";
            append_json_string(out, usage.op_name);
            out << ",\"topology_name\":";
            append_json_string(out, usage.topology_name);
            out << ",\"topology_origin\":";
            append_json_string(out, usage.topology_origin);
            out << ",\"topology_bearing\":";
            append_json_string(out, usage.topology_bearing);
            out << ",\"locality_subject\":{";
            out << "\"kind\":";
            append_json_string(out, usage.locality_subject_kind);
            out << ",\"value\":";
            append_json_string(out, usage.locality_subject_value);
            out << ",\"explicit\":"
                << (usage.locality_subject_explicit ? "true" : "false");
            out << "}";
            out << ",\"resolved_subject\":{";
            out << "\"series_key\":";
            append_json_string(out, usage.resolved_series_key);
            out << ",\"entity_key\":";
            append_json_string(out, usage.resolved_entity_key);
            out << "}";
            out << ",\"required_topology_families\":";
            append_json_string_array(out, usage.required_topology_families);
            out << ",\"required_topology_query_kinds\":";
            append_json_string_array(out, usage.required_topology_query_kinds);
            out << ",\"catalog_present\":"
                << (usage.catalog_present ? "true" : "false");
            out << ",\"runtime_resolved\":"
                << (usage.runtime_resolved ? "true" : "false");
            out << ",\"artifact\":";
            append_json_topology_artifact(out, usage.artifact);
            out << "}";
        }

        void append_json_run_provenance(
            std::ostream& out,
            const RunProvenanceSummary& provenance) {
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

            out << "{";
            out << "\"primary_series_key\":";
            append_json_string(out, provenance.primary_series_key);
            out << ",\"primary_selection_mode\":";
            append_json_string(out, provenance.primary_selection_mode);
            out << ",\"time_kind\":";
            append_json_string(out, provenance.time_kind);
            out << ",\"time_resolution\":";
            append_json_string(out, provenance.time_resolution);
            out << ",\"series_count\":" << provenance.series_count;
            out << ",\"has_time_axis\":"
                << (provenance.has_time_axis ? "true" : "false");
            out << ",\"has_entity_axis\":"
                << (provenance.has_entity_axis ? "true" : "false");
            out << ",\"has_spatial_axis\":"
                << (provenance.has_spatial_axis ? "true" : "false");
            out << ",\"input_warning_count\":" << provenance.input_warning_count;
            out << ",\"input_format\":";
            append_json_string(out, provenance.input_format.empty() ? "csv" : provenance.input_format);
            out << ",\"input_row_count\":" << provenance.input_row_count;
            out << ",\"csv_had_header\":"
                << (provenance.csv_had_header ? "true" : "false");
            out << ",\"csv_physical_line_count\":"
                << provenance.csv_physical_line_count;
            out << ",\"csv_blank_line_count\":" << provenance.csv_blank_line_count;
            out << ",\"csv_data_row_count\":" << provenance.csv_data_row_count;
            out << ",\"dropped_row_count\":" << provenance.dropped_row_count;
            out << ",\"input_warnings\":";
            append_json_string_array(out, provenance.input_warnings);
            out << ",\"alignment_applied\":"
                << (provenance.alignment_applied ? "true" : "false");
            out << ",\"alignment_calendar\":";
            append_json_string(out, provenance.alignment_calendar);
            out << ",\"alignment_missing\":";
            append_json_string(out, provenance.alignment_missing);
            out << ",\"alignment_non_temporal_multi_series\":";
            append_json_string(out, provenance.alignment_non_temporal_multi_series);
            out << ",\"topology_names\":";
            append_json_string_array(out, provenance.topology_names);
            out << ",\"topology_usage_count\":" << provenance.topology_usage_count;
            out << ",\"topology_report_limits\":{";
            out << "\"topologies\":" << kRunTopologyDiagnosticsTopologyLimit;
            out << ",\"topology_usages\":"
                << kRunTopologyDiagnosticsUsageLimit;
            out << "}";
            out << ",\"topologies_reported_count\":"
                << topology_reported_count;
            out << ",\"topologies_suppressed_count\":"
                << topology_suppressed_count;
            out << ",\"topology_usages_reported_count\":"
                << usage_reported_count;
            out << ",\"topology_usages_suppressed_count\":"
                << usage_suppressed_count;
            out << ",\"topologies\":[";
            for (std::size_t i = 0; i < topology_reported_count; ++i) {
                if (i != 0) {
                    out << ",";
                }
                append_json_topology_summary(out, provenance.topologies[i]);
            }
            out << "]";
            out << ",\"topology_usages\":[";
            for (std::size_t i = 0; i < usage_reported_count; ++i) {
                if (i != 0) {
                    out << ",";
                }
                append_json_topology_usage(out, provenance.topology_usages[i]);
            }
            out << "]";
            out << "}";
        }

        void append_json_dynamic_execution(
            std::ostream& out,
            const RunDynamicExecutionSummary& dynamic) {
            out << "{";
            out << "\"enabled\":" << (dynamic.enabled ? "true" : "false");
            out << ",\"mode\":";
            append_json_string(out, dynamic.mode);
            out << ",\"subgraph_ids\":";
            append_json_string_array(out, dynamic.subgraph_ids);
            out << ",\"total_decisions\":" << dynamic.total_decisions;
            out << ",\"decision_sample_limit\":"
                << dynamic.decision_sample_limit;
            out << ",\"emitted_decision_samples\":"
                << dynamic.emitted_decision_samples;
            out << ",\"suppressed_decisions\":"
                << dynamic.suppressed_decisions;
            out << ",\"fallback_decisions\":"
                << dynamic.fallback_decisions;
            out << ",\"inactive_output_rows\":"
                << dynamic.inactive_output_rows;
            out << ",\"inactive_node_rows\":"
                << dynamic.inactive_node_rows;
            out << ",\"max_active_subgraphs_per_row\":"
                << dynamic.max_active_subgraphs_per_row;
            out << ",\"max_active_subgraph_cap\":"
                << dynamic.max_active_subgraph_cap;
            out << ",\"bounded_by_declared_caps\":"
                << (dynamic.bounded_by_declared_caps ? "true" : "false");
            out << ",\"route_decision_digest\":"
                << dynamic.route_decision_digest;
            out << ",\"routes\":[";
            for (std::size_t i = 0; i < dynamic.routes.size(); ++i) {
                if (i != 0) {
                    out << ",";
                }
                const auto& route = dynamic.routes[i];
                out << "{";
                out << "\"id\":";
                append_json_string(out, route.id);
                out << ",\"condition_node_id\":";
                append_json_string(out, route.condition_node_id);
                out << ",\"condition_is_constant\":"
                    << (route.condition_is_constant ? "true" : "false");
                out << ",\"constant_condition_value\":"
                    << (route.constant_condition_value ? "true" : "false");
                out << ",\"true_decisions\":" << route.true_decisions;
                out << ",\"false_decisions\":" << route.false_decisions;
                out << ",\"fallback_decisions\":"
                    << route.fallback_decisions;
                out << ",\"fallback_subgraphs\":";
                append_json_string_array(out, route.fallback_subgraphs);
                out << ",\"cases\":[";
                for (std::size_t case_index = 0;
                     case_index < route.cases.size();
                     ++case_index) {
                    if (case_index != 0) {
                        out << ",";
                    }
                    const auto& route_case = route.cases[case_index];
                    out << "{";
                    out << "\"id\":";
                    append_json_string(out, route_case.id);
                    out << ",\"when\":"
                        << (route_case.when ? "true" : "false");
                    out << ",\"activate_subgraphs\":";
                    append_json_string_array(out, route_case.activate_subgraphs);
                    out << ",\"decisions\":" << route_case.decisions;
                    out << "}";
                }
                out << "]}";
            }
            out << "],\"outputs\":[";
            for (std::size_t i = 0; i < dynamic.outputs.size(); ++i) {
                if (i != 0) {
                    out << ",";
                }
                const auto& output = dynamic.outputs[i];
                out << "{";
                out << "\"id\":";
                append_json_string(out, output.id);
                out << ",\"node_id\":";
                append_json_string(out, output.node_id);
                out << ",\"owning_subgraph\":";
                append_json_string(out, output.owning_subgraph);
                out << ",\"active_rows\":" << output.active_rows;
                out << ",\"inactive_rows\":" << output.inactive_rows;
                out << "}";
            }
            out << "],\"decision_samples\":[";
            for (std::size_t i = 0; i < dynamic.decision_samples.size(); ++i) {
                if (i != 0) {
                    out << ",";
                }
                const auto& sample = dynamic.decision_samples[i];
                out << "{";
                out << "\"row\":" << sample.row;
                out << ",\"route_id\":";
                append_json_string(out, sample.route_id);
                out << ",\"selected_case_id\":";
                append_json_string(out, sample.selected_case_id);
                out << ",\"fallback_used\":"
                    << (sample.fallback_used ? "true" : "false");
                out << ",\"condition_value\":"
                    << (sample.condition_value ? "true" : "false");
                out << ",\"active_subgraphs\":";
                append_json_string_array(out, sample.active_subgraphs);
                out << "}";
            }
            out << "]}";
        }

        void append_json_objective_components(
            std::ostream& out,
            const RunObjectiveSummary& objective) {
            out << "[";
            for (std::size_t i = 0; i < objective.components.size(); ++i) {
                if (i != 0) {
                    out << ",";
                }
                const auto& component = objective.components[i];
                out << "{";
                out << "\"role\":";
                append_json_string(out, run_objective_component_role_name(component.role));
                out << ",\"source\":";
                append_json_string(out, objective_value_source_name(component.value.source));
                out << ",\"name\":";
                append_json_string(out, component.value.name);
                out << ",\"direction\":";
                append_json_string(out, objective_direction_name(component.direction));
                out << ",\"weight\":";
                append_json_number(out, component.weight);
                out << ",\"present\":" << (component.present ? "true" : "false");
                out << ",\"finite\":" << (component.finite ? "true" : "false");
                out << ",\"raw_value\":";
                append_json_number(out, component.raw_value);
                out << ",\"score_contribution\":";
                append_json_number(out, component.score_contribution);
                out << "}";
            }
            out << "]";
        }

        void append_json_objective_value_spec(
            std::ostream& out,
            const ObjectiveValueSpec& value) {
            out << "{";
            out << "\"source\":";
            append_json_string(out, objective_value_source_name(value.source));
            out << ",\"name\":";
            append_json_string(out, value.name);
            out << ",\"summary\":";
            append_json_string(out, numeric_node_summary_mode_name(value.summary_mode));
            out << "}";
        }

        void append_json_pareto_objective(
            std::ostream& out,
            const ObjectiveVectorComponentSpec& objective) {
            out << "{";
            out << "\"label\":";
            append_json_string(out, objective_vector_component_label(objective));
            out << ",\"value\":";
            append_json_objective_value_spec(out, objective.value);
            out << ",\"direction\":";
            append_json_string(out, objective_direction_name(objective.direction));
            out << "}";
        }

        void append_json_range_block(
            std::ostream& out,
            bool has_value,
            double min_value,
            double max_value,
            double range) {
            out << "{";
            out << "\"has_value\":" << (has_value ? "true" : "false");
            out << ",\"min\":";
            if (has_value) {
                append_json_number(out, min_value);
            }
            else {
                out << "null";
            }
            out << ",\"max\":";
            if (has_value) {
                append_json_number(out, max_value);
            }
            else {
                out << "null";
            }
            out << ",\"range\":";
            if (has_value) {
                append_json_number(out, range);
            }
            else {
                out << "null";
            }
            out << "}";
        }

        void append_json_pareto_objective_range(
            std::ostream& out,
            const SweepParetoObjectiveRange& range) {
            out << "{";
            out << "\"label\":";
            append_json_string(out, range.label);
            out << ",\"raw_non_dominated\":";
            append_json_range_block(
                out,
                range.raw_non_dominated_has_value,
                range.raw_non_dominated_min,
                range.raw_non_dominated_max,
                range.raw_non_dominated_range);
            out << ",\"emitted_front\":";
            append_json_range_block(
                out,
                range.emitted_front_has_value,
                range.emitted_front_min,
                range.emitted_front_max,
                range.emitted_front_range);
            out << "}";
        }

        void append_json_pareto_summary(
            std::ostream& out,
            const SweepParetoSummary& pareto) {
            out << "{";
            out << "\"requested\":" << (pareto.requested ? "true" : "false");
            out << ",\"computed\":" << (pareto.computed ? "true" : "false");
            out << ",\"source\":";
            append_json_string(out, sweep_pareto_summary_source_name(pareto.source));
            out << ",\"observed_candidate_count\":"
                << pareto.observed_candidate_count;
            out << ",\"unique_candidate_count\":" << pareto.unique_candidate_count;
            out << ",\"duplicate_observation_count\":"
                << pareto.duplicate_observation_count;
            out << ",\"retained_candidate_pool\":" << pareto.retained_candidate_pool;
            out << ",\"finite_candidate_count\":" << pareto.finite_candidate_count;
            out << ",\"incomplete_candidate_count\":"
                << pareto.incomplete_candidate_count;
            out << ",\"dominated_candidate_count\":"
                << pareto.dominated_candidate_count;
            out << ",\"non_dominated_candidate_count\":"
                << pareto.non_dominated_candidate_count;
            out << ",\"emitted_front_row_count\":"
                << pareto.emitted_front_row_count;
            out << ",\"front_row_limit\":" << pareto.front_row_limit;
            out << ",\"front_rows_capped\":"
                << (pareto.front_rows_capped ? "true" : "false");
            out << ",\"output_depollution_requested\":"
                << (pareto.output_depollution_requested ? "true" : "false");
            out << ",\"output_depollution_source\":";
            append_json_string(
                out,
                sweep_pareto_output_depollution_source_name(
                    pareto.output_depollution_source));
            out << ",\"output_depollution_used_result_diversity\":"
                << (pareto.output_depollution_used_result_diversity ? "true" : "false");
            out << ",\"output_depollution_dedupe_trade_hash\":"
                << (pareto.output_depollution_dedupe_trade_hash ? "true" : "false");
            out << ",\"output_depollution_basin_aware\":"
                << (pareto.output_depollution_basin_aware ? "true" : "false");
            out << ",\"output_depollution_max_per_basin\":"
                << pareto.output_depollution_max_per_basin;
            out << ",\"output_depollution_param_bucket_size\":"
                << pareto.output_depollution_param_bucket_size;
            out << ",\"output_depollution_min_param_distance\":"
                << pareto.output_depollution_min_param_distance;
            out << ",\"output_depollution_min_trades\":"
                << pareto.output_depollution_min_trades;
            out << ",\"emitted_unique_trade_hashes\":"
                << pareto.emitted_unique_trade_hashes;
            out << ",\"emitted_duplicate_trade_hash_groups\":"
                << pareto.emitted_duplicate_trade_hash_groups;
            out << ",\"emitted_largest_trade_hash_group\":"
                << pareto.emitted_largest_trade_hash_group;
            out << ",\"emitted_basin_count\":"
                << pareto.emitted_basin_count;
            out << ",\"suppressed_duplicate_trade_hash\":"
                << pareto.suppressed_duplicate_trade_hash;
            out << ",\"suppressed_basin\":" << pareto.suppressed_basin;
            out << ",\"suppressed_min_trades\":"
                << pareto.suppressed_min_trades;
            out << ",\"suppressed_row_limit\":"
                << pareto.suppressed_row_limit;
            out << ",\"primary_sort_index\":" << pareto.primary_sort_index;
            out << ",\"primary_sort_objective\":";
            if (!pareto.objectives.empty()) {
                append_json_pareto_objective(
                    out,
                    pareto.objectives[std::min(
                        pareto.primary_sort_index,
                        pareto.objectives.size() - 1)]);
            }
            else {
                out << "null";
            }
            out << ",\"objectives\":[";
            for (std::size_t i = 0; i < pareto.objectives.size(); ++i) {
                if (i != 0) {
                    out << ",";
                }
                append_json_pareto_objective(out, pareto.objectives[i]);
            }
            out << "],\"objective_ranges\":[";
            for (std::size_t i = 0; i < pareto.objective_ranges.size(); ++i) {
                if (i != 0) {
                    out << ",";
                }
                append_json_pareto_objective_range(out, pareto.objective_ranges[i]);
            }
            out << "],\"front\":[";
            for (std::size_t i = 0; i < pareto.front.size(); ++i) {
                if (i != 0) {
                    out << ",";
                }
                const auto& candidate = pareto.front[i];
                out << "{";
                out << "\"front_rank\":" << candidate.front_rank;
                out << ",\"candidate_index\":" << candidate.candidate_index;
                out << ",\"scalar_score\":";
                append_json_number(out, candidate.scalar_score);
                out << ",\"dominates_count\":" << candidate.dominates_count;
                out << ",\"objective_values\":[";
                for (std::size_t j = 0; j < candidate.objective_values.size(); ++j) {
                    if (j != 0) {
                        out << ",";
                    }
                    const auto& value = candidate.objective_values[j];
                    out << "{";
                    out << "\"present\":" << (value.present ? "true" : "false");
                    out << ",\"finite\":" << (value.finite ? "true" : "false");
                    out << ",\"value\":";
                    append_json_number(out, value.value);
                    out << "}";
                }
                out << "],\"params\":";
                append_json_parameter_settings(out, candidate.parameter_values);
                out << "}";
            }
            out << "]}";
        }

        void append_json_posterior_proposal_reason(
            std::ostream& out,
            const SweepPosteriorProposalReason& reason) {
            out << "{";
            out << "\"proposal_order\":" << reason.proposal_order;
            out << ",\"candidate_index\":" << reason.candidate_index;
            out << ",\"acquisition_score_present\":"
                << (reason.acquisition_score_present ? "true" : "false");
            out << ",\"acquisition_score_finite\":"
                << (reason.acquisition_score_finite ? "true" : "false");
            out << ",\"acquisition_score\":";
            if (reason.acquisition_score_present &&
                reason.acquisition_score_finite) {
                append_json_number(out, reason.acquisition_score);
            }
            else {
                out << "null";
            }
            out << ",\"reason\":";
            append_json_string(out, reason.reason);
            out << "}";
        }

        void append_json_posterior_summary(
            std::ostream& out,
            const SweepPosteriorDiagnosticsSummary& posterior) {
            out << "{";
            out << "\"requested\":"
                << (posterior.requested ? "true" : "false");
            out << ",\"computed\":"
                << (posterior.computed ? "true" : "false");
            out << ",\"status\":";
            append_json_string(
                out,
                sweep_posterior_diagnostics_status_name(posterior.status));
            out << ",\"unsupported_reason\":";
            append_json_string(out, posterior.unsupported_reason);
            out << ",\"surrogate\":";
            append_json_string(
                out,
                plan_sweep_posterior_surrogate_kind_name(
                    posterior.surrogate));
            out << ",\"acquisition\":";
            append_json_string(
                out,
                plan_sweep_posterior_acquisition_kind_name(
                    posterior.acquisition));
            out << ",\"initial_design_size\":"
                << posterior.initial_design_size;
            out << ",\"iterations\":" << posterior.iterations;
            out << ",\"seed\":" << posterior.seed;
            out << ",\"exploration_weight\":";
            append_json_number(out, posterior.exploration_weight);
            out << ",\"normalize_inputs\":"
                << (posterior.normalize_inputs ? "true" : "false");
            out << ",\"top_k\":" << posterior.top_k;
            out << ",\"surrogate_trained\":"
                << (posterior.surrogate_trained ? "true" : "false");
            out << ",\"training_observation_count\":"
                << posterior.training_observation_count;
            out << ",\"finite_observation_count\":"
                << posterior.finite_observation_count;
            out << ",\"surrogate_training_observation_count\":"
                << posterior.surrogate_training_observation_count;
            out << ",\"invalid_training_observation_count\":"
                << posterior.invalid_training_observation_count;
            out << ",\"acquisition_ready\":"
                << (posterior.acquisition_ready ? "true" : "false");
            out << ",\"diagnostics\":";
            append_json_string_array(out, posterior.diagnostics);
            out << ",\"proposal_reasons\":[";
            for (std::size_t i = 0; i < posterior.proposal_reasons.size(); ++i) {
                if (i != 0) {
                    out << ",";
                }
                append_json_posterior_proposal_reason(
                    out,
                    posterior.proposal_reasons[i]);
            }
            out << "]}";
        }

        void append_json_run_report(std::ostream& out, const RunReport& report) {
            out << "{";
            out << "\"outcome\":";
            append_json_string(out, run_outcome_state_name(report.outcome));
            out << ",\"execution\":{";
            out << "\"pnl\":";
            append_json_number(out, report.execution.pnl);
            out << ",\"trades\":" << report.execution.trades;
            out << ",\"max_drawdown\":";
            append_json_number(out, report.execution.max_drawdown);
            out << ",\"trade_hash\":" << report.execution.trade_hash;
            out << ",\"rows_evaluated\":" << report.execution.rows_evaluated;
            out << ",\"warmup_rows\":" << report.execution.warmup_rows;
            if (report.execution.dynamic_execution.enabled) {
                out << ",\"dynamic_execution\":";
                append_json_dynamic_execution(
                    out,
                    report.execution.dynamic_execution);
            }
            out << "},\"objective\":{";
            out << "\"requested\":" << (report.objective.requested ? "true" : "false");
            out << ",\"rankability\":";
            append_json_string(out, run_rankability_state_name(report.objective.rankability));
            out << ",\"score\":";
            append_json_number(out, report.objective.score);
            out << ",\"components\":";
            append_json_objective_components(out, report.objective);
            out << "},";
            append_json_metric_catalog(out, report.metric_catalog);
            if (run_provenance_has_data(report.provenance)) {
                out << ",\"provenance\":";
                append_json_run_provenance(out, report.provenance);
            }
            out << "}";
        }

        void append_json_sweep_range(
            std::ostream& out,
            const SweepParameterRangeSummary& range) {
            out << "{";
            out << "\"name\":";
            append_json_string(out, range.name);
            out << ",\"expanded_count\":" << range.expanded_count;
            out << ",\"is_int\":"
                << (range.requested_range.is_int ? "true" : "false");
            out << ",\"min\":";
            append_json_number(out, range.requested_range.min);
            out << ",\"max\":";
            append_json_number(out, range.requested_range.max);
            out << ",\"step\":";
            append_json_number(out, range.requested_range.step);
            out << ",\"values\":[";
            for (std::size_t i = 0; i < range.requested_range.values.size(); ++i) {
                if (i != 0) {
                    out << ",";
                }
                append_json_number(out, range.requested_range.values[i]);
            }
            out << "]}";
        }

        void append_json_sweep_stage(
            std::ostream& out,
            const SweepStageSummary& stage) {
            out << "{";
            out << "\"stage\":";
            append_json_string(out, sweep_progress_stage_name(stage.stage));
            out << ",\"done\":" << stage.done;
            out << ",\"total\":" << stage.total;
            out << "}";
        }

        void append_json_sweep_trace_seed(
            std::ostream& out,
            const SweepTraceSeedRecord& seed) {
            out << "{";
            out << "\"source\":";
            append_json_string(out, seed.source);
            out << ",\"stream_name\":";
            append_json_string(out, seed.stream_name);
            out << ",\"requested_seed_explicit\":"
                << (seed.requested_seed_explicit ? "true" : "false");
            out << ",\"requested_seed\":" << seed.requested_seed;
            out << ",\"effective_seed\":" << seed.effective_seed;
            out << ",\"deterministic\":"
                << (seed.deterministic ? "true" : "false");
            out << "}";
        }

        const char* sweep_trace_policy_config_value_kind_name(
            SweepTracePolicyConfigValueKind kind) {
            switch (kind) {
            case SweepTracePolicyConfigValueKind::String:
                return "string";
            case SweepTracePolicyConfigValueKind::Boolean:
                return "boolean";
            case SweepTracePolicyConfigValueKind::UnsignedInteger:
                return "u64";
            case SweepTracePolicyConfigValueKind::SignedInteger:
                return "i64";
            case SweepTracePolicyConfigValueKind::FloatingPoint:
                return "f64";
            }
            return "string";
        }

        void append_json_sweep_trace_policy_config_value(
            std::ostream& out,
            const SweepTracePolicyConfigEntry& entry) {
            switch (entry.value_kind) {
            case SweepTracePolicyConfigValueKind::Boolean:
                out << (entry.value == "true" ? "true" : "false");
                break;
            case SweepTracePolicyConfigValueKind::UnsignedInteger:
            case SweepTracePolicyConfigValueKind::SignedInteger:
            case SweepTracePolicyConfigValueKind::FloatingPoint:
                out << entry.value;
                break;
            case SweepTracePolicyConfigValueKind::String:
            default:
                append_json_string(out, entry.value);
                break;
            }
        }

        void append_json_sweep_trace_policy_config(
            std::ostream& out,
            const SweepTracePolicyConfigEntry& entry) {
            out << "{";
            out << "\"name\":";
            append_json_string(out, entry.name);
            out << ",\"value_type\":";
            append_json_string(
                out,
                sweep_trace_policy_config_value_kind_name(entry.value_kind));
            out << ",\"value\":";
            append_json_sweep_trace_policy_config_value(out, entry);
            out << "}";
        }

        void append_json_sweep_trace_event(
            std::ostream& out,
            const SweepTraceEvent& event) {
            out << "{";
            out << "\"kind\":";
            append_json_string(out, sweep_trace_event_kind_name(event.kind));
            out << ",\"stage\":";
            append_json_string(out, sweep_progress_stage_name(event.stage));
            out << ",\"candidate_index\":" << event.candidate_index;
            out << ",\"proposal_order\":" << event.proposal_order;
            out << ",\"batch_index\":" << event.batch_index;
            out << ",\"has_parent_candidate\":"
                << (event.has_parent_candidate ? "true" : "false");
            out << ",\"parent_candidate_index\":";
            if (event.has_parent_candidate) {
                out << event.parent_candidate_index;
            }
            else {
                out << "null";
            }
            out << ",\"score_present\":"
                << (event.score_present ? "true" : "false");
            out << ",\"score_finite\":"
                << (event.score_finite ? "true" : "false");
            out << ",\"score\":";
            if (event.score_present && event.score_finite) {
                append_json_number(out, event.score);
            }
            else {
                out << "null";
            }
            out << ",\"run_outcome_present\":"
                << (event.run_outcome_present ? "true" : "false");
            out << ",\"run_outcome\":";
            if (event.run_outcome_present) {
                append_json_string(out, run_outcome_state_name(event.run_outcome));
            }
            else {
                out << "null";
            }
            out << ",\"run_rankability_present\":"
                << (event.run_rankability_present ? "true" : "false");
            out << ",\"run_rankability\":";
            if (event.run_rankability_present) {
                append_json_string(
                    out,
                    run_rankability_state_name(event.run_rankability));
            }
            else {
                out << "null";
            }
            out << ",\"reason\":";
            append_json_string(out, event.reason);
            out << "}";
        }

        void append_json_sweep_proposal_trace(
            std::ostream& out,
            const SweepTraceSummary& trace) {
            const auto digest_hex = [](std::uint64_t digest) {
                std::ostringstream text;
                text << "0x" << std::hex << std::setw(16)
                    << std::setfill('0') << digest;
                return text.str();
            };

            out << "{";
            out << "\"captured\":" << (trace.captured ? "true" : "false");
            out << ",\"policy_name\":";
            append_json_string(out, trace.policy_name);
            out << ",\"effective_policy_config\":[";
            for (std::size_t i = 0; i < trace.effective_policy_config.size(); ++i) {
                if (i != 0) {
                    out << ",";
                }
                append_json_sweep_trace_policy_config(
                    out,
                    trace.effective_policy_config[i]);
            }
            out << "]";
            out << ",\"event_limit\":" << trace.event_limit;
            out << ",\"total_event_count\":" << trace.total_event_count;
            out << ",\"proposal_count\":" << trace.proposal_count;
            out << ",\"observation_count\":" << trace.observation_count;
            out << ",\"promotion_count\":" << trace.promotion_count;
            out << ",\"rejection_count\":" << trace.rejection_count;
            out << ",\"events_capped\":"
                << (trace.events_capped ? "true" : "false");
            out << ",\"full_event_stream_captured\":"
                << (trace.full_event_stream_captured ? "true" : "false");
            out << ",\"trace_digest_available\":"
                << (trace.trace_digest_available ? "true" : "false");
            out << ",\"trace_digest\":";
            if (trace.trace_digest_available) {
                append_json_string(out, digest_hex(trace.trace_digest));
            }
            else {
                out << "null";
            }
            out << ",\"event_order_valid\":"
                << (trace.event_order_valid ? "true" : "false");
            out << ",\"event_order_violations\":";
            append_json_string_array(out, trace.event_order_violations);
            out << ",\"observation_payload_complete\":"
                << (trace.observation_payload_complete ? "true" : "false");
            out << ",\"observation_payload_gaps\":";
            append_json_string_array(out, trace.observation_payload_gaps);
            out << ",\"exact_replay_from_report\":"
                << (trace.exact_replay_from_report ? "true" : "false");
            out << ",\"non_replayable_reasons\":";
            append_json_string_array(out, trace.non_replayable_reasons);
            out << ",\"seeds\":[";
            for (std::size_t i = 0; i < trace.seeds.size(); ++i) {
                if (i != 0) {
                    out << ",";
                }
                append_json_sweep_trace_seed(out, trace.seeds[i]);
            }
            out << "],\"events\":[";
            for (std::size_t i = 0; i < trace.events.size(); ++i) {
                if (i != 0) {
                    out << ",";
                }
                append_json_sweep_trace_event(out, trace.events[i]);
            }
            out << "]}";
        }

        void append_json_run_outcome_counts(
            std::ostream& out,
            const RunOutcomeCounts& counts) {
            out << "{";
            out << "\"completed\":" << counts.completed;
            out << ",\"completed_with_non_finite_metrics\":"
                << counts.completed_with_non_finite_metrics;
            out << ",\"failed\":" << counts.failed;
            out << ",\"canceled\":" << counts.canceled;
            out << "}";
        }

        void append_json_run_rankability_counts(
            std::ostream& out,
            const RunRankabilityCounts& counts) {
            out << "{";
            out << "\"not_evaluated\":" << counts.not_evaluated;
            out << ",\"rankable\":" << counts.rankable;
            out << ",\"missing_primary_value\":" << counts.missing_primary_value;
            out << ",\"non_finite_primary_value\":"
                << counts.non_finite_primary_value;
            out << ",\"missing_penalty_value\":" << counts.missing_penalty_value;
            out << ",\"non_finite_penalty_value\":"
                << counts.non_finite_penalty_value;
            out << "}";
        }

        void append_json_result_diversity(
            std::ostream& out,
            const SweepResultDiversitySummary& diversity) {
            out << "{";
            out << "\"requested\":" << (diversity.requested ? "true" : "false");
            out << ",\"dedupe_trade_hash\":"
                << (diversity.dedupe_trade_hash ? "true" : "false");
            out << ",\"basin_aware\":"
                << (diversity.basin_aware ? "true" : "false");
            out << ",\"oversample_multiplier\":"
                << diversity.oversample_multiplier;
            out << ",\"max_per_basin\":" << diversity.max_per_basin;
            out << ",\"param_bucket_size\":" << diversity.param_bucket_size;
            out << ",\"min_param_distance\":" << diversity.min_param_distance;
            out << ",\"min_trades\":" << diversity.min_trades;
            out << ",\"retained_candidate_pool\":"
                << diversity.retained_candidate_pool;
            out << ",\"returned_unique_trade_hashes\":"
                << diversity.returned_unique_trade_hashes;
            out << ",\"returned_duplicate_trade_hash_groups\":"
                << diversity.returned_duplicate_trade_hash_groups;
            out << ",\"returned_largest_trade_hash_group\":"
                << diversity.returned_largest_trade_hash_group;
            out << ",\"returned_basin_count\":" << diversity.returned_basin_count;
            out << ",\"suppressed_duplicate_trade_hash\":"
                << diversity.suppressed_duplicate_trade_hash;
            out << ",\"suppressed_basin\":" << diversity.suppressed_basin;
            out << ",\"suppressed_min_trades\":" << diversity.suppressed_min_trades;
            out << "}";
        }

        void append_json_candidate(
            std::ostream& out,
            const SweepCandidateReport& candidate) {
            out << "{";
            out << "\"rank\":" << candidate.rank;
            out << ",\"candidate_index\":" << candidate.candidate_index;
            out << ",\"score\":";
            append_json_number(out, candidate.score);
            out << ",\"params\":";
            append_json_parameter_settings(out, candidate.parameter_values);
            out << ",\"diagnostics\":";
            append_json_string_array(out, candidate.diagnostics);
            out << ",\"run\":";
            append_json_run_report(out, candidate.run);
            out << "}";
        }

        void append_json_sweep_report(std::ostream& out, const SweepReport& report) {
            const auto candidates = sweep_candidate_reports_for(report);
            out << "{";
            out << "\"schema_version\":";
            append_json_string(out, evaluation_report_schema_version());
            out << ",\"kind\":\"sweep\"";
            out << ",\"engine\":";
            append_json_string(out, sweep_engine_name(report.requested_sweep.engine));
            out << ",\"objective\":";
            append_json_string(out, describe_ranking_objective(report.resolved_objective));
            out << ",\"parameter_count\":" << report.parameter_count;
            out << ",\"total_combinations\":" << report.total_combinations;
            out << ",\"total_combinations_overflow\":"
                << (report.total_combinations_overflow ? "true" : "false");
            out << ",\"candidate_space_size\":" << report.candidate_space_size;
            out << ",\"candidate_space_truncated\":"
                << (report.candidate_space_truncated ? "true" : "false");
            out << ",\"evaluations_run\":" << report.evaluations_run;
            out << ",\"compile_failures\":" << report.compile_failures;
            out << ",\"execution_failures\":" << report.execution_failures;
            out << ",\"canceled\":" << (report.canceled ? "true" : "false");
            out << ",\"last_compile_error\":";
            append_json_string(out, report.last_compile_error);
            out << ",\"last_execution_failure\":";
            append_json_string(out, report.last_execution_failure);
            out << ",\"warnings\":";
            append_json_string_array(out, report.warnings);
            out << ",\"parameter_ranges\":[";
            for (std::size_t i = 0; i < report.parameter_ranges.size(); ++i) {
                if (i != 0) {
                    out << ",";
                }
                append_json_sweep_range(out, report.parameter_ranges[i]);
            }
            out << "],\"stage_summaries\":[";
            for (std::size_t i = 0; i < report.stage_summaries.size(); ++i) {
                if (i != 0) {
                    out << ",";
                }
                append_json_sweep_stage(out, report.stage_summaries[i]);
            }
            out << "],\"proposal_trace\":";
            append_json_sweep_proposal_trace(out, report.proposal_trace);
            out << ",\"candidate_outcomes\":";
            append_json_run_outcome_counts(out, report.candidate_outcomes);
            out << ",\"candidate_rankability\":";
            append_json_run_rankability_counts(out, report.candidate_rankability);
            out << ",\"result_diversity\":";
            append_json_result_diversity(out, report.result_diversity);
            out << ",\"search_geometry\":";
            append_json_sweep_geometry(out, report.search_geometry);
            out << ",\"pareto\":";
            append_json_pareto_summary(out, report.pareto);
            out << ",\"posterior\":";
            append_json_posterior_summary(out, report.posterior);
            out << ",\"top_k\":" << candidates.size();
            out << ",\"candidates\":[";
            for (std::size_t i = 0; i < candidates.size(); ++i) {
                if (i != 0) {
                    out << ",";
                }
                append_json_candidate(out, candidates[i]);
            }
            out << "]}";
        }

        void append_json_panel_report(std::ostream& out, const PanelReport& report) {
            out << "{";
            out << "\"schema_version\":";
            append_json_string(out, evaluation_report_schema_version());
            out << ",\"kind\":\"panel\"";
            out << ",\"entities_total\":" << report.entities_total;
            out << ",\"entities_ok\":" << report.entities_ok;
            out << ",\"entities_failed\":" << report.entities_failed;
            out << ",\"ranking_objective\":";
            append_json_string(out, describe_ranking_objective(report.resolved_objective));
            out << ",\"top_rows\":[";
            for (std::size_t i = 0; i < report.top_rows.size(); ++i) {
                if (i != 0) {
                    out << ",";
                }
                const auto& row = report.top_rows[i];
                out << "{";
                out << "\"rank\":" << (i + 1);
                out << ",\"series_key\":";
                append_json_string(out, row.series_key);
                out << ",\"entity_key\":";
                append_json_string(out, row.entity_key);
                out << ",\"ranking_score\":";
                append_json_number(out, row.ranking_score);
                out << ",\"run\":";
                append_json_run_report(out, row.run);
                out << "}";
            }
            out << "]}";
        }
    } // namespace

    void append_json_evaluation_report(
        std::ostream& out,
        const EvaluationReport& report) {
        switch (report.kind) {
        case EvaluationReportKind::Run:
            out << "{\"schema_version\":";
            append_json_string(out, evaluation_report_schema_version());
            out << ",\"kind\":\"run\",\"run\":";
            append_json_run_report(out, report.run.value_or(RunReport{}));
            out << "}";
            return;
        case EvaluationReportKind::Sweep:
            append_json_sweep_report(out, report.sweep.value_or(SweepReport{}));
            return;
        case EvaluationReportKind::Panel:
            append_json_panel_report(out, report.panel.value_or(PanelReport{}));
            return;
        case EvaluationReportKind::None:
        case EvaluationReportKind::Ensemble:
        case EvaluationReportKind::DynamicDag:
            out << "{\"schema_version\":";
            append_json_string(out, evaluation_report_schema_version());
            out << ",\"kind\":";
            append_json_string(out, evaluation_report_kind_name(report.kind));
            out << "}";
            return;
        }
    }

} // namespace sugar::evaluation_report_internal
