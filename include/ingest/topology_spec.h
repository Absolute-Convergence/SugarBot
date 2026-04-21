#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sugar {

    enum class TopologyOrigin : std::uint8_t {
        Unknown = 0,
        BuiltinRuntime = 1,
        LayoutDeclared = 2,
    };

    enum class TopologyBearing : std::uint8_t {
        MetadataOnly = 0,
        QueryBearing = 1,
        RuntimeBearing = 2,
    };

    enum class TopologyFamily : std::uint8_t {
        Unknown = 0,
        TemporalWindow = 1,
        SpatialMetric2D = 2,
        SpatioTemporalComposite = 3,
        RelationalGraph = 4,
        SemanticGraph = 5,
        ProbabilisticGraph = 6,
        Composite = 7,
    };

    enum class TopologyDomainScope : std::uint8_t {
        RowDomain = 0,
        SeriesLocal = 1,
        EntityPanel = 2,
        ObservationDomain = 3,
    };

    enum class TopologyQueryKind : std::uint8_t {
        Selection = 0,
        OrderedWindow = 1,
        MetricRadius = 2,
        MetricKNearest = 3,
        Intersection = 4,
        NeighborSet = 5,
        WeightedNeighborSet = 6,
        TransitionSet = 7,
    };

    struct TopologyArtifactRef {
        std::string stable_id{};
        std::string locator{};
        std::string format{};
        std::string digest{};
        std::string producer{};

        bool empty() const {
            return stable_id.empty() &&
                locator.empty() &&
                format.empty() &&
                digest.empty() &&
                producer.empty();
        }
    };

    inline std::string topology_artifact_identity(const TopologyArtifactRef& ref) {
        if (!ref.stable_id.empty()) return ref.stable_id;
        if (!ref.digest.empty()) return ref.digest;
        return ref.locator;
    }

    struct TopologySpec {
        std::string name{};
        TopologyOrigin origin{ TopologyOrigin::LayoutDeclared };
        TopologyBearing bearing{ TopologyBearing::MetadataOnly };
        TopologyFamily family{ TopologyFamily::Unknown };
        TopologyDomainScope domain_scope{ TopologyDomainScope::RowDomain };
        std::vector<TopologyQueryKind> query_kinds{};
        bool directed{ false };
        bool weighted{ false };
        std::optional<TopologyArtifactRef> artifact_ref{};
        std::vector<std::pair<std::string, std::string>> metadata{};

        bool valid() const {
            return !name.empty();
        }

        bool is_runtime_bearing() const {
            return bearing == TopologyBearing::RuntimeBearing;
        }

        bool supports_query_kind(TopologyQueryKind kind) const {
            return std::find(query_kinds.begin(), query_kinds.end(), kind) != query_kinds.end();
        }
    };

    inline bool is_builtin_topology_name(std::string_view name) {
        return name.rfind("builtin.", 0) == 0;
    }

    class TopologyCatalog {
    public:
        bool empty() const { return specs_.empty(); }
        std::size_t size() const { return specs_.size(); }
        const std::vector<TopologySpec>& specs() const { return specs_; }

        const TopologySpec* find(std::string_view name) const {
            for (const auto& spec : specs_) {
                if (spec.name == name) return &spec;
            }
            return nullptr;
        }

        bool has(std::string_view name) const {
            return find(name) != nullptr;
        }

        bool has_family(TopologyFamily family) const {
            return std::any_of(specs_.begin(), specs_.end(),
                [&](const TopologySpec& spec) { return spec.family == family; });
        }

        bool add(TopologySpec spec, std::string* err = nullptr) {
            if (!spec.valid()) {
                if (err) *err = "topology spec name must not be empty";
                return false;
            }
            if (is_builtin_topology_name(spec.name) &&
                spec.origin != TopologyOrigin::BuiltinRuntime)
            {
                if (err) *err = "topology name is reserved for builtin runtime topology: " + spec.name;
                return false;
            }
            for (auto& existing : specs_) {
                if (existing.name == spec.name) {
                    if (err) *err = "duplicate topology name: " + spec.name;
                    return false;
                }
            }
            specs_.push_back(std::move(spec));
            return true;
        }

        void upsert_builtin(TopologySpec spec) {
            if (!spec.valid()) return;
            spec.origin = TopologyOrigin::BuiltinRuntime;
            spec.bearing = TopologyBearing::RuntimeBearing;
            for (auto& existing : specs_) {
                if (existing.name == spec.name) {
                    existing = std::move(spec);
                    return;
                }
            }
            specs_.push_back(std::move(spec));
        }

        std::vector<std::string> list_names() const {
            std::vector<std::string> out;
            out.reserve(specs_.size());
            for (const auto& spec : specs_) {
                out.push_back(spec.name);
            }
            return out;
        }

        std::vector<TopologySpec> snapshot() const {
            return specs_;
        }

    private:
        std::vector<TopologySpec> specs_{};
    };

    inline std::string_view builtin_temporal_topology_name() {
        return "builtin.temporal.window";
    }

    inline std::string_view builtin_spatial_topology_name() {
        return "builtin.spatial.metric_2d";
    }

    inline std::string_view builtin_spatiotemporal_topology_name() {
        return "builtin.spatiotemporal.composite";
    }

    inline TopologySpec make_builtin_temporal_topology_spec() {
        TopologySpec spec{};
        spec.name = std::string(builtin_temporal_topology_name());
        spec.origin = TopologyOrigin::BuiltinRuntime;
        spec.bearing = TopologyBearing::RuntimeBearing;
        spec.family = TopologyFamily::TemporalWindow;
        spec.domain_scope = TopologyDomainScope::SeriesLocal;
        spec.query_kinds = { TopologyQueryKind::OrderedWindow };
        spec.artifact_ref = TopologyArtifactRef{
            "builtin.temporal.window.v1",
            "builtin://topology/temporal/window",
            "builtin",
            {},
            "sugarbot"
        };
        spec.metadata.emplace_back("origin", "builtin");
        return spec;
    }

    inline TopologySpec make_builtin_spatial_topology_spec() {
        TopologySpec spec{};
        spec.name = std::string(builtin_spatial_topology_name());
        spec.origin = TopologyOrigin::BuiltinRuntime;
        spec.bearing = TopologyBearing::RuntimeBearing;
        spec.family = TopologyFamily::SpatialMetric2D;
        spec.domain_scope = TopologyDomainScope::SeriesLocal;
        spec.query_kinds = {
            TopologyQueryKind::MetricRadius,
            TopologyQueryKind::MetricKNearest
        };
        spec.artifact_ref = TopologyArtifactRef{
            "builtin.spatial.metric_2d.v1",
            "builtin://topology/spatial/metric_2d",
            "builtin",
            {},
            "sugarbot"
        };
        spec.metadata.emplace_back("origin", "builtin");
        spec.metadata.emplace_back("space", "coordinates_2d");
        return spec;
    }

    inline TopologySpec make_builtin_spatiotemporal_topology_spec() {
        TopologySpec spec{};
        spec.name = std::string(builtin_spatiotemporal_topology_name());
        spec.origin = TopologyOrigin::BuiltinRuntime;
        spec.bearing = TopologyBearing::RuntimeBearing;
        spec.family = TopologyFamily::SpatioTemporalComposite;
        spec.domain_scope = TopologyDomainScope::SeriesLocal;
        spec.query_kinds = {
            TopologyQueryKind::OrderedWindow,
            TopologyQueryKind::MetricRadius,
            TopologyQueryKind::MetricKNearest,
            TopologyQueryKind::Intersection
        };
        spec.artifact_ref = TopologyArtifactRef{
            "builtin.spatiotemporal.composite.v1",
            "builtin://topology/spatiotemporal/composite",
            "builtin",
            {},
            "sugarbot"
        };
        spec.metadata.emplace_back("origin", "builtin");
        spec.metadata.emplace_back("composition", "time+spatial");
        return spec;
    }

    inline void mount_builtin_runtime_topologies(
        TopologyCatalog& catalog,
        bool has_time_axis,
        bool has_spatial_axis) {
        if (has_time_axis) {
            catalog.upsert_builtin(make_builtin_temporal_topology_spec());
        }
        if (has_spatial_axis) {
            catalog.upsert_builtin(make_builtin_spatial_topology_spec());
        }
        if (has_time_axis && has_spatial_axis) {
            catalog.upsert_builtin(make_builtin_spatiotemporal_topology_spec());
        }
    }

    inline std::string_view topology_origin_name(TopologyOrigin origin) {
        switch (origin) {
        case TopologyOrigin::Unknown: return "unknown";
        case TopologyOrigin::BuiltinRuntime: return "builtin_runtime";
        case TopologyOrigin::LayoutDeclared: return "layout_declared";
        }
        return "unknown";
    }

    inline std::string_view topology_bearing_name(TopologyBearing bearing) {
        switch (bearing) {
        case TopologyBearing::MetadataOnly: return "metadata_only";
        case TopologyBearing::QueryBearing: return "query_bearing";
        case TopologyBearing::RuntimeBearing: return "runtime_bearing";
        }
        return "unknown";
    }

    inline std::string_view topology_family_name(TopologyFamily family) {
        switch (family) {
        case TopologyFamily::Unknown: return "unknown";
        case TopologyFamily::TemporalWindow: return "temporal_window";
        case TopologyFamily::SpatialMetric2D: return "spatial_metric_2d";
        case TopologyFamily::SpatioTemporalComposite: return "spatiotemporal_composite";
        case TopologyFamily::RelationalGraph: return "relational_graph";
        case TopologyFamily::SemanticGraph: return "semantic_graph";
        case TopologyFamily::ProbabilisticGraph: return "probabilistic_graph";
        case TopologyFamily::Composite: return "composite";
        }
        return "unknown";
    }

    inline std::string_view topology_domain_scope_name(TopologyDomainScope scope) {
        switch (scope) {
        case TopologyDomainScope::RowDomain: return "row_domain";
        case TopologyDomainScope::SeriesLocal: return "series_local";
        case TopologyDomainScope::EntityPanel: return "entity_panel";
        case TopologyDomainScope::ObservationDomain: return "observation_domain";
        }
        return "unknown";
    }

    inline std::string_view topology_query_kind_name(TopologyQueryKind kind) {
        switch (kind) {
        case TopologyQueryKind::Selection: return "selection";
        case TopologyQueryKind::OrderedWindow: return "ordered_window";
        case TopologyQueryKind::MetricRadius: return "metric_radius";
        case TopologyQueryKind::MetricKNearest: return "metric_k_nearest";
        case TopologyQueryKind::Intersection: return "intersection";
        case TopologyQueryKind::NeighborSet: return "neighbor_set";
        case TopologyQueryKind::WeightedNeighborSet: return "weighted_neighbor_set";
        case TopologyQueryKind::TransitionSet: return "transition_set";
        }
        return "unknown";
    }

} // namespace sugar
