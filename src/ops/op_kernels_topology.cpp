#include "op_kernels_topology.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

#include "ops/kahan_accumulator.h"

namespace sugar::op_kernels {

namespace {

double qnan() {
    return std::numeric_limits<double>::quiet_NaN();
}

NeighborhoodRequest make_neighbor_request(
    const NeighborhoodQuery& query,
    std::size_t anchor,
    TopologyQueryKind kind,
    const TopologyNeighborhoodOptions& options) {
    NeighborhoodRequest request{};
    request.topology_name = std::string(query.topology_name());
    request.anchor.row_index = anchor;
    request.query_kind = kind;
    request.include_anchor = options.include_anchor;
    request.set_param("graph.direction", static_cast<std::int64_t>(options.direction));
    if (options.max_neighbors != std::numeric_limits<std::size_t>::max()) {
        request.set_param("graph.max_neighbors", options.max_neighbors);
    }
    if (!options.graph_version.empty()) {
        request.set_param("graph.version", options.graph_version);
    }
    if (!options.hierarchy_traversal.empty()) {
        request.set_param("hierarchy.traversal", options.hierarchy_traversal);
    }
    if (options.has_hierarchy_max_depth) {
        request.set_param("hierarchy.max_depth", options.hierarchy_max_depth);
    }
    return request;
}

TopologyNeighborhoodOptions make_options(
    GraphNeighborhoodDirection direction,
    bool include_anchor,
    std::size_t max_neighbors) {
    TopologyNeighborhoodOptions options{};
    options.direction = direction;
    options.include_anchor = include_anchor;
    options.max_neighbors = max_neighbors;
    return options;
}

template <class Fn>
std::vector<double> evaluate_neighbor_selection(
    std::span<const double> values,
    const NeighborhoodQuery& query,
    const TopologyNeighborhoodOptions& options,
    Fn&& fn) {
    std::vector<double> out(values.size(), qnan());
    const std::size_t n = std::min(values.size(), query.row_count());
    for (std::size_t i = 0; i < n; ++i) {
        const auto result = query.query(make_neighbor_request(
            query,
            i,
            TopologyQueryKind::NeighborSet,
            options));
        out[i] = fn(result.materialize_indices());
    }
    return out;
}

double mean_from_indices(std::span<const double> values, const std::vector<std::size_t>& indices) {
    if (indices.empty()) return qnan();

    numeric::kahan_accumulator sum{};
    for (const auto idx : indices) {
        if (idx >= values.size()) return qnan();
        const double value = values[idx];
        if (!std::isfinite(value)) return qnan();
        sum.add(value);
    }

    const double mean = sum.value() / static_cast<double>(indices.size());
    return std::isfinite(mean) ? mean : qnan();
}

double min_from_indices(std::span<const double> values, const std::vector<std::size_t>& indices) {
    if (indices.empty()) return qnan();

    double best = std::numeric_limits<double>::infinity();
    for (const auto idx : indices) {
        if (idx >= values.size()) return qnan();
        const double value = values[idx];
        if (!std::isfinite(value)) return qnan();
        best = std::min(best, value);
    }
    return std::isfinite(best) ? best : qnan();
}

double max_from_indices(std::span<const double> values, const std::vector<std::size_t>& indices) {
    if (indices.empty()) return qnan();

    double best = -std::numeric_limits<double>::infinity();
    for (const auto idx : indices) {
        if (idx >= values.size()) return qnan();
        const double value = values[idx];
        if (!std::isfinite(value)) return qnan();
        best = std::max(best, value);
    }
    return std::isfinite(best) ? best : qnan();
}

} // namespace

std::vector<double> topology_neighbor_mean(
    std::span<const double> values,
    const NeighborhoodQuery& query,
    GraphNeighborhoodDirection direction,
    bool include_anchor,
    std::size_t max_neighbors) {
    return topology_neighbor_mean(values, query, make_options(direction, include_anchor, max_neighbors));
}

std::vector<double> topology_neighbor_mean(
    std::span<const double> values,
    const NeighborhoodQuery& query,
    const TopologyNeighborhoodOptions& options) {
    return evaluate_neighbor_selection(
        values,
        query,
        options,
        [&](const std::vector<std::size_t>& indices) {
            return mean_from_indices(values, indices);
        });
}

std::vector<double> topology_neighbor_min(
    std::span<const double> values,
    const NeighborhoodQuery& query,
    GraphNeighborhoodDirection direction,
    bool include_anchor,
    std::size_t max_neighbors) {
    return topology_neighbor_min(values, query, make_options(direction, include_anchor, max_neighbors));
}

std::vector<double> topology_neighbor_min(
    std::span<const double> values,
    const NeighborhoodQuery& query,
    const TopologyNeighborhoodOptions& options) {
    return evaluate_neighbor_selection(
        values,
        query,
        options,
        [&](const std::vector<std::size_t>& indices) {
            return min_from_indices(values, indices);
        });
}

std::vector<double> topology_neighbor_max(
    std::span<const double> values,
    const NeighborhoodQuery& query,
    GraphNeighborhoodDirection direction,
    bool include_anchor,
    std::size_t max_neighbors) {
    return topology_neighbor_max(values, query, make_options(direction, include_anchor, max_neighbors));
}

std::vector<double> topology_neighbor_max(
    std::span<const double> values,
    const NeighborhoodQuery& query,
    const TopologyNeighborhoodOptions& options) {
    return evaluate_neighbor_selection(
        values,
        query,
        options,
        [&](const std::vector<std::size_t>& indices) {
            return max_from_indices(values, indices);
        });
}

std::vector<double> topology_weighted_sum(
    std::span<const double> values,
    const NeighborhoodQuery& query,
    GraphNeighborhoodDirection direction,
    bool include_anchor,
    std::size_t max_neighbors) {
    return topology_weighted_sum(values, query, make_options(direction, include_anchor, max_neighbors));
}

std::vector<double> topology_weighted_sum(
    std::span<const double> values,
    const NeighborhoodQuery& query,
    const TopologyNeighborhoodOptions& options) {
    std::vector<double> out(values.size(), qnan());
    const std::size_t n = std::min(values.size(), query.row_count());
    for (std::size_t i = 0; i < n; ++i) {
        const auto result = query.query(make_neighbor_request(
            query,
            i,
            TopologyQueryKind::WeightedNeighborSet,
            options));
        const auto indices = result.materialize_indices();
        if (indices.empty()) {
            out[i] = 0.0;
            continue;
        }
        if (result.weights.size() != indices.size()) {
            continue;
        }

        numeric::kahan_accumulator sum{};
        bool valid = true;
        for (std::size_t j = 0; j < indices.size(); ++j) {
            const auto idx = indices[j];
            if (idx >= values.size() ||
                !std::isfinite(values[idx]) ||
                !std::isfinite(result.weights[j])) {
                valid = false;
                break;
            }
            sum.add(values[idx] * result.weights[j]);
        }
        if (valid && std::isfinite(sum.value())) {
            out[i] = sum.value();
        }
    }
    return out;
}

std::vector<double> topology_degree(
    const NeighborhoodQuery& query,
    GraphNeighborhoodDirection direction,
    bool include_anchor,
    std::size_t max_neighbors,
    std::size_t output_size) {
    return topology_degree(query, make_options(direction, include_anchor, max_neighbors), output_size);
}

std::vector<double> topology_degree(
    const NeighborhoodQuery& query,
    const TopologyNeighborhoodOptions& options,
    std::size_t output_size) {
    std::vector<double> out(output_size, qnan());
    const std::size_t n = std::min(output_size, query.row_count());
    for (std::size_t i = 0; i < n; ++i) {
        const auto result = query.query(make_neighbor_request(
            query,
            i,
            TopologyQueryKind::NeighborSet,
            options));
        const auto indices = result.materialize_indices();
        const bool in_range = std::all_of(indices.begin(), indices.end(),
            [&](std::size_t idx) { return idx < output_size; });
        if (in_range) {
            out[i] = static_cast<double>(indices.size());
        }
    }
    return out;
}

} // namespace sugar::op_kernels
