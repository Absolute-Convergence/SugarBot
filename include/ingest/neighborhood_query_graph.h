#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ingest/neighborhood_query_core.h"

namespace sugar {
    class StaticGraphNeighborhoodQuery final : public NeighborhoodQuery {
    public:
        StaticGraphNeighborhoodQuery(
            std::string topology_name,
            std::size_t rows,
            std::vector<StaticGraphEdge> edges,
            bool directed = true,
            bool weighted = false,
            std::span<const TimeKey> time_axis = {})
            : topology_name_(std::move(topology_name)),
              row_count_(rows),
              edges_(std::move(edges)),
              directed_(directed),
              weighted_(weighted),
              time_axis_(time_axis) {
            if (topology_name_.empty()) {
                throw std::invalid_argument("StaticGraphNeighborhoodQuery: topology name must not be empty");
            }
            if (!time_axis_.empty() && time_axis_.size() != row_count_) {
                throw std::invalid_argument("StaticGraphNeighborhoodQuery: time axis size must match row count");
            }
            for (auto& edge : edges_) {
                if (edge.source >= row_count_ || edge.target >= row_count_) {
                    throw std::out_of_range("StaticGraphNeighborhoodQuery: edge endpoint out of range");
                }
                if (!std::isfinite(edge.weight)) {
                    throw std::invalid_argument("StaticGraphNeighborhoodQuery: edge weight must be finite");
                }
                if (edge.valid_from || edge.valid_to) {
                    has_time_valid_edges_ = true;
                }
                if (edge.valid_from && edge.valid_to && *edge.valid_to <= *edge.valid_from) {
                    throw std::invalid_argument(
                        "StaticGraphNeighborhoodQuery: edge valid_to must be greater than valid_from");
                }
                if (!weighted_) {
                    edge.weight = 1.0;
                }
            }
        }

        bool has_axis(AxisKind) const override { return false; }
        std::size_t row_count() const override { return row_count_; }
        std::string_view topology_name() const override { return topology_name_; }

        NeighborhoodResult query(const NeighborhoodRequest& request) const override {
            if (!request.topology_name.empty() && request.topology_name != topology_name()) {
                throw std::invalid_argument("StaticGraphNeighborhoodQuery: unsupported topology name");
            }
            if (request.anchor.row_index >= row_count_) {
                throw std::out_of_range("StaticGraphNeighborhoodQuery: anchor index out of range");
            }
            if (request.query_kind != TopologyQueryKind::NeighborSet &&
                request.query_kind != TopologyQueryKind::WeightedNeighborSet)
            {
                throw std::logic_error("StaticGraphNeighborhoodQuery: only graph neighbor-set queries are supported");
            }

            const GraphNeighborhoodDirection direction = parse_direction(request);
            const std::size_t max_neighbors = parse_max_neighbors(request);
            const bool emit_weights = request.query_kind == TopologyQueryKind::WeightedNeighborSet;
            const GraphFilterContext filter = make_filter_context(request);

            struct Candidate {
                std::size_t index{ 0 };
                double weight{ 0.0 };
            };

            std::vector<Candidate> candidates;
            const auto add_candidate = [&](std::size_t index, double weight) {
                if (!request.include_anchor && index == request.anchor.row_index) return;
                const auto it = std::find_if(candidates.begin(), candidates.end(),
                    [&](const Candidate& candidate) { return candidate.index == index; });
                if (it != candidates.end()) {
                    it->weight += weight;
                }
                else {
                    candidates.push_back(Candidate{ index, weight });
                }
            };

            const std::size_t anchor = request.anchor.row_index;
            for (const auto& edge : edges_) {
                if (!edge_active(edge, filter)) {
                    continue;
                }
                const double weight = weighted_ ? edge.weight : 1.0;
                if (directed_) {
                    const bool outgoing =
                        (direction == GraphNeighborhoodDirection::Outgoing ||
                         direction == GraphNeighborhoodDirection::Both) &&
                        edge.source == anchor;
                    const bool incoming =
                        (direction == GraphNeighborhoodDirection::Incoming ||
                         direction == GraphNeighborhoodDirection::Both) &&
                        edge.target == anchor;
                    if (outgoing) {
                        add_candidate(edge.target, weight);
                    }
                    if (incoming && !(outgoing && edge.source == edge.target)) {
                        add_candidate(edge.source, weight);
                    }
                }
                else {
                    if (edge.source == anchor) {
                        add_candidate(edge.target, weight);
                    }
                    else if (edge.target == anchor) {
                        add_candidate(edge.source, weight);
                    }
                }
            }

            std::stable_sort(candidates.begin(), candidates.end(),
                [](const Candidate& lhs, const Candidate& rhs) {
                    return lhs.index < rhs.index;
                });
            if (max_neighbors < candidates.size()) {
                candidates.resize(max_neighbors);
            }

            NeighborhoodSelection selection{};
            selection.anchor = anchor;
            selection.indices.reserve(candidates.size());
            std::vector<double> weights;
            if (emit_weights) {
                weights.reserve(candidates.size());
            }
            for (const auto& candidate : candidates) {
                selection.indices.push_back(candidate.index);
                if (emit_weights) {
                    weights.push_back(candidate.weight);
                }
            }

            return annotate_neighborhood_result(
                make_neighborhood_result(
                    topology_name(),
                    request.anchor,
                    selection,
                    {},
                    std::move(weights),
                    true),
                request,
                row_count_,
                max_neighbors,
                max_neighbors != std::numeric_limits<std::size_t>::max());
        }

    private:
        struct GraphFilterContext {
            bool has_time{ false };
            TimeKey time{ 0 };
            bool has_version{ false };
            std::string version{};
        };

        GraphNeighborhoodDirection parse_direction(const NeighborhoodRequest& request) const {
            const auto* value = request.find_param_as<std::int64_t>("graph.direction");
            if (!value) {
                return directed_ ? GraphNeighborhoodDirection::Outgoing : GraphNeighborhoodDirection::Both;
            }
            switch (static_cast<GraphNeighborhoodDirection>(*value)) {
            case GraphNeighborhoodDirection::Outgoing:
            case GraphNeighborhoodDirection::Incoming:
            case GraphNeighborhoodDirection::Both:
                return static_cast<GraphNeighborhoodDirection>(*value);
            }
            throw std::invalid_argument("StaticGraphNeighborhoodQuery: unknown graph.direction param");
        }

        std::size_t parse_max_neighbors(const NeighborhoodRequest& request) const {
            if (const auto* value = request.find_param_as<std::size_t>("graph.max_neighbors")) {
                return *value;
            }
            if (const auto* signed_value = request.find_param_as<std::int64_t>("graph.max_neighbors")) {
                if (*signed_value < 0) {
                    throw std::invalid_argument("StaticGraphNeighborhoodQuery: graph.max_neighbors must be >= 0");
                }
                return static_cast<std::size_t>(*signed_value);
            }
            return std::numeric_limits<std::size_t>::max();
        }

        GraphFilterContext make_filter_context(const NeighborhoodRequest& request) const {
            GraphFilterContext out{};
            if (const auto* version_param = request.find_param("graph.version")) {
                const auto* version = std::get_if<std::string>(version_param);
                if (!version) {
                    throw std::invalid_argument("StaticGraphNeighborhoodQuery: graph.version must be string");
                }
                out.version = *version;
                out.has_version = !out.version.empty();
            }

            if (read_time_param(request, "graph.time", out.time) ||
                read_time_param(request, "time.anchor", out.time)) {
                out.has_time = true;
            }
            else if (!time_axis_.empty()) {
                out.time = time_axis_[request.anchor.row_index];
                out.has_time = true;
            }

            if (has_time_valid_edges_ && !out.has_time) {
                throw std::invalid_argument(
                    "StaticGraphNeighborhoodQuery: time-valid edges require graph.time or a bound time axis");
            }
            return out;
        }

        static bool read_time_param(
            const NeighborhoodRequest& request,
            std::string_view name,
            TimeKey& out) {
            const auto* value = request.find_param(name);
            if (!value) {
                return false;
            }
            if (const auto* signed_value = std::get_if<std::int64_t>(value)) {
                out = static_cast<TimeKey>(*signed_value);
                return true;
            }
            if (const auto* size_value = std::get_if<std::size_t>(value)) {
                if (*size_value > static_cast<std::size_t>(std::numeric_limits<TimeKey>::max())) {
                    throw std::invalid_argument(
                        "StaticGraphNeighborhoodQuery: " + std::string(name) + " must fit TimeKey");
                }
                out = static_cast<TimeKey>(*size_value);
                return true;
            }
            throw std::invalid_argument(
                "StaticGraphNeighborhoodQuery: " + std::string(name) + " must be integer");
        }

        bool edge_active(const StaticGraphEdge& edge, const GraphFilterContext& filter) const {
            if (!edge.version.empty() && (!filter.has_version || edge.version != filter.version)) {
                return false;
            }
            if (edge.valid_from || edge.valid_to) {
                if (!filter.has_time) {
                    throw std::invalid_argument(
                        "StaticGraphNeighborhoodQuery: time-valid edges require graph.time or a bound time axis");
                }
                if (edge.valid_from && filter.time < *edge.valid_from) {
                    return false;
                }
                if (edge.valid_to && filter.time >= *edge.valid_to) {
                    return false;
                }
            }
            return true;
        }

        std::string topology_name_{};
        std::size_t row_count_{ 0 };
        std::vector<StaticGraphEdge> edges_{};
        bool directed_{ true };
        bool weighted_{ false };
        std::span<const TimeKey> time_axis_{};
        bool has_time_valid_edges_{ false };
    };

} // namespace sugar
