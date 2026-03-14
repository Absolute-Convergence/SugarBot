#include "op_kernels_spatial.h"

#include <cmath>
#include <limits>

namespace sugar::op_kernels {

namespace {

double qnan() {
    return std::numeric_limits<double>::quiet_NaN();
}

bool fill_mean_from_selection(
    std::span<const double> x,
    const NeighborhoodSelection& sel,
    double& out_value) {
    if (sel.indices.empty()) return false;

    long double sum = 0.0L;
    std::size_t count = 0;
    for (const auto idx : sel.indices) {
        if (idx >= x.size()) return false;
        const double v = x[idx];
        if (!std::isfinite(v)) return false;
        sum += static_cast<long double>(v);
        ++count;
    }

    if (count == 0) return false;
    out_value = static_cast<double>(sum / static_cast<long double>(count));
    return std::isfinite(out_value);
}

} // namespace

std::vector<double> spatial_radius_mean(
    std::span<const double> x,
    const NeighborhoodQuery& query,
    double radius,
    bool include_self) {
    std::vector<double> out(x.size(), qnan());
    if (!query.has_axis(AxisKind::Spatial)) return out;

    const auto req = SpatialNeighborhoodRequest::radius_euclidean(radius, include_self);
    const std::size_t n = std::min(x.size(), query.row_count());
    for (std::size_t i = 0; i < n; ++i) {
        const auto sel = query.query_spatial(i, req);
        double mean = qnan();
        if (fill_mean_from_selection(x, sel, mean)) {
            out[i] = mean;
        }
    }
    return out;
}

std::vector<double> st_radius_mean(
    std::span<const double> x,
    const NeighborhoodQuery& query,
    std::size_t last_n,
    double radius) {
    std::vector<double> out(x.size(), qnan());
    if (last_n == 0) return out;
    if (!query.has_axis(AxisKind::Time) || !query.has_axis(AxisKind::Spatial)) return out;

    const auto req = SpatioTemporalNeighborhoodRequest::intersection(
        TimeNeighborhoodRequest::last_n_samples(last_n, true),
        SpatialNeighborhoodRequest::radius_euclidean(radius, true));
    const std::size_t n = std::min(x.size(), query.row_count());
    for (std::size_t i = 0; i < n; ++i) {
        if (i + 1 < last_n) continue;
        const auto sel = query.query_spatiotemporal(i, req);
        double mean = qnan();
        if (fill_mean_from_selection(x, sel, mean)) {
            out[i] = mean;
        }
    }
    return out;
}

} // namespace sugar::op_kernels
