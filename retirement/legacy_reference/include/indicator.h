#pragma once

#include <vector>
#include <memory>

#include "series.h"
#include "series_view.h"

namespace sugar {

    class Indicator {
    public:
        virtual ~Indicator() = default;

        // SeriesView is canonical and required for all indicators.
        virtual std::vector<double> compute(const SeriesView& view) const = 0;

        // CandleSeries compatibility path: forwards to SeriesView.
        virtual std::vector<double> compute(const CandleSeries& series) const {
            return compute(SeriesView::from_series(series));
        }
    };

    using IndicatorPtr = std::shared_ptr<Indicator>;

} // namespace sugar
