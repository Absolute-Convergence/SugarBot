#pragma once

#include <cstddef>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include "ingest/neighborhood_query.h"

namespace sugar::op_kernels {

struct TopologyNeighborhoodOptions {
    GraphNeighborhoodDirection direction{ GraphNeighborhoodDirection::Outgoing };
    bool include_anchor{ false };
    std::size_t max_neighbors{ std::numeric_limits<std::size_t>::max() };
    std::string graph_version{};
    std::string hierarchy_traversal{};
    bool has_hierarchy_max_depth{ false };
    std::size_t hierarchy_max_depth{ 1 };
};

std::vector<double> topology_neighbor_mean(
    std::span<const double> values,
    const NeighborhoodQuery& query,
    GraphNeighborhoodDirection direction,
    bool include_anchor,
    std::size_t max_neighbors);

std::vector<double> topology_neighbor_mean(
    std::span<const double> values,
    const NeighborhoodQuery& query,
    const TopologyNeighborhoodOptions& options);

std::vector<double> topology_neighbor_min(
    std::span<const double> values,
    const NeighborhoodQuery& query,
    GraphNeighborhoodDirection direction,
    bool include_anchor,
    std::size_t max_neighbors);

std::vector<double> topology_neighbor_min(
    std::span<const double> values,
    const NeighborhoodQuery& query,
    const TopologyNeighborhoodOptions& options);

std::vector<double> topology_neighbor_max(
    std::span<const double> values,
    const NeighborhoodQuery& query,
    GraphNeighborhoodDirection direction,
    bool include_anchor,
    std::size_t max_neighbors);

std::vector<double> topology_neighbor_max(
    std::span<const double> values,
    const NeighborhoodQuery& query,
    const TopologyNeighborhoodOptions& options);

std::vector<double> topology_weighted_sum(
    std::span<const double> values,
    const NeighborhoodQuery& query,
    GraphNeighborhoodDirection direction,
    bool include_anchor,
    std::size_t max_neighbors);

std::vector<double> topology_weighted_sum(
    std::span<const double> values,
    const NeighborhoodQuery& query,
    const TopologyNeighborhoodOptions& options);

std::vector<double> topology_degree(
    const NeighborhoodQuery& query,
    GraphNeighborhoodDirection direction,
    bool include_anchor,
    std::size_t max_neighbors,
    std::size_t output_size);

std::vector<double> topology_degree(
    const NeighborhoodQuery& query,
    const TopologyNeighborhoodOptions& options,
    std::size_t output_size);

} // namespace sugar::op_kernels
