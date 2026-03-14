#include "observation_capability.h"

#include <algorithm>
#include <array>

namespace sugar {

    namespace {

        const TopologySpec& builtin_temporal_topology_spec_ref() {
            static const TopologySpec kSpec = make_builtin_temporal_topology_spec();
            return kSpec;
        }

        const TopologySpec& builtin_spatial_topology_spec_ref() {
            static const TopologySpec kSpec = make_builtin_spatial_topology_spec();
            return kSpec;
        }

        const TopologySpec& builtin_spatiotemporal_topology_spec_ref() {
            static const TopologySpec kSpec = make_builtin_spatiotemporal_topology_spec();
            return kSpec;
        }

        constexpr std::array<ObservationSurfaceKind, 7> kObservationSurfaceOrder{
            ObservationSurfaceKind::RowDomain,
            ObservationSurfaceKind::Temporal,
            ObservationSurfaceKind::Spatial,
            ObservationSurfaceKind::SpatioTemporal,
            ObservationSurfaceKind::TemporalPanel,
            ObservationSurfaceKind::SpatialPanel,
            ObservationSurfaceKind::SpatioTemporalPanel,
        };

        void add_reason(std::vector<ObservationCapabilityReasonCode>& reasons,
            ObservationCapabilityReasonCode code) {
            if (std::find(reasons.begin(), reasons.end(), code) == reasons.end()) {
                reasons.push_back(code);
            }
        }

        void append_topology(std::vector<TopologySpec>& topologies, const TopologySpec& spec) {
            if (!spec.valid()) return;

            const auto it = std::find_if(topologies.begin(), topologies.end(),
                [&](const TopologySpec& existing) {
                    return existing.name == spec.name;
                });
            if (it == topologies.end()) {
                topologies.push_back(spec);
            }
            else {
                *it = spec;
            }
        }

        void append_catalog_topologies(
            std::vector<TopologySpec>& topologies,
            const TopologyCatalog* catalog) {
            if (!catalog) return;
            for (const auto& spec : catalog->specs()) {
                append_topology(topologies, spec);
            }
        }

        void append_builtin_runtime_topologies(
            std::vector<TopologySpec>& topologies,
            bool has_time_axis,
            bool has_spatial_axis) {
            if (has_time_axis) {
                append_topology(topologies, builtin_temporal_topology_spec_ref());
            }
            if (has_spatial_axis) {
                append_topology(topologies, builtin_spatial_topology_spec_ref());
            }
            if (has_time_axis && has_spatial_axis) {
                append_topology(topologies, builtin_spatiotemporal_topology_spec_ref());
            }
        }

        std::size_t derive_frameset_series_count(const FrameSet& fs, bool produced_frameset) {
            if (fs.by_entity.empty()) return 0;
            if (produced_frameset) return fs.by_entity.size();
            return 1;
        }

        std::size_t derive_frameset_row_count(const FrameSet& fs) {
            if (fs.by_entity.empty()) return 0;
            return fs.by_entity.begin()->second.row_count();
        }

        bool has_panel_time_support(const ObservationWorldFacts& facts) {
            return facts.has_entity_axis && facts.has_time_axis && facts.series_count > 0;
        }

        bool has_row_domain(const ObservationWorldFacts& facts) {
            return facts.row_count > 0;
        }

        ObservationSurfaceCapability invalid_surface(
            ObservationSurfaceKind surface,
            ObservationCapabilityReasonCode code) {
            ObservationSurfaceCapability out{};
            out.surface = surface;
            out.status = ObservationCapabilityStatus::Invalid;
            add_reason(out.reasons, code);
            return out;
        }

    } // namespace

    std::string_view observation_surface_name(ObservationSurfaceKind surface) {
        switch (surface) {
        case ObservationSurfaceKind::RowDomain: return "Row domain";
        case ObservationSurfaceKind::Temporal: return "Temporal";
        case ObservationSurfaceKind::Spatial: return "Spatial";
        case ObservationSurfaceKind::SpatioTemporal: return "Spatiotemporal";
        case ObservationSurfaceKind::TemporalPanel: return "Temporal panel";
        case ObservationSurfaceKind::SpatialPanel: return "Spatial panel";
        case ObservationSurfaceKind::SpatioTemporalPanel: return "Spatiotemporal panel";
        }
        return "Unknown";
    }

    std::string_view observation_capability_status_name(ObservationCapabilityStatus status) {
        switch (status) {
        case ObservationCapabilityStatus::Valid: return "valid";
        case ObservationCapabilityStatus::Invalid: return "invalid";
        case ObservationCapabilityStatus::Conditional: return "conditional";
        }
        return "unknown";
    }

    std::string_view observation_capability_reason_code_name(ObservationCapabilityReasonCode code) {
        switch (code) {
        case ObservationCapabilityReasonCode::MissingTimeAxis: return "missing_time_axis";
        case ObservationCapabilityReasonCode::MissingSpatialAxis: return "missing_spatial_axis";
        case ObservationCapabilityReasonCode::MissingEntityAxis: return "missing_entity_axis";
        case ObservationCapabilityReasonCode::MissingSharedPerEntityTimeAxis: return "missing_shared_per_entity_time_axis";
        case ObservationCapabilityReasonCode::SpatialSchemaCanBeProvided: return "spatial_schema_can_be_provided";
        case ObservationCapabilityReasonCode::EntitySchemaCanBeProvided: return "entity_schema_can_be_provided";
        case ObservationCapabilityReasonCode::EmptyObservationDomain: return "empty_observation_domain";
        }
        return "unknown";
    }

    std::string_view observation_capability_reason_text(ObservationCapabilityReasonCode code) {
        switch (code) {
        case ObservationCapabilityReasonCode::MissingTimeAxis: return "no semantic time axis";
        case ObservationCapabilityReasonCode::MissingSpatialAxis: return "no spatial neighborhood/index schema";
        case ObservationCapabilityReasonCode::MissingEntityAxis: return "no entity axis";
        case ObservationCapabilityReasonCode::MissingSharedPerEntityTimeAxis: return "no shared per-entity time axis";
        case ObservationCapabilityReasonCode::SpatialSchemaCanBeProvided: return "provide spatial neighborhood/index schema";
        case ObservationCapabilityReasonCode::EntitySchemaCanBeProvided: return "provide entity grouping schema";
        case ObservationCapabilityReasonCode::EmptyObservationDomain: return "empty observation row domain";
        }
        return "unknown reason";
    }

    ObservationWorldFacts make_observation_world_facts(
        const FrameSet& fs,
        const PreprocessReport& report) {
        ObservationWorldFacts out{};
        out.series_count = derive_frameset_series_count(fs, report.produced_frameset);
        out.row_count = derive_frameset_row_count(fs);
        out.produced_frameset = report.produced_frameset;
        out.has_time_axis = report.has_time_axis;
        out.has_spatial_axis = report.has_spatial_axis;
        out.has_entity_axis = report.has_entity_axis;
        append_builtin_runtime_topologies(
            out.available_topologies,
            out.has_time_axis,
            out.has_spatial_axis);
        return out;
    }

    ObservationWorldFacts make_observation_world_facts(
        const ResolvedRuntimeInput& runtime) {
        ObservationWorldFacts out{};
        out.source_kind = runtime.source_kind;
        out.produced_frameset = runtime.run_manifest.preprocess.produced_frameset;
        out.has_time_axis = runtime.run_manifest.preprocess.has_time_axis;
        out.has_spatial_axis = runtime.run_manifest.preprocess.has_spatial_axis;
        out.has_entity_axis = runtime.run_manifest.preprocess.has_entity_axis;
        out.has_runtime_bundle = static_cast<bool>(runtime.aligned_bundle);

        append_catalog_topologies(out.available_topologies, runtime.topology_catalog.get());
        append_catalog_topologies(
            out.available_topologies,
            runtime.aligned_bundle ? runtime.aligned_bundle->topology_catalog.get() : nullptr);
        for (const auto& spec : runtime.run_manifest.topologies) {
            append_topology(out.available_topologies, spec);
        }

        if (runtime.aligned_bundle) {
            out.series_count = runtime.aligned_bundle->series_keys.size();
            out.row_count = runtime.aligned_bundle->row_count();
            out.has_time_axis = runtime.aligned_bundle->has_axis(AxisKind::Time);
            out.has_spatial_axis = runtime.aligned_bundle->has_axis(AxisKind::Spatial);
            append_builtin_runtime_topologies(
                out.available_topologies,
                out.has_time_axis,
                out.has_spatial_axis);
        }
        else if (runtime.frameset) {
            out.series_count = derive_frameset_series_count(*runtime.frameset, out.produced_frameset);
            out.row_count = derive_frameset_row_count(*runtime.frameset);
            append_builtin_runtime_topologies(
                out.available_topologies,
                out.has_time_axis,
                out.has_spatial_axis);
        }

        return out;
    }

    ObservationSurfaceCapability diagnose_observation_surface(
        const ObservationWorldFacts& facts,
        ObservationSurfaceKind surface) {
        ObservationSurfaceCapability out{};
        out.surface = surface;

        if (surface == ObservationSurfaceKind::RowDomain) {
            if (has_row_domain(facts)) {
                out.status = ObservationCapabilityStatus::Valid;
            }
            else {
                out.status = ObservationCapabilityStatus::Invalid;
                add_reason(out.reasons, ObservationCapabilityReasonCode::EmptyObservationDomain);
            }
            return out;
        }

        if (!has_row_domain(facts)) {
            return invalid_surface(surface, ObservationCapabilityReasonCode::EmptyObservationDomain);
        }

        switch (surface) {
        case ObservationSurfaceKind::Temporal:
            if (facts.has_time_axis) {
                out.status = ObservationCapabilityStatus::Valid;
            }
            else {
                out.status = ObservationCapabilityStatus::Invalid;
                add_reason(out.reasons, ObservationCapabilityReasonCode::MissingTimeAxis);
            }
            return out;

        case ObservationSurfaceKind::Spatial:
            if (facts.has_spatial_axis) {
                out.status = ObservationCapabilityStatus::Valid;
            }
            else {
                out.status = ObservationCapabilityStatus::Conditional;
                add_reason(out.reasons, ObservationCapabilityReasonCode::SpatialSchemaCanBeProvided);
            }
            return out;

        case ObservationSurfaceKind::SpatioTemporal:
            if (facts.has_time_axis && facts.has_spatial_axis) {
                out.status = ObservationCapabilityStatus::Valid;
            }
            else if (facts.has_time_axis) {
                out.status = ObservationCapabilityStatus::Conditional;
                add_reason(out.reasons, ObservationCapabilityReasonCode::SpatialSchemaCanBeProvided);
            }
            else {
                out.status = ObservationCapabilityStatus::Invalid;
                add_reason(out.reasons, ObservationCapabilityReasonCode::MissingTimeAxis);
                if (!facts.has_spatial_axis) {
                    add_reason(out.reasons, ObservationCapabilityReasonCode::SpatialSchemaCanBeProvided);
                }
            }
            return out;

        case ObservationSurfaceKind::TemporalPanel:
            if (!facts.has_time_axis) {
                out.status = ObservationCapabilityStatus::Invalid;
                add_reason(out.reasons, ObservationCapabilityReasonCode::MissingSharedPerEntityTimeAxis);
                if (!facts.has_entity_axis) {
                    add_reason(out.reasons, ObservationCapabilityReasonCode::EntitySchemaCanBeProvided);
                }
            }
            else if (!facts.has_entity_axis) {
                out.status = ObservationCapabilityStatus::Conditional;
                add_reason(out.reasons, ObservationCapabilityReasonCode::EntitySchemaCanBeProvided);
            }
            else if (!has_panel_time_support(facts)) {
                out.status = ObservationCapabilityStatus::Invalid;
                add_reason(out.reasons, ObservationCapabilityReasonCode::MissingSharedPerEntityTimeAxis);
            }
            else {
                out.status = ObservationCapabilityStatus::Valid;
            }
            return out;

        case ObservationSurfaceKind::SpatialPanel:
            if (facts.has_entity_axis && facts.has_spatial_axis) {
                out.status = ObservationCapabilityStatus::Valid;
            }
            else {
                out.status = ObservationCapabilityStatus::Conditional;
                if (!facts.has_entity_axis) {
                    add_reason(out.reasons, ObservationCapabilityReasonCode::EntitySchemaCanBeProvided);
                }
                if (!facts.has_spatial_axis) {
                    add_reason(out.reasons, ObservationCapabilityReasonCode::SpatialSchemaCanBeProvided);
                }
            }
            return out;

        case ObservationSurfaceKind::SpatioTemporalPanel:
            if (!facts.has_time_axis) {
                out.status = ObservationCapabilityStatus::Invalid;
                add_reason(out.reasons, ObservationCapabilityReasonCode::MissingSharedPerEntityTimeAxis);
                if (!facts.has_entity_axis) {
                    add_reason(out.reasons, ObservationCapabilityReasonCode::EntitySchemaCanBeProvided);
                }
                if (!facts.has_spatial_axis) {
                    add_reason(out.reasons, ObservationCapabilityReasonCode::SpatialSchemaCanBeProvided);
                }
            }
            else if (!facts.has_entity_axis || !facts.has_spatial_axis) {
                out.status = ObservationCapabilityStatus::Conditional;
                if (!facts.has_entity_axis) {
                    add_reason(out.reasons, ObservationCapabilityReasonCode::EntitySchemaCanBeProvided);
                }
                if (!facts.has_spatial_axis) {
                    add_reason(out.reasons, ObservationCapabilityReasonCode::SpatialSchemaCanBeProvided);
                }
            }
            else if (!has_panel_time_support(facts)) {
                out.status = ObservationCapabilityStatus::Invalid;
                add_reason(out.reasons, ObservationCapabilityReasonCode::MissingSharedPerEntityTimeAxis);
            }
            else {
                out.status = ObservationCapabilityStatus::Valid;
            }
            return out;

        case ObservationSurfaceKind::RowDomain:
            break;
        }

        out.status = ObservationCapabilityStatus::Invalid;
        return out;
    }

    ObservationCapabilityReport diagnose_observation_capabilities(
        const ObservationWorldFacts& facts) {
        ObservationCapabilityReport out{};
        out.facts = facts;
        append_builtin_runtime_topologies(
            out.facts.available_topologies,
            out.facts.has_time_axis,
            out.facts.has_spatial_axis);
        out.surfaces.reserve(kObservationSurfaceOrder.size());
        for (const auto surface : kObservationSurfaceOrder) {
            out.surfaces.push_back(diagnose_observation_surface(out.facts, surface));
        }
        return out;
    }

} // namespace sugar
