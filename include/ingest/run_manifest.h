#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ingest/align.h"
#include "ingest/data_spec.h"
#include "ingest/preprocess.h"
#include "ingest/topology_spec.h"

namespace sugar {

    enum class RuntimePrimarySelectionMode {
        Unknown = 0,
        SingleSeriesImplicit = 1,
        ExplicitSeries = 2,
        HeuristicCompatibility = 3,
        LegacyCompatibility = 4,
    };

    struct PlanSpec;
    struct ObservationCapabilityReport;
    struct PlanCapabilityReport;

    struct RngSeedRecord {
        std::string component;
        std::uint64_t seed{ 0 };
        bool explicit_seed{ false };
    };

    struct CapabilityReasonSnapshot {
        std::string code{};
        std::string detail{};
    };

    struct CapabilityOutcomeSnapshot {
        std::string domain{};
        std::string subject{};
        std::string status{};
        std::vector<CapabilityReasonSnapshot> reasons{};
    };

    struct TopologyUsageSnapshot {
        std::string subject_kind{};
        std::string subject_id{};
        std::string op_name{};
        std::string topology_name{};
        std::string topology_origin{};
        std::string topology_bearing{};
        std::string locality_subject_kind{};
        std::string locality_subject_value{};
        bool locality_subject_explicit{ false };
        std::string resolved_series_key{};
        std::string resolved_entity_key{};
        std::vector<std::string> required_topology_families{};
        std::vector<std::string> required_topology_query_kinds{};
        bool catalog_present{ false };
        bool runtime_resolved{ false };
        TopologyArtifactRef artifact_ref{};
    };

    struct PreprocessReport {
        DataSpec data_spec{};
        MissingPolicy missing_policy{};
        DataSpec::DedupePolicy dedupe_policy{ DataSpec::DedupePolicy::Error };
        bool has_time_axis{ true };
        bool has_spatial_axis{ false };
        TimeKind time_kind{ TimeKind::YyyyMmDd };
        std::string time_resolution;
        std::vector<std::string> series_keys;
        bool produced_frameset{ false };
        bool has_entity_axis{ false };
        bool csv_had_header{ false };
        std::size_t csv_physical_line_count{ 0 };
        std::size_t csv_blank_line_count{ 0 };
        std::size_t csv_data_row_count{ 0 };
        std::size_t dropped_row_count{ 0 };
        std::vector<std::string> input_warnings{};
        bool alignment_applied{ false };
        align::CalendarPolicy alignment_calendar{ align::CalendarPolicy::Intersection };
        align::MissingPolicy alignment_missing{ align::MissingPolicy::NaN };
        align::NonTemporalMultiSeriesPolicy alignment_non_temporal_multi_series{
            align::NonTemporalMultiSeriesPolicy::Reject
        };
    };

    struct RunManifest {
        PreprocessReport preprocess{};
        std::string primary_series_key;
        RuntimePrimarySelectionMode primary_selection_mode{ RuntimePrimarySelectionMode::Unknown };
        std::vector<TopologySpec> topologies{};
        std::vector<TopologyUsageSnapshot> topology_usages{};
        std::vector<CapabilityOutcomeSnapshot> capability_outcomes{};
        std::vector<RngSeedRecord> rng_seeds;
    };

    std::string infer_time_resolution(TimeKind kind);
    std::string_view runtime_primary_selection_mode_name(RuntimePrimarySelectionMode mode);

    PreprocessReport make_preprocess_report(
        const DataSpec& spec,
        const PreprocessResult& prep,
        const std::optional<align::AlignConfig>& align_cfg = std::nullopt);

    RunManifest make_run_manifest(
        const PreprocessReport& preprocess,
        std::string primary_series_key = {},
        std::shared_ptr<const TopologyCatalog> topology_catalog = {},
        RuntimePrimarySelectionMode primary_selection_mode = RuntimePrimarySelectionMode::Unknown);

    void append_observation_capability_snapshot(
        const ObservationCapabilityReport& report,
        RunManifest& manifest);

    void append_plan_capability_snapshot(
        const PlanCapabilityReport& report,
        RunManifest& manifest);

    void append_planspec_rng_seeds(const PlanSpec& spec, RunManifest& manifest);

} // namespace sugar
