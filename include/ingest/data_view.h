#pragma once

#include <span>
#include <string>
#include <optional>
#include <memory>

#include "ingest/align.h"
#include "ingest/locality_subject.h"
#include "ingest/neighborhood_query.h"

namespace sugar {

    struct ColumnRef {
        std::string series;
        ColumnId col;
    };

    struct SpatialAxisValues2D {
        std::string x_name{};
        std::string y_name{};
        std::span<const double> x{};
        std::span<const double> y{};

        bool valid() const {
            return !x_name.empty() && !y_name.empty() && x.size() == y.size();
        }
    };

    class DataView {
    public:
        explicit DataView(const align::AlignedBundle& bundle) : bundle_(&bundle) {}

        std::span<const TimeKey> time() const;
        std::span<const TimeKey> time_axis() const;
        std::span<const double> col(const ColumnRef& ref) const;
        bool has_col(const ColumnRef& ref) const;
        std::size_t size() const;
        std::size_t row_count() const;
        bool empty() const;
        std::span<const AxisDef> axes() const;
        bool has_axis(AxisKind kind) const;
        bool has_axis(std::string_view name) const;
        const AxisDef* axis(AxisKind kind) const;
        const AxisDef* axis(std::string_view name) const;
        std::shared_ptr<const TopologyCatalog> topology_catalog() const;
        const TopologySpec* topology(std::string_view name) const;
        bool has_topology(std::string_view name) const;
        bool has_topology(TopologyFamily family) const;
        std::optional<SpatialAxisValues2D> spatial_axis_2d(std::string_view series) const;
        RuntimeLocalitySubject resolve_locality_subject(
            const ResolvedLocalitySubject& subject,
            std::string_view compatibility_primary_series = {}) const;
        std::unique_ptr<NeighborhoodQuery> resolve_topology(
            const RuntimeLocalitySubject& subject,
            std::string_view topology_name = {}) const;
        std::unique_ptr<NeighborhoodQuery> resolve_topology(
            const ResolvedLocalitySubject& subject,
            std::string_view compatibility_primary_series,
            std::string_view topology_name = {}) const;
        std::unique_ptr<NeighborhoodQuery> resolve_topology(
            std::string_view series,
            std::string_view topology_name = {}) const;
        std::unique_ptr<NeighborhoodQuery> make_neighborhood_query(
            const ResolvedLocalitySubject& subject,
            std::string_view compatibility_primary_series) const;
        std::unique_ptr<NeighborhoodQuery> make_neighborhood_query(std::string_view series) const;
        const align::AlignedBundle& bundle() const;
        const std::shared_ptr<IndexSchema>& index_schema() const;

    private:
        const align::AlignedBundle* bundle_ = nullptr;
    };

} // namespace sugar
