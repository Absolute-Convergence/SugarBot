#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ingest/numeric_frame.h"
#include "ingest/topology_spec.h"

namespace sugar::align {

    enum class CalendarPolicy {
        Intersection,
        Union,
    };

    enum class MissingPolicy {
        NaN,
    };

    enum class NonTemporalMultiSeriesPolicy {
        Reject,
        EqualLengthRowDomain,
    };

    std::string_view non_temporal_multi_series_policy_name(NonTemporalMultiSeriesPolicy policy);

    struct AlignConfig {
        CalendarPolicy calendar{ CalendarPolicy::Intersection };
        MissingPolicy missing{ MissingPolicy::NaN };
        NonTemporalMultiSeriesPolicy non_temporal_multi_series{ NonTemporalMultiSeriesPolicy::Reject };
    };

    struct AlignedBundle {
        std::shared_ptr<FrameSchema> schema;
        std::shared_ptr<IndexSchema> index_schema;
        std::shared_ptr<TopologyCatalog> topology_catalog;
        std::vector<TimeKey> time;            // legacy storage for row ordering / time-aligned calendars
        std::vector<std::string> series_keys; // stable order
        std::vector<std::string> entity_keys; // optional parallel entity labels; defaults to series_keys
        std::vector<NumericFrame> frames;     // aligned frames, one per series_key

        std::size_t find_series_index(std::string_view key) const;
        std::size_t find_series_index_for_entity(std::string_view key) const;
        std::size_t row_count() const;
        bool empty() const;
        bool has_index_schema() const;
        bool has_time_axis() const;
        std::span<const AxisDef> axes() const;
        bool has_axis(AxisKind kind) const;
        bool has_axis(std::string_view name) const;
        const AxisDef* axis(AxisKind kind) const;
        const AxisDef* axis(std::string_view name) const;
        std::string_view entity_key_for_series_index(std::size_t index) const;
        std::span<const TimeKey> time_axis() const;
    };

    AlignedBundle align_frames(
        std::shared_ptr<FrameSchema> schema,
        const std::vector<std::pair<std::string, NumericFrame>>& inputs,
        AlignConfig cfg);

} // namespace sugar::align
