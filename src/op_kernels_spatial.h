#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "neighborhood_query.h"

namespace sugar::op_kernels {

std::vector<double> spatial_radius_mean(
    std::span<const double> x,
    const NeighborhoodQuery& query,
    double radius,
    bool include_self);

std::vector<double> st_radius_mean(
    std::span<const double> x,
    const NeighborhoodQuery& query,
    std::size_t last_n,
    double radius);

} // namespace sugar::op_kernels
