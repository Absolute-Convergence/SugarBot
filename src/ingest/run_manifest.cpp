#include "ingest/run_manifest.h"

#include <algorithm>
#include <cstdint>
#include <span>
#include <utility>

#include "ingest/observation_capability.h"
#include "sweep/plan_capability.h"
#include "sweep/plan_spec.h"

namespace sugar {

    namespace {
        bool has_intraday_component(std::span<const TimeKey> t) {
            for (const TimeKey tk : t) {
                const auto sec = static_cast<std::int64_t>(tk % 86400);
                if (sec != 0) return true;
            }
            return false;
        }

        const TopologySpec* find_topology_snapshot(
            const RunManifest& manifest,
            std::string_view name) {
            const auto it = std::find_if(
                manifest.topologies.begin(),
                manifest.topologies.end(),
                [&](const TopologySpec& spec) { return spec.name == name; });
            return it == manifest.topologies.end() ? nullptr : &*it;
        }

        void upsert_capability_outcome(
            std::vector<CapabilityOutcomeSnapshot>& outcomes,
            CapabilityOutcomeSnapshot outcome) {
            const auto it = std::find_if(
                outcomes.begin(),
                outcomes.end(),
                [&](const CapabilityOutcomeSnapshot& existing) {
                    return existing.domain == outcome.domain &&
                        existing.subject == outcome.subject;
                });
            if (it != outcomes.end()) {
                *it = std::move(outcome);
                return;
            }
            outcomes.push_back(std::move(outcome));
        }

        void upsert_topology_usage(
            std::vector<TopologyUsageSnapshot>& usages,
            TopologyUsageSnapshot usage) {
            const auto it = std::find_if(
                usages.begin(),
                usages.end(),
                [&](const TopologyUsageSnapshot& existing) {
                    return existing.subject_kind == usage.subject_kind &&
                        existing.subject_id == usage.subject_id &&
                        existing.op_name == usage.op_name &&
                        existing.topology_name == usage.topology_name;
                });
            if (it != usages.end()) {
                *it = std::move(usage);
                return;
            }
            usages.push_back(std::move(usage));
        }

        std::vector<std::string> topology_family_names(
            const std::vector<TopologyFamily>& families) {
            std::vector<std::string> out;
            out.reserve(families.size());
            for (const auto family : families) {
                out.emplace_back(topology_family_name(family));
            }
            return out;
        }

        std::vector<std::string> topology_query_kind_names(
            const std::vector<TopologyQueryKind>& kinds) {
            std::vector<std::string> out;
            out.reserve(kinds.size());
            for (const auto kind : kinds) {
                out.emplace_back(topology_query_kind_name(kind));
            }
            return out;
        }

        bool node_has_unresolved_topology_reason(const PlanNodeCapability& node) {
            for (const auto& reason : node.reasons) {
                switch (reason.code) {
                case PlanCapabilityReasonCode::MissingNamedTopology:
                case PlanCapabilityReasonCode::MissingRequiredTopologyFamily:
                case PlanCapabilityReasonCode::MissingRequiredTopologyQueryKind:
                case PlanCapabilityReasonCode::IncompatibleTopologyFamily:
                case PlanCapabilityReasonCode::TopologyResolutionFailed:
                    return true;
                default:
                    break;
                }
            }
            return false;
        }
    } // namespace

    std::string infer_time_resolution(TimeKind kind) {
        switch (kind) {
        case TimeKind::YyyyMmDd:
            return "calendar_date_yyyymmdd";
        case TimeKind::Iso8601:
            return "utc_epoch_seconds";
        case TimeKind::YearQuarter:
            return "year_quarter";
        case TimeKind::YearMonth:
            return "year_month";
        case TimeKind::Index:
            return "index_ordinal";
        }
        return "unknown";
    }

    std::string_view runtime_primary_selection_mode_name(RuntimePrimarySelectionMode mode) {
        switch (mode) {
        case RuntimePrimarySelectionMode::Unknown:
            return "unknown";
        case RuntimePrimarySelectionMode::SingleSeriesImplicit:
            return "single_series_implicit";
        case RuntimePrimarySelectionMode::ExplicitSeries:
            return "explicit_series";
        case RuntimePrimarySelectionMode::HeuristicCompatibility:
            return "heuristic_compatibility";
        case RuntimePrimarySelectionMode::LegacyCompatibility:
            return "legacy_compatibility";
        }
        return "unknown";
    }

    PreprocessReport make_preprocess_report(
        const DataSpec& spec,
        const PreprocessResult& prep,
        const std::optional<align::AlignConfig>& align_cfg) {
        PreprocessReport out{};
        out.data_spec = spec;
        out.missing_policy = spec.missing_default;
        out.dedupe_policy = spec.dedupe;
        out.has_time_axis = data_spec_has_time_axis(spec);
        out.has_spatial_axis = data_spec_has_spatial_axis(spec);
        out.time_kind = spec.time_kind;
        out.time_resolution = out.has_time_axis ? infer_time_resolution(spec.time_kind) : "none";
        out.has_entity_axis = spec.entity_col.has_value();
        out.input_format = prep.input_format;
        out.input_row_count = prep.input_row_count;
        out.csv_had_header = prep.csv_had_header;
        out.parsed_columns = prep.parsed_columns;
        out.csv_physical_line_count = prep.csv_load.physical_line_count;
        out.csv_blank_line_count = prep.csv_load.blank_line_count;
        out.csv_data_row_count = prep.csv_load.data_row_count;
        out.dropped_row_count = prep.dropped_row_count;
        out.value_column_quality = prep.value_column_quality;
        out.input_warnings = prep.warnings;
        out.preprocess_error_code = prep.error_code;
        out.preprocess_error_message = prep.err;
        out.preprocess_error_aggregates = prep.error_aggregates;

        if (prep.frameset.has_value()) {
            out.produced_frameset = true;
            out.series_keys.reserve(prep.frameset->by_entity.size());
            for (const auto& kv : prep.frameset->by_entity) {
                out.series_keys.push_back(kv.first);
            }
            std::sort(out.series_keys.begin(), out.series_keys.end());
        }
        else if (prep.frame.has_value()) {
            out.produced_frameset = false;
            out.series_keys.push_back("__single__");
        }

        if (out.has_time_axis && spec.time_kind == TimeKind::Iso8601) {
            bool intraday = false;
            if (prep.frame.has_value()) {
                intraday = has_intraday_component(prep.frame->time());
            }
            else if (prep.frameset.has_value()) {
                for (const auto& kv : prep.frameset->by_entity) {
                    if (has_intraday_component(kv.second.time())) {
                        intraday = true;
                        break;
                    }
                }
            }
            out.time_resolution = intraday
                ? "utc_epoch_seconds_intraday"
                : "utc_epoch_seconds_day_boundary";
        }

        if (align_cfg.has_value()) {
            out.alignment_applied = true;
            out.alignment_calendar = align_cfg->calendar;
            out.alignment_missing = align_cfg->missing;
            out.alignment_non_temporal_multi_series = align_cfg->non_temporal_multi_series;
        }

        return out;
    }

    RunManifest make_run_manifest(
        const PreprocessReport& preprocess,
        std::string primary_series_key,
        std::shared_ptr<const TopologyCatalog> topology_catalog,
        RuntimePrimarySelectionMode primary_selection_mode) {
        RunManifest out{};
        out.preprocess = preprocess;
        out.primary_series_key = std::move(primary_series_key);
        out.primary_selection_mode = primary_selection_mode;
        if (topology_catalog) {
            out.topologies = topology_catalog->snapshot();
        }
        return out;
    }

    void append_observation_capability_snapshot(
        const ObservationCapabilityReport& report,
        RunManifest& manifest) {
        manifest.capability_outcomes.erase(
            std::remove_if(
                manifest.capability_outcomes.begin(),
                manifest.capability_outcomes.end(),
                [](const CapabilityOutcomeSnapshot& outcome) {
                    return outcome.domain == "observation_surface";
                }),
            manifest.capability_outcomes.end());
        for (const auto& surface : report.surfaces) {
            CapabilityOutcomeSnapshot outcome{};
            outcome.domain = "observation_surface";
            outcome.subject = std::string(observation_surface_name(surface.surface));
            outcome.status = std::string(observation_capability_status_name(surface.status));
            outcome.reasons.reserve(surface.reasons.size());
            for (const auto reason : surface.reasons) {
                outcome.reasons.push_back(
                    CapabilityReasonSnapshot{
                        std::string(observation_capability_reason_code_name(reason)),
                        std::string(observation_capability_reason_text(reason))
                    });
            }
            upsert_capability_outcome(manifest.capability_outcomes, std::move(outcome));
        }
    }

    void append_plan_capability_snapshot(
        const PlanCapabilityReport& report,
        RunManifest& manifest) {
        manifest.capability_outcomes.erase(
            std::remove_if(
                manifest.capability_outcomes.begin(),
                manifest.capability_outcomes.end(),
                [](const CapabilityOutcomeSnapshot& outcome) {
                    return outcome.domain == "plan_report" ||
                        outcome.domain == "plan_node" ||
                        outcome.domain == "plan_binding";
                }),
            manifest.capability_outcomes.end());
        manifest.topology_usages.clear();
        upsert_capability_outcome(
            manifest.capability_outcomes,
            CapabilityOutcomeSnapshot{
                "plan_report",
                "plan",
                std::string(plan_capability_status_name(report.status)),
                {}
            });

        for (const auto& node : report.nodes) {
            CapabilityOutcomeSnapshot outcome{};
            outcome.domain = "plan_node";
            outcome.subject = node.node_id;
            outcome.status = std::string(plan_capability_status_name(node.status));
            outcome.reasons.reserve(node.reasons.size());
            for (const auto& reason : node.reasons) {
                outcome.reasons.push_back(
                    CapabilityReasonSnapshot{
                        std::string(plan_capability_reason_code_name(reason.code)),
                        plan_capability_reason_text(reason)
                    });
            }
            upsert_capability_outcome(manifest.capability_outcomes, std::move(outcome));

            const bool mentions_topology =
                !node.resolved_topology_name.empty() ||
                !node.default_topology_name.empty() ||
                !node.required_topology_families.empty() ||
                !node.required_topology_query_kinds.empty();
            if (!mentions_topology) {
                continue;
            }

            const std::string chosen_topology =
                !node.resolved_topology_name.empty()
                ? node.resolved_topology_name
                : node.default_topology_name;
            const TopologySpec* spec = chosen_topology.empty()
                ? nullptr
                : find_topology_snapshot(manifest, chosen_topology);

            TopologyUsageSnapshot usage{};
            usage.subject_kind = "plan_node";
            usage.subject_id = node.node_id;
            usage.op_name = node.op_name;
            usage.topology_name = chosen_topology;
            if (node.has_runtime_locality_subject) {
                usage.locality_subject_kind =
                    std::string(locality_subject_kind_name(node.runtime_locality_subject.kind));
                usage.locality_subject_value = node.runtime_locality_subject.value;
                usage.locality_subject_explicit = node.runtime_locality_subject.explicit_binding;
                usage.resolved_series_key = node.runtime_locality_subject.series_key;
                usage.resolved_entity_key = node.runtime_locality_subject.entity_key;
            }
            else if (node.has_locality_subject) {
                usage.locality_subject_kind =
                    std::string(locality_subject_kind_name(node.locality_subject.kind));
                usage.locality_subject_value = node.locality_subject.value;
                usage.locality_subject_explicit = node.locality_subject.explicit_binding;
            }
            usage.required_topology_families = topology_family_names(node.required_topology_families);
            usage.required_topology_query_kinds = topology_query_kind_names(node.required_topology_query_kinds);
            usage.catalog_present = spec != nullptr;
            usage.runtime_resolved = !chosen_topology.empty() && !node_has_unresolved_topology_reason(node);
            if (spec && spec->artifact_ref.has_value()) {
                usage.artifact_ref = *spec->artifact_ref;
            }
            if (spec) {
                usage.topology_origin = std::string(topology_origin_name(spec->origin));
                usage.topology_bearing = std::string(topology_bearing_name(spec->bearing));
            }
            upsert_topology_usage(manifest.topology_usages, std::move(usage));
        }

        for (const auto& binding : report.bindings) {
            CapabilityOutcomeSnapshot outcome{};
            outcome.domain = "plan_binding";
            outcome.subject = binding.binding_name;
            outcome.status = std::string(plan_capability_status_name(binding.status));
            outcome.reasons.reserve(binding.reasons.size());
            for (const auto& reason : binding.reasons) {
                outcome.reasons.push_back(
                    CapabilityReasonSnapshot{
                        std::string(plan_capability_reason_code_name(reason.code)),
                        plan_capability_reason_text(reason)
                    });
            }
            upsert_capability_outcome(manifest.capability_outcomes, std::move(outcome));
        }
    }

    void append_planspec_rng_seeds(const PlanSpec& spec, RunManifest& manifest) {
        if (!spec.sweep.has_value()) return;

        const auto& sw = *spec.sweep;
        if (sw.engine != PlanSweepEngineKind::MonteCarlo) return;

        manifest.rng_seeds.push_back(
            RngSeedRecord{
                "plan_sweep.monte_carlo",
                sw.monte_carlo.seed,
                sw.monte_carlo.seed != 0
            });
    }

} // namespace sugar
