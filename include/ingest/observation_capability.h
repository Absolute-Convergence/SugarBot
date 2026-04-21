#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

#include "ingest/run_pipeline.h"

namespace sugar {

    enum class ObservationSurfaceKind {
        RowDomain = 0,
        Temporal = 1,
        Spatial = 2,
        SpatioTemporal = 3,
        TemporalPanel = 4,
        SpatialPanel = 5,
        SpatioTemporalPanel = 6,
    };

    enum class ObservationCapabilityStatus {
        Valid = 0,
        Invalid = 1,
        Conditional = 2,
    };

    enum class ObservationCapabilityReasonCode {
        MissingTimeAxis = 0,
        MissingSpatialAxis = 1,
        MissingEntityAxis = 2,
        MissingSharedPerEntityTimeAxis = 3,
        SpatialSchemaCanBeProvided = 4,
        EntitySchemaCanBeProvided = 5,
        EmptyObservationDomain = 6,
    };

    struct ObservationWorldFacts {
        RuntimeObservationSourceKind source_kind{ RuntimeObservationSourceKind::Unknown };
        std::size_t series_count{ 0 };
        std::size_t row_count{ 0 };
        bool produced_frameset{ false };
        bool has_time_axis{ false };
        bool has_spatial_axis{ false };
        bool has_entity_axis{ false };
        bool has_runtime_bundle{ false };
        std::vector<TopologySpec> available_topologies{};
    };

    struct ObservationSurfaceCapability {
        ObservationSurfaceKind surface{ ObservationSurfaceKind::RowDomain };
        ObservationCapabilityStatus status{ ObservationCapabilityStatus::Invalid };
        std::vector<ObservationCapabilityReasonCode> reasons{};
    };

    struct ObservationCapabilityReport {
        ObservationWorldFacts facts{};
        std::vector<ObservationSurfaceCapability> surfaces{};
    };

    std::string_view observation_surface_name(ObservationSurfaceKind surface);
    std::string_view observation_capability_status_name(ObservationCapabilityStatus status);
    std::string_view observation_capability_reason_code_name(ObservationCapabilityReasonCode code);
    std::string_view observation_capability_reason_text(ObservationCapabilityReasonCode code);

    ObservationWorldFacts make_observation_world_facts(
        const FrameSet& fs,
        const PreprocessReport& report);

    ObservationWorldFacts make_observation_world_facts(
        const ResolvedRuntimeInput& runtime);

    ObservationWorldFacts make_observation_world_facts(
        const ResolvedObservationWorld& world);

    ObservationSurfaceCapability diagnose_observation_surface(
        const ObservationWorldFacts& facts,
        ObservationSurfaceKind surface);

    ObservationCapabilityReport diagnose_observation_capabilities(
        const ObservationWorldFacts& facts);

} // namespace sugar
