#pragma once

#include <algorithm>
#include <cctype>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ingest/neighborhood_query_core.h"

namespace sugar {
    class HierarchyNeighborhoodQuery final : public NeighborhoodQuery {
    public:
        HierarchyNeighborhoodQuery(
            std::string topology_name,
            std::size_t rows,
            std::vector<StaticGraphEdge> parent_child_edges)
            : topology_name_(std::move(topology_name)),
              row_count_(rows),
              parent_of_(rows, npos()),
              children_(rows) {
            if (topology_name_.empty()) {
                throw std::invalid_argument("HierarchyNeighborhoodQuery: topology name must not be empty");
            }

            for (const auto& edge : parent_child_edges) {
                if (edge.source >= row_count_ || edge.target >= row_count_) {
                    throw std::out_of_range("HierarchyNeighborhoodQuery: edge endpoint out of range");
                }
                if (edge.source == edge.target) {
                    throw std::invalid_argument("HierarchyNeighborhoodQuery: self-parent edge is not allowed");
                }
                if (parent_of_[edge.target] != npos()) {
                    throw std::invalid_argument("HierarchyNeighborhoodQuery: child node has multiple parents");
                }
                parent_of_[edge.target] = edge.source;
                children_[edge.source].push_back(edge.target);
            }

            for (auto& children : children_) {
                std::sort(children.begin(), children.end());
            }
            validate_acyclic();
        }

        bool has_axis(AxisKind) const override { return false; }
        std::size_t row_count() const override { return row_count_; }
        std::string_view topology_name() const override { return topology_name_; }

        NeighborhoodResult query(const NeighborhoodRequest& request) const override {
            if (!request.topology_name.empty() && request.topology_name != topology_name()) {
                throw std::invalid_argument("HierarchyNeighborhoodQuery: unsupported topology name");
            }
            if (request.anchor.row_index >= row_count_) {
                throw std::out_of_range("HierarchyNeighborhoodQuery: anchor index out of range");
            }
            if (request.query_kind != TopologyQueryKind::NeighborSet) {
                throw std::logic_error("HierarchyNeighborhoodQuery: only hierarchy neighbor-set queries are supported");
            }

            std::vector<std::size_t> selected;
            if (request.include_anchor) {
                selected.push_back(request.anchor.row_index);
            }

            const HierarchyTraversal traversal = parse_traversal(request);
            const std::size_t max_depth = parse_max_depth(request);
            switch (traversal) {
            case HierarchyTraversal::Parent:
                append_parent_chain(request.anchor.row_index, max_depth, selected);
                break;
            case HierarchyTraversal::Children:
                append_children(request.anchor.row_index, max_depth, selected);
                break;
            case HierarchyTraversal::Siblings:
                append_siblings(request.anchor.row_index, selected);
                break;
            case HierarchyTraversal::ParentAndChildren:
                append_parent_chain(request.anchor.row_index, max_depth, selected);
                append_children(request.anchor.row_index, max_depth, selected);
                break;
            }

            dedupe_preserving_order(selected);
            const std::size_t max_neighbors = parse_max_neighbors(request);
            if (selected.size() > max_neighbors) {
                selected.resize(max_neighbors);
            }

            NeighborhoodSelection selection{};
            selection.anchor = request.anchor.row_index;
            selection.indices = std::move(selected);
            const std::size_t result_cap = max_neighbors;
            return annotate_neighborhood_result(
                make_neighborhood_result(topology_name(), request.anchor, selection, {}, {}, true),
                request,
                row_count_,
                result_cap,
                result_cap != std::numeric_limits<std::size_t>::max());
        }

    private:
        static constexpr std::size_t npos() {
            return std::numeric_limits<std::size_t>::max();
        }

        static std::string normalized_token(std::string value) {
            value.erase(std::remove_if(value.begin(), value.end(),
                [](unsigned char ch) {
                    return std::isspace(ch) != 0 || ch == '_' || ch == '-';
                }),
                value.end());
            std::transform(value.begin(), value.end(), value.begin(),
                [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            return value;
        }

        static HierarchyTraversal parse_traversal_string(const std::string& raw) {
            const std::string token = normalized_token(raw);
            if (token.empty() || token == "children" || token == "child" ||
                token == "descendants" || token == "descendant") {
                return HierarchyTraversal::Children;
            }
            if (token == "parent" || token == "parents" ||
                token == "ancestor" || token == "ancestors") {
                return HierarchyTraversal::Parent;
            }
            if (token == "sibling" || token == "siblings") {
                return HierarchyTraversal::Siblings;
            }
            if (token == "adjacent" || token == "parentchildren" ||
                token == "parentschildren" || token == "parentandchildren") {
                return HierarchyTraversal::ParentAndChildren;
            }
            throw std::invalid_argument("HierarchyNeighborhoodQuery: unknown hierarchy.traversal param");
        }

        static HierarchyTraversal parse_traversal_integral(std::int64_t raw) {
            switch (static_cast<HierarchyTraversal>(raw)) {
            case HierarchyTraversal::Parent:
            case HierarchyTraversal::Children:
            case HierarchyTraversal::Siblings:
            case HierarchyTraversal::ParentAndChildren:
                return static_cast<HierarchyTraversal>(raw);
            }
            throw std::invalid_argument("HierarchyNeighborhoodQuery: unknown hierarchy.traversal param");
        }

        static GraphNeighborhoodDirection parse_graph_direction(const NeighborhoodRequest& request) {
            const auto* value = request.find_param_as<std::int64_t>("graph.direction");
            if (!value) {
                return GraphNeighborhoodDirection::Outgoing;
            }
            switch (static_cast<GraphNeighborhoodDirection>(*value)) {
            case GraphNeighborhoodDirection::Outgoing:
            case GraphNeighborhoodDirection::Incoming:
            case GraphNeighborhoodDirection::Both:
                return static_cast<GraphNeighborhoodDirection>(*value);
            }
            throw std::invalid_argument("HierarchyNeighborhoodQuery: unknown graph.direction param");
        }

        static HierarchyTraversal parse_traversal(const NeighborhoodRequest& request) {
            if (const auto* text = request.find_param_as<std::string>("hierarchy.traversal")) {
                return parse_traversal_string(*text);
            }
            if (const auto* value = request.find_param_as<std::int64_t>("hierarchy.traversal")) {
                return parse_traversal_integral(*value);
            }

            switch (parse_graph_direction(request)) {
            case GraphNeighborhoodDirection::Incoming:
                return HierarchyTraversal::Parent;
            case GraphNeighborhoodDirection::Outgoing:
                return HierarchyTraversal::Children;
            case GraphNeighborhoodDirection::Both:
                return HierarchyTraversal::ParentAndChildren;
            }
            return HierarchyTraversal::Children;
        }

        static std::size_t parse_non_negative_size_param(
            const NeighborhoodRequest& request,
            std::string_view primary_name,
            std::string_view fallback_name,
            std::size_t fallback) {
            const auto read_one = [&](std::string_view name, std::size_t& out) -> bool {
                if (const auto* value = request.find_param_as<std::size_t>(name)) {
                    out = *value;
                    return true;
                }
                if (const auto* signed_value = request.find_param_as<std::int64_t>(name)) {
                    if (*signed_value < 0) {
                        throw std::invalid_argument(
                            "HierarchyNeighborhoodQuery: " + std::string(name) + " must be >= 0");
                    }
                    out = static_cast<std::size_t>(*signed_value);
                    return true;
                }
                return false;
            };

            std::size_t out = fallback;
            if (read_one(primary_name, out)) return out;
            if (!fallback_name.empty() && read_one(fallback_name, out)) return out;
            return out;
        }

        static std::size_t parse_max_depth(const NeighborhoodRequest& request) {
            return parse_non_negative_size_param(
                request,
                "hierarchy.max_depth",
                {},
                1);
        }

        static std::size_t parse_max_neighbors(const NeighborhoodRequest& request) {
            return parse_non_negative_size_param(
                request,
                "hierarchy.max_neighbors",
                "graph.max_neighbors",
                std::numeric_limits<std::size_t>::max());
        }

        static void dedupe_preserving_order(std::vector<std::size_t>& selected) {
            std::vector<std::size_t> out;
            out.reserve(selected.size());
            for (const auto idx : selected) {
                if (std::find(out.begin(), out.end(), idx) == out.end()) {
                    out.push_back(idx);
                }
            }
            selected = std::move(out);
        }

        void validate_acyclic() const {
            for (std::size_t start = 0; start < row_count_; ++start) {
                std::size_t current = start;
                for (std::size_t depth = 0; depth <= row_count_; ++depth) {
                    const std::size_t parent = parent_of_[current];
                    if (parent == npos()) {
                        break;
                    }
                    current = parent;
                    if (depth == row_count_) {
                        throw std::invalid_argument("HierarchyNeighborhoodQuery: cycle detected");
                    }
                }
            }
        }

        void append_parent_chain(
            std::size_t anchor,
            std::size_t max_depth,
            std::vector<std::size_t>& selected) const {
            std::size_t current = anchor;
            for (std::size_t depth = 0; depth < max_depth; ++depth) {
                const std::size_t parent = parent_of_[current];
                if (parent == npos()) {
                    break;
                }
                selected.push_back(parent);
                current = parent;
            }
        }

        void append_children(
            std::size_t anchor,
            std::size_t max_depth,
            std::vector<std::size_t>& selected) const {
            std::vector<std::size_t> frontier = children_[anchor];
            for (std::size_t depth = 0; depth < max_depth && !frontier.empty(); ++depth) {
                std::vector<std::size_t> next;
                for (const auto child : frontier) {
                    selected.push_back(child);
                    next.insert(next.end(), children_[child].begin(), children_[child].end());
                }
                std::sort(next.begin(), next.end());
                frontier = std::move(next);
            }
        }

        void append_siblings(
            std::size_t anchor,
            std::vector<std::size_t>& selected) const {
            const std::size_t parent = parent_of_[anchor];
            if (parent == npos()) {
                return;
            }
            for (const auto sibling : children_[parent]) {
                if (sibling != anchor) {
                    selected.push_back(sibling);
                }
            }
        }

        std::string topology_name_{};
        std::size_t row_count_{ 0 };
        std::vector<std::size_t> parent_of_{};
        std::vector<std::vector<std::size_t>> children_{};
    };

} // namespace sugar
