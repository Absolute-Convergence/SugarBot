#pragma once

#include <algorithm>
#include <vector>

#include "ingest/numeric_frame.h"
#include "legacy/series.h"

namespace sugar {

    // Explicit legacy adapter: CandleSeries is converted into NumericFrame before runtime.
    inline NumericFrame frame_from_candles(const CandleSeries& series, FrameSchema& schema) {
        NumericFrame frame(schema, std::vector<TimeKey>(series.dates().begin(), series.dates().end()));
        frame.set_index_schema(IndexSchema::make_time_only());

        const ColumnId open_id = schema.columns().get_or_add("open");
        const ColumnId high_id = schema.columns().get_or_add("high");
        const ColumnId low_id = schema.columns().get_or_add("low");
        const ColumnId close_id = schema.columns().get_or_add("close");
        const ColumnId volume_id = schema.columns().get_or_add("volume");

        {
            auto out = frame.ensure_col(open_id);
            std::copy(series.opens().begin(), series.opens().end(), out.begin());
        }
        {
            auto out = frame.ensure_col(high_id);
            std::copy(series.highs().begin(), series.highs().end(), out.begin());
        }
        {
            auto out = frame.ensure_col(low_id);
            std::copy(series.lows().begin(), series.lows().end(), out.begin());
        }
        {
            auto out = frame.ensure_col(close_id);
            std::copy(series.closes().begin(), series.closes().end(), out.begin());
        }
        {
            auto out = frame.ensure_col(volume_id);
            std::copy(series.volumes().begin(), series.volumes().end(), out.begin());
        }

        return frame;
    }

} // namespace sugar
