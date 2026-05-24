#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

#include "ingest/neighborhood_query_core.h"

namespace sugar {
    class TimeNeighborhoodQuery final : public NeighborhoodQuery {
    public:
        explicit TimeNeighborhoodQuery(std::span<const TimeKey> time)
            : time_(time) {
        }

        std::size_t row_count() const override { return time_.size(); }
        bool has_axis(AxisKind kind) const override { return kind == AxisKind::Time; }
        std::string_view topology_name() const override { return builtin_temporal_topology_name(); }

        NeighborhoodResult query(const NeighborhoodRequest& request) const override {
            if (!request.topology_name.empty() && request.topology_name != topology_name()) {
                throw std::invalid_argument("TimeNeighborhoodQuery: unsupported topology name");
            }
            if (request.anchor.row_index >= time_.size()) {
                throw std::out_of_range("TimeNeighborhoodQuery: anchor index out of range");
            }
            if (request.query_kind != TopologyQueryKind::OrderedWindow) {
                throw std::logic_error("TimeNeighborhoodQuery: only ordered-window queries are supported");
            }

            const auto* mode_value = request.find_param_as<std::int64_t>("time.mode");
            if (mode_value == nullptr) {
                throw std::invalid_argument("TimeNeighborhoodQuery: missing time.mode param");
            }

            const TimeNeighborhoodMode mode = static_cast<TimeNeighborhoodMode>(*mode_value);
            const NeighborhoodAnchor anchor = request.anchor;
            switch (mode) {
            case TimeNeighborhoodMode::LastNSamples: {
                const auto* last_n = request.find_param_as<std::size_t>("time.last_n");
                if (last_n == nullptr) {
                    throw std::invalid_argument("TimeNeighborhoodQuery: missing time.last_n param");
                }
                const auto slice = query_last_n(anchor.row_index, *last_n, request.include_anchor);
                return annotate_neighborhood_result(
                    make_neighborhood_result(topology_name(), anchor, slice),
                    request,
                    time_.size(),
                    *last_n,
                    true);
            }
            case TimeNeighborhoodMode::LookbackDelta: {
                const auto* delta = request.find_param_as<std::int64_t>("time.lookback_delta");
                if (delta == nullptr) {
                    throw std::invalid_argument("TimeNeighborhoodQuery: missing time.lookback_delta param");
                }
                const auto slice = query_lookback(anchor.row_index, static_cast<TimeKey>(*delta), request.include_anchor);
                return annotate_neighborhood_result(
                    make_neighborhood_result(topology_name(), anchor, slice),
                    request,
                    time_.size());
            }
            default:
                throw std::invalid_argument("TimeNeighborhoodQuery: unknown request mode");
            }
        }

    private:
        NeighborhoodSlice query_last_n(
            std::size_t anchor_index,
            std::size_t n,
            bool include_anchor) const {
            const std::size_t end = include_anchor ? (anchor_index + 1) : anchor_index;
            if (n == 0 || end == 0) {
                return NeighborhoodSlice{ end, end, anchor_index };
            }

            const std::size_t begin = (end > n) ? (end - n) : 0;
            return NeighborhoodSlice{ begin, end, anchor_index };
        }

        NeighborhoodSlice query_lookback(
            std::size_t anchor_index,
            TimeKey delta,
            bool include_anchor) const {
            if (delta < 0) {
                throw std::invalid_argument("TimeNeighborhoodQuery: lookback delta must be >= 0");
            }

            const std::size_t end = include_anchor ? (anchor_index + 1) : anchor_index;
            if (end == 0) {
                return NeighborhoodSlice{ 0, 0, anchor_index };
            }

            const std::int64_t anchor_t = static_cast<std::int64_t>(time_[anchor_index]);
            const std::int64_t min_t64 = anchor_t - static_cast<std::int64_t>(delta);
            const std::int64_t lo = static_cast<std::int64_t>(std::numeric_limits<TimeKey>::min());
            const TimeKey min_t = static_cast<TimeKey>((min_t64 < lo) ? lo : min_t64);

            const auto b = time_.begin();
            const auto e = b + static_cast<std::ptrdiff_t>(end);
            const auto it = std::lower_bound(b, e, min_t);
            const std::size_t begin = static_cast<std::size_t>(it - b);
            return NeighborhoodSlice{ begin, end, anchor_index };
        }

        std::span<const TimeKey> time_;
    };

    class SpatialNeighborhoodQuery2D final : public NeighborhoodQuery {
    public:
        SpatialNeighborhoodQuery2D(std::span<const double> x, std::span<const double> y)
            : x_(x), y_(y) {
            if (x_.size() != y_.size()) {
                throw std::invalid_argument("SpatialNeighborhoodQuery2D: x/y spans must have identical sizes");
            }
        }

        std::size_t row_count() const override { return x_.size(); }
        bool has_axis(AxisKind kind) const override { return kind == AxisKind::Spatial; }
        std::string_view topology_name() const override { return builtin_spatial_topology_name(); }

        NeighborhoodResult query(const NeighborhoodRequest& request) const override {
            if (!request.topology_name.empty() && request.topology_name != topology_name()) {
                throw std::invalid_argument("SpatialNeighborhoodQuery2D: unsupported topology name");
            }
            if (request.anchor.row_index >= x_.size()) {
                throw std::out_of_range("SpatialNeighborhoodQuery2D: anchor index out of range");
            }

            SpatialNeighborhoodRequest compat{};
            switch (request.query_kind) {
            case TopologyQueryKind::MetricRadius: {
                compat.mode = SpatialNeighborhoodMode::RadiusEuclidean;
                compat.include_anchor = request.include_anchor;
                const auto* radius = request.find_param_as<double>("spatial.radius");
                if (radius == nullptr) {
                    throw std::invalid_argument("SpatialNeighborhoodQuery2D: missing spatial.radius param");
                }
                compat.radius = *radius;
                break;
            }
            case TopologyQueryKind::MetricKNearest: {
                compat.mode = SpatialNeighborhoodMode::KNearest;
                compat.include_anchor = request.include_anchor;
                const auto* k_nearest = request.find_param_as<std::size_t>("spatial.k_nearest");
                if (k_nearest == nullptr) {
                    throw std::invalid_argument("SpatialNeighborhoodQuery2D: missing spatial.k_nearest param");
                }
                compat.k_nearest = *k_nearest;
                break;
            }
            default:
                throw std::logic_error("SpatialNeighborhoodQuery2D: only spatial metric queries are supported");
            }

            return select_spatial(request, compat);
        }

    private:
        NeighborhoodResult select_spatial(
            const NeighborhoodRequest& request,
            const SpatialNeighborhoodRequest& spatial_request) const {
            const NeighborhoodAnchor& anchor = request.anchor;
            const std::size_t anchor_index = anchor.row_index;
            if (anchor_index >= x_.size()) {
                throw std::out_of_range("SpatialNeighborhoodQuery2D: anchor index out of range");
            }

            struct Candidate {
                std::size_t idx{ 0 };
                double dist2{ 0.0 };
            };

            std::vector<Candidate> selected;
            selected.reserve(x_.size());

            const double ax = x_[anchor_index];
            const double ay = y_[anchor_index];
            if (!std::isfinite(ax) || !std::isfinite(ay)) {
                throw std::invalid_argument("SpatialNeighborhoodQuery2D: anchor coordinates must be finite");
            }

            for (std::size_t i = 0; i < x_.size(); ++i) {
                if (!spatial_request.include_anchor && i == anchor_index) continue;
                if (!std::isfinite(x_[i]) || !std::isfinite(y_[i])) continue;
                const double dx = x_[i] - ax;
                const double dy = y_[i] - ay;
                const double d2 = dx * dx + dy * dy;
                selected.push_back(Candidate{ i, d2 });
            }

            bool has_result_cap = false;
            std::size_t result_cap = std::numeric_limits<std::size_t>::max();
            if (spatial_request.mode == SpatialNeighborhoodMode::RadiusEuclidean) {
                if (spatial_request.radius < 0.0) {
                    throw std::invalid_argument("SpatialNeighborhoodQuery2D: radius must be >= 0");
                }
                const double radius2 = spatial_request.radius * spatial_request.radius;
                selected.erase(std::remove_if(selected.begin(), selected.end(),
                    [&](const Candidate& c) { return c.dist2 > radius2; }), selected.end());
            }
            else {
                std::stable_sort(selected.begin(), selected.end(),
                    [](const Candidate& a, const Candidate& b) {
                        if (a.dist2 != b.dist2) return a.dist2 < b.dist2;
                        return a.idx < b.idx;
                    });
                result_cap = spatial_request.k_nearest;
                has_result_cap = true;
                if (spatial_request.k_nearest < selected.size()) {
                    selected.resize(spatial_request.k_nearest);
                }
            }

            std::stable_sort(selected.begin(), selected.end(),
                [](const Candidate& a, const Candidate& b) {
                    if (a.idx != b.idx) return a.idx < b.idx;
                    return a.dist2 < b.dist2;
                });

            NeighborhoodSelection selection{};
            selection.anchor = anchor_index;
            selection.indices.reserve(selected.size());
            std::vector<double> distances;
            distances.reserve(selected.size());
            for (const auto& candidate : selected) {
                selection.indices.push_back(candidate.idx);
                distances.push_back(std::sqrt(candidate.dist2));
            }
            return annotate_neighborhood_result(
                make_neighborhood_result(
                    topology_name(),
                    anchor,
                    selection,
                    std::move(distances),
                    {},
                    true),
                request,
                row_count(),
                result_cap,
                has_result_cap);
        }

        std::span<const double> x_;
        std::span<const double> y_;
    };

    class SpatioTemporalNeighborhoodQuery2D final : public NeighborhoodQuery {
    public:
        SpatioTemporalNeighborhoodQuery2D(
            std::span<const TimeKey> time,
            std::span<const double> x,
            std::span<const double> y)
            : time_query_(time), spatial_query_(x, y), row_count_(time.size()) {
            if (time.size() != x.size() || x.size() != y.size()) {
                throw std::invalid_argument("SpatioTemporalNeighborhoodQuery2D: time/x/y spans must have identical sizes");
            }
        }

        std::size_t row_count() const override { return row_count_; }
        bool has_axis(AxisKind kind) const override {
            return kind == AxisKind::Time || kind == AxisKind::Spatial;
        }
        std::string_view topology_name() const override { return builtin_spatiotemporal_topology_name(); }

        NeighborhoodResult query(const NeighborhoodRequest& request) const override {
            if (!request.topology_name.empty() && request.topology_name != topology_name()) {
                throw std::invalid_argument("SpatioTemporalNeighborhoodQuery2D: unsupported topology name");
            }

            const std::size_t anchor_index = request.anchor.row_index;
            if (anchor_index >= row_count_) {
                throw std::out_of_range("SpatioTemporalNeighborhoodQuery2D: anchor index out of range");
            }

            if (request.query_kind == TopologyQueryKind::OrderedWindow) {
                NeighborhoodRequest time_request = request;
                time_request.topology_name = std::string(time_query_.topology_name());
                return time_query_.query(time_request);
            }
            if (request.query_kind == TopologyQueryKind::MetricRadius ||
                request.query_kind == TopologyQueryKind::MetricKNearest) {
                NeighborhoodRequest spatial_request = request;
                spatial_request.topology_name = std::string(spatial_query_.topology_name());
                return spatial_query_.query(spatial_request);
            }
            if (request.query_kind != TopologyQueryKind::Intersection) {
                throw std::logic_error("SpatioTemporalNeighborhoodQuery2D: unsupported query kind");
            }

            const auto* time_mode = request.find_param_as<std::int64_t>("time.mode");
            const auto* spatial_mode = request.find_param_as<std::int64_t>("spatial.mode");
            if (time_mode == nullptr || spatial_mode == nullptr) {
                throw std::invalid_argument("SpatioTemporalNeighborhoodQuery2D: missing nested time/spatial mode params");
            }

            TimeNeighborhoodRequest time_request{};
            time_request.mode = static_cast<TimeNeighborhoodMode>(*time_mode);
            time_request.include_anchor = request.include_anchor;
            if (const auto* time_include_anchor = request.find_param_as<bool>("time.include_anchor")) {
                time_request.include_anchor = *time_include_anchor;
            }
            if (time_request.mode == TimeNeighborhoodMode::LastNSamples) {
                const auto* last_n = request.find_param_as<std::size_t>("time.last_n");
                if (last_n == nullptr) {
                    throw std::invalid_argument("SpatioTemporalNeighborhoodQuery2D: missing time.last_n param");
                }
                time_request.last_n = *last_n;
            }
            else {
                const auto* lookback = request.find_param_as<std::int64_t>("time.lookback_delta");
                if (lookback == nullptr) {
                    throw std::invalid_argument("SpatioTemporalNeighborhoodQuery2D: missing time.lookback_delta param");
                }
                time_request.lookback_delta = static_cast<TimeKey>(*lookback);
            }

            SpatialNeighborhoodRequest spatial_request{};
            spatial_request.mode = static_cast<SpatialNeighborhoodMode>(*spatial_mode);
            spatial_request.include_anchor = request.include_anchor;
            if (const auto* spatial_include_anchor = request.find_param_as<bool>("spatial.include_anchor")) {
                spatial_request.include_anchor = *spatial_include_anchor;
            }
            if (spatial_request.mode == SpatialNeighborhoodMode::RadiusEuclidean) {
                const auto* radius = request.find_param_as<double>("spatial.radius");
                if (radius == nullptr) {
                    throw std::invalid_argument("SpatioTemporalNeighborhoodQuery2D: missing spatial.radius param");
                }
                spatial_request.radius = *radius;
            }
            else {
                const auto* k_nearest = request.find_param_as<std::size_t>("spatial.k_nearest");
                if (k_nearest == nullptr) {
                    throw std::invalid_argument("SpatioTemporalNeighborhoodQuery2D: missing spatial.k_nearest param");
                }
                spatial_request.k_nearest = *k_nearest;
            }

            bool require_both = true;
            if (const auto* require_both_param = request.find_param_as<bool>("spatiotemporal.require_both")) {
                require_both = *require_both_param;
            }

            const auto time_result = time_query_.query(
                make_neighborhood_request(time_query_.topology_name(), anchor_index, time_request));
            const auto spatial_result = spatial_query_.query(
                make_neighborhood_request(spatial_query_.topology_name(), anchor_index, spatial_request));
            const auto time_indices = time_result.materialize_indices();
            const auto spatial_indices = spatial_result.materialize_indices();

            std::vector<std::uint8_t> time_mask(row_count_, 0u);
            for (const auto idx : time_indices) {
                if (idx < time_mask.size()) {
                    time_mask[idx] = 1u;
                }
            }

            NeighborhoodSelection selection{};
            selection.anchor = anchor_index;
            if (require_both) {
                for (const auto idx : spatial_indices) {
                    if (idx < time_mask.size() && time_mask[idx]) {
                        selection.indices.push_back(idx);
                    }
                }
            }
            else {
                selection.indices = spatial_indices;
                for (const auto idx : time_indices) {
                    if (std::find(selection.indices.begin(), selection.indices.end(), idx) == selection.indices.end()) {
                        selection.indices.push_back(idx);
                    }
                }
                std::sort(selection.indices.begin(), selection.indices.end());
            }

            return annotate_neighborhood_result(
                make_neighborhood_result(topology_name(), request.anchor, selection, {}, {}, true),
                request,
                row_count_);
        }

    private:
        TimeNeighborhoodQuery time_query_;
        SpatialNeighborhoodQuery2D spatial_query_;
        std::size_t row_count_{ 0 };
    };

} // namespace sugar
