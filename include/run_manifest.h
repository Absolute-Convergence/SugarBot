#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "align.h"
#include "data_spec.h"
#include "preprocess.h"
#include "topology_spec.h"

namespace sugar {

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
        bool alignment_applied{ false };
        align::CalendarPolicy alignment_calendar{ align::CalendarPolicy::Intersection };
        align::MissingPolicy alignment_missing{ align::MissingPolicy::NaN };
    };

    struct RunManifest {
        PreprocessReport preprocess{};
        std::string primary_series_key;
        std::vector<TopologySpec> topologies{};
        std::vector<TopologyUsageSnapshot> topology_usages{};
        std::vector<CapabilityOutcomeSnapshot> capability_outcomes{};
        std::vector<RngSeedRecord> rng_seeds;
    };

    std::string infer_time_resolution(TimeKind kind);

    PreprocessReport make_preprocess_report(
        const DataSpec& spec,
        const PreprocessResult& prep,
        const std::optional<align::AlignConfig>& align_cfg = std::nullopt);

    RunManifest make_run_manifest(
        const PreprocessReport& preprocess,
        std::string primary_series_key = {},
        std::shared_ptr<const TopologyCatalog> topology_catalog = {});

    void append_observation_capability_snapshot(
        const ObservationCapabilityReport& report,
        RunManifest& manifest);

    void append_plan_capability_snapshot(
        const PlanCapabilityReport& report,
        RunManifest& manifest);

    void append_planspec_rng_seeds(const PlanSpec& spec, RunManifest& manifest);

} // namespace sugar
