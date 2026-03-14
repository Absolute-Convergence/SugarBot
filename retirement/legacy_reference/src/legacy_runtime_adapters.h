#pragma once

#include <memory>

#include "align.h"
#include "data_view.h"
#include "series.h"
#include "series_view.h"

namespace sugar {

    bool make_single_series_dataview_from_series_view(
        const SeriesView& src,
        std::shared_ptr<align::AlignedBundle>& bundle_out,
        ColumnRef& primary_out);

    bool make_single_series_dataview_from_candles(
        const CandleSeries& series,
        std::shared_ptr<align::AlignedBundle>& bundle_out,
        ColumnRef& primary_out);

} // namespace sugar
