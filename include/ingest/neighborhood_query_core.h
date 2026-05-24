#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ingest/index_schema.h"
#include "ingest/topology_spec.h"
#include "ingest/time_key.h"

namespace sugar {

    using NeighborhoodParamValue = std::variant<
        std::monostate,
        bool,
        std::size_t,
        std::int64_t,
        double,
        std::string>;

    struct NeighborhoodAnchor {
        std::size_t row_index{ 0 };
        std::string series{};
        std::string entity{};
    };

    struct NeighborhoodRequest {
        std::string topology_name{};
        NeighborhoodAnchor anchor{};
        TopologyQueryKind query_kind{ TopologyQueryKind::Selection };
        bool include_anchor{ true };
        std::vector<std::pair<std::string, NeighborhoodParamValue>> params{};

        const NeighborhoodParamValue* find_param(std::string_view name) const {
            for (const auto& param : params) {
                if (param.first == name) return &param.second;
            }
            return nullptr;
        }

        template <typename T>
        const T* find_param_as(std::string_view name) const {
            const auto* value = find_param(name);
            if (!value) return nullptr;
            return std::get_if<T>(value);
        }

        void set_param(std::string name, NeighborhoodParamValue value) {
            for (auto& param : params) {
                if (param.first == name) {
                    param.second = std::move(value);
                    return;
                }
            }
            params.emplace_back(std::move(name), std::move(value));
        }
    };

    struct NeighborhoodResult {
        std::string topology_name{};
        NeighborhoodAnchor anchor{};
        TopologyQueryKind query_kind{ TopologyQueryKind::Selection };
        bool include_anchor{ true };
        std::vector<std::pair<std::string, NeighborhoodParamValue>> query_params{};
        std::vector<std::size_t> indices{};
        std::vector<double> weights{};
        std::vector<double> distances{};
        std::size_t row_count_bound{ 0 };
        bool has_result_cap{ false };
        std::size_t result_cap{ std::numeric_limits<std::size_t>::max() };
        bool ordered{ false };
        bool contiguous{ false };
        std::size_t begin{ 0 };
        std::size_t end{ 0 };

        bool empty() const { return size() == 0; }

        std::size_t size() const {
            if (contiguous) {
                return (end > begin) ? (end - begin) : 0;
            }
            return indices.size();
        }

        const NeighborhoodParamValue* find_param(std::string_view name) const {
            for (const auto& param : query_params) {
                if (param.first == name) return &param.second;
            }
            return nullptr;
        }

        template <typename T>
        const T* find_param_as(std::string_view name) const {
            const auto* value = find_param(name);
            if (!value) return nullptr;
            return std::get_if<T>(value);
        }

        std::vector<std::size_t> materialize_indices() const {
            if (!contiguous) return indices;

            std::vector<std::size_t> out;
            out.reserve(size());
            for (std::size_t i = begin; i < end; ++i) {
                out.push_back(i);
            }
            return out;
        }
    };

    struct NeighborhoodSlice {
        std::size_t begin{ 0 }; // inclusive
        std::size_t end{ 0 };   // exclusive
        std::size_t anchor{ 0 };

        bool empty() const { return begin >= end; }
        std::size_t size() const { return (end > begin) ? (end - begin) : 0; }
    };

    struct NeighborhoodSelection {
        std::vector<std::size_t> indices;
        std::size_t anchor{ 0 };

        bool empty() const { return indices.empty(); }
        std::size_t size() const { return indices.size(); }
    };

    enum class TimeNeighborhoodMode : std::uint8_t {
        LastNSamples = 0,
        LookbackDelta = 1,
    };

    struct TimeNeighborhoodRequest {
        TimeNeighborhoodMode mode{ TimeNeighborhoodMode::LastNSamples };
        std::size_t last_n{ 0 };
        TimeKey lookback_delta{ 0 };
        bool include_anchor{ true };

        static TimeNeighborhoodRequest last_n_samples(
            std::size_t n,
            bool include_anchor = true) {
            TimeNeighborhoodRequest r{};
            r.mode = TimeNeighborhoodMode::LastNSamples;
            r.last_n = n;
            r.include_anchor = include_anchor;
            return r;
        }

        static TimeNeighborhoodRequest lookback(
            TimeKey delta,
            bool include_anchor = true) {
            TimeNeighborhoodRequest r{};
            r.mode = TimeNeighborhoodMode::LookbackDelta;
            r.lookback_delta = delta;
            r.include_anchor = include_anchor;
            return r;
        }
    };

    enum class SpatialNeighborhoodMode : std::uint8_t {
        RadiusEuclidean = 0,
        KNearest = 1,
    };

    struct SpatialNeighborhoodRequest {
        SpatialNeighborhoodMode mode{ SpatialNeighborhoodMode::RadiusEuclidean };
        double radius{ 0.0 };
        std::size_t k_nearest{ 0 };
        bool include_anchor{ true };

        static SpatialNeighborhoodRequest radius_euclidean(
            double r,
            bool include_anchor = true) {
            SpatialNeighborhoodRequest req{};
            req.mode = SpatialNeighborhoodMode::RadiusEuclidean;
            req.radius = r;
            req.include_anchor = include_anchor;
            return req;
        }

        static SpatialNeighborhoodRequest nearest_k(
            std::size_t k,
            bool include_anchor = true) {
            SpatialNeighborhoodRequest req{};
            req.mode = SpatialNeighborhoodMode::KNearest;
            req.k_nearest = k;
            req.include_anchor = include_anchor;
            return req;
        }
    };

    enum class GraphNeighborhoodDirection : std::uint8_t {
        Outgoing = 0,
        Incoming = 1,
        Both = 2,
    };

    struct GraphNeighborhoodRequest {
        GraphNeighborhoodDirection direction{ GraphNeighborhoodDirection::Outgoing };
        bool weighted{ false };
        bool include_anchor{ false };
        std::size_t max_neighbors{ std::numeric_limits<std::size_t>::max() };

        static GraphNeighborhoodRequest neighbors(
            GraphNeighborhoodDirection dir = GraphNeighborhoodDirection::Outgoing,
            bool include_anchor = false,
            std::size_t max = std::numeric_limits<std::size_t>::max()) {
            GraphNeighborhoodRequest req{};
            req.direction = dir;
            req.weighted = false;
            req.include_anchor = include_anchor;
            req.max_neighbors = max;
            return req;
        }

        static GraphNeighborhoodRequest weighted_neighbors(
            GraphNeighborhoodDirection dir = GraphNeighborhoodDirection::Outgoing,
            bool include_anchor = false,
            std::size_t max = std::numeric_limits<std::size_t>::max()) {
            GraphNeighborhoodRequest req{};
            req.direction = dir;
            req.weighted = true;
            req.include_anchor = include_anchor;
            req.max_neighbors = max;
            return req;
        }
    };

    enum class HierarchyTraversal : std::uint8_t {
        Parent = 0,
        Children = 1,
        Siblings = 2,
        ParentAndChildren = 3,
    };

    struct HierarchyNeighborhoodRequest {
        HierarchyTraversal traversal{ HierarchyTraversal::Children };
        bool include_anchor{ false };
        std::size_t max_depth{ 1 };
        std::size_t max_neighbors{ std::numeric_limits<std::size_t>::max() };

        static HierarchyNeighborhoodRequest parent(
            bool include_anchor = false,
            std::size_t max_depth = 1,
            std::size_t max_neighbors = std::numeric_limits<std::size_t>::max()) {
            HierarchyNeighborhoodRequest req{};
            req.traversal = HierarchyTraversal::Parent;
            req.include_anchor = include_anchor;
            req.max_depth = max_depth;
            req.max_neighbors = max_neighbors;
            return req;
        }

        static HierarchyNeighborhoodRequest children(
            bool include_anchor = false,
            std::size_t max_depth = 1,
            std::size_t max_neighbors = std::numeric_limits<std::size_t>::max()) {
            HierarchyNeighborhoodRequest req{};
            req.traversal = HierarchyTraversal::Children;
            req.include_anchor = include_anchor;
            req.max_depth = max_depth;
            req.max_neighbors = max_neighbors;
            return req;
        }

        static HierarchyNeighborhoodRequest siblings(
            bool include_anchor = false,
            std::size_t max_neighbors = std::numeric_limits<std::size_t>::max()) {
            HierarchyNeighborhoodRequest req{};
            req.traversal = HierarchyTraversal::Siblings;
            req.include_anchor = include_anchor;
            req.max_neighbors = max_neighbors;
            return req;
        }

        static HierarchyNeighborhoodRequest parent_and_children(
            bool include_anchor = false,
            std::size_t max_depth = 1,
            std::size_t max_neighbors = std::numeric_limits<std::size_t>::max()) {
            HierarchyNeighborhoodRequest req{};
            req.traversal = HierarchyTraversal::ParentAndChildren;
            req.include_anchor = include_anchor;
            req.max_depth = max_depth;
            req.max_neighbors = max_neighbors;
            return req;
        }
    };

    struct SpatioTemporalNeighborhoodRequest {
        TimeNeighborhoodRequest time{};
        SpatialNeighborhoodRequest spatial{};
        bool require_both{ true };

        static SpatioTemporalNeighborhoodRequest intersection(
            const TimeNeighborhoodRequest& t,
            const SpatialNeighborhoodRequest& s) {
            SpatioTemporalNeighborhoodRequest req{};
            req.time = t;
            req.spatial = s;
            req.require_both = true;
            return req;
        }
    };

    inline NeighborhoodRequest make_neighborhood_request(
        std::string_view topology_name,
        std::size_t anchor_index,
        const TimeNeighborhoodRequest& request) {
        NeighborhoodRequest out{};
        out.topology_name = std::string(topology_name);
        out.anchor.row_index = anchor_index;
        out.query_kind = TopologyQueryKind::OrderedWindow;
        out.include_anchor = request.include_anchor;
        out.set_param("time.mode", static_cast<std::int64_t>(request.mode));
        out.set_param("time.last_n", request.last_n);
        out.set_param("time.lookback_delta", static_cast<std::int64_t>(request.lookback_delta));
        return out;
    }

    inline NeighborhoodRequest make_neighborhood_request(
        std::string_view topology_name,
        std::size_t anchor_index,
        const SpatialNeighborhoodRequest& request) {
        NeighborhoodRequest out{};
        out.topology_name = std::string(topology_name);
        out.anchor.row_index = anchor_index;
        out.query_kind = (request.mode == SpatialNeighborhoodMode::RadiusEuclidean)
            ? TopologyQueryKind::MetricRadius
            : TopologyQueryKind::MetricKNearest;
        out.include_anchor = request.include_anchor;
        out.set_param("spatial.mode", static_cast<std::int64_t>(request.mode));
        out.set_param("spatial.radius", request.radius);
        out.set_param("spatial.k_nearest", request.k_nearest);
        return out;
    }

    inline NeighborhoodRequest make_neighborhood_request(
        std::string_view topology_name,
        std::size_t anchor_index,
        const SpatioTemporalNeighborhoodRequest& request) {
        NeighborhoodRequest out{};
        out.topology_name = std::string(topology_name);
        out.anchor.row_index = anchor_index;
        out.query_kind = TopologyQueryKind::Intersection;
        out.include_anchor = request.time.include_anchor && request.spatial.include_anchor;
        out.set_param("time.mode", static_cast<std::int64_t>(request.time.mode));
        out.set_param("time.last_n", request.time.last_n);
        out.set_param("time.lookback_delta", static_cast<std::int64_t>(request.time.lookback_delta));
        out.set_param("time.include_anchor", request.time.include_anchor);
        out.set_param("spatial.mode", static_cast<std::int64_t>(request.spatial.mode));
        out.set_param("spatial.radius", request.spatial.radius);
        out.set_param("spatial.k_nearest", request.spatial.k_nearest);
        out.set_param("spatial.include_anchor", request.spatial.include_anchor);
        out.set_param("spatiotemporal.require_both", request.require_both);
        return out;
    }

    inline NeighborhoodRequest make_neighborhood_request(
        std::string_view topology_name,
        std::size_t anchor_index,
        const GraphNeighborhoodRequest& request) {
        NeighborhoodRequest out{};
        out.topology_name = std::string(topology_name);
        out.anchor.row_index = anchor_index;
        out.query_kind = request.weighted
            ? TopologyQueryKind::WeightedNeighborSet
            : TopologyQueryKind::NeighborSet;
        out.include_anchor = request.include_anchor;
        out.set_param("graph.direction", static_cast<std::int64_t>(request.direction));
        if (request.max_neighbors != std::numeric_limits<std::size_t>::max()) {
            out.set_param("graph.max_neighbors", request.max_neighbors);
        }
        return out;
    }

    inline NeighborhoodRequest make_neighborhood_request(
        std::string_view topology_name,
        std::size_t anchor_index,
        const HierarchyNeighborhoodRequest& request) {
        NeighborhoodRequest out{};
        out.topology_name = std::string(topology_name);
        out.anchor.row_index = anchor_index;
        out.query_kind = TopologyQueryKind::NeighborSet;
        out.include_anchor = request.include_anchor;
        out.set_param("hierarchy.traversal", static_cast<std::int64_t>(request.traversal));
        out.set_param("hierarchy.max_depth", request.max_depth);
        if (request.max_neighbors != std::numeric_limits<std::size_t>::max()) {
            out.set_param("hierarchy.max_neighbors", request.max_neighbors);
        }
        return out;
    }

    inline NeighborhoodSlice neighborhood_result_to_slice(const NeighborhoodResult& result) {
        if (result.contiguous) {
            return NeighborhoodSlice{ result.begin, result.end, result.anchor.row_index };
        }

        if (result.indices.empty()) {
            return NeighborhoodSlice{ 0, 0, result.anchor.row_index };
        }

        const bool ordered = std::is_sorted(result.indices.begin(), result.indices.end());
        if (!ordered) {
            throw std::logic_error("NeighborhoodResult cannot convert unordered selection into NeighborhoodSlice");
        }

        const std::size_t begin = result.indices.front();
        const std::size_t end = result.indices.back() + 1;
        for (std::size_t i = 0; i < result.indices.size(); ++i) {
            if (result.indices[i] != begin + i) {
                throw std::logic_error("NeighborhoodResult cannot convert non-contiguous selection into NeighborhoodSlice");
            }
        }

        return NeighborhoodSlice{ begin, end, result.anchor.row_index };
    }

    inline NeighborhoodSelection neighborhood_result_to_selection(const NeighborhoodResult& result) {
        NeighborhoodSelection out{};
        out.anchor = result.anchor.row_index;
        out.indices = result.materialize_indices();
        return out;
    }

    inline NeighborhoodResult make_neighborhood_result(
        std::string_view topology_name,
        const NeighborhoodAnchor& anchor,
        const NeighborhoodSlice& slice) {
        NeighborhoodResult out{};
        out.topology_name = std::string(topology_name);
        out.anchor = anchor;
        out.ordered = true;
        out.contiguous = true;
        out.begin = slice.begin;
        out.end = slice.end;
        return out;
    }

    inline NeighborhoodResult make_neighborhood_result(
        std::string_view topology_name,
        const NeighborhoodAnchor& anchor,
        const NeighborhoodSelection& selection,
        std::vector<double> distances = {},
        std::vector<double> weights = {},
        bool ordered = true) {
        NeighborhoodResult out{};
        out.topology_name = std::string(topology_name);
        out.anchor = anchor;
        out.indices = selection.indices;
        out.distances = std::move(distances);
        out.weights = std::move(weights);
        out.ordered = ordered;
        out.contiguous = false;
        return out;
    }

    inline NeighborhoodResult annotate_neighborhood_result(
        NeighborhoodResult result,
        const NeighborhoodRequest& request,
        std::size_t row_count_bound,
        std::size_t result_cap = std::numeric_limits<std::size_t>::max(),
        bool has_result_cap = false) {
        result.query_kind = request.query_kind;
        result.include_anchor = request.include_anchor;
        result.query_params = request.params;
        result.row_count_bound = row_count_bound;
        result.has_result_cap = has_result_cap;
        result.result_cap = result_cap;
        return result;
    }

    // Contract stub for locality requests.
    // Concrete implementations may support time-only, spatial-only, or
    // spatio-temporal locality over the same observation row domain.
    class NeighborhoodQuery {
    public:
        virtual ~NeighborhoodQuery() = default;

        virtual bool has_axis(AxisKind kind) const = 0;
        virtual std::size_t row_count() const = 0;
        virtual std::string_view topology_name() const = 0;
        virtual NeighborhoodResult query(const NeighborhoodRequest& request) const = 0;

        NeighborhoodSlice query_time(
            std::size_t anchor_index,
            const TimeNeighborhoodRequest& request) const {
            return neighborhood_result_to_slice(
                query(make_neighborhood_request(topology_name(), anchor_index, request)));
        }

        NeighborhoodSelection query_spatial(
            std::size_t anchor_index,
            const SpatialNeighborhoodRequest& request) const {
            return neighborhood_result_to_selection(
                query(make_neighborhood_request(topology_name(), anchor_index, request)));
        }

        NeighborhoodSelection query_spatiotemporal(
            std::size_t anchor_index,
            const SpatioTemporalNeighborhoodRequest& request) const {
            return neighborhood_result_to_selection(
                query(make_neighborhood_request(topology_name(), anchor_index, request)));
        }
    };

    class BoundNeighborhoodQuery final : public NeighborhoodQuery {
    public:
        BoundNeighborhoodQuery(std::unique_ptr<NeighborhoodQuery> inner, NeighborhoodAnchor anchor_defaults)
            : inner_(std::move(inner)), anchor_defaults_(std::move(anchor_defaults)) {
            if (!inner_) {
                throw std::invalid_argument("BoundNeighborhoodQuery: inner query must not be null");
            }
        }

        bool has_axis(AxisKind kind) const override { return inner_->has_axis(kind); }
        std::size_t row_count() const override { return inner_->row_count(); }
        std::string_view topology_name() const override { return inner_->topology_name(); }

        NeighborhoodResult query(const NeighborhoodRequest& request) const override {
            NeighborhoodRequest bound = request;
            if (bound.anchor.series.empty()) {
                bound.anchor.series = anchor_defaults_.series;
            }
            if (bound.anchor.entity.empty()) {
                bound.anchor.entity = anchor_defaults_.entity;
            }
            return inner_->query(bound);
        }

    private:
        std::unique_ptr<NeighborhoodQuery> inner_;
        NeighborhoodAnchor anchor_defaults_{};
    };

    inline std::unique_ptr<NeighborhoodQuery> bind_neighborhood_query_subject(
        std::unique_ptr<NeighborhoodQuery> inner,
        NeighborhoodAnchor anchor_defaults) {
        if (!inner) return {};
        if (anchor_defaults.series.empty() && anchor_defaults.entity.empty()) {
            return inner;
        }
        return std::make_unique<BoundNeighborhoodQuery>(std::move(inner), std::move(anchor_defaults));
    }

} // namespace sugar
