#pragma once

#include <vector>
#include <memory>
#include <limits>

#include "series.h"
#include "series_view.h"

namespace sugar {

    class Indicator {
    public:
        virtual ~Indicator() = default;

        // Existing contract (unchanged): required for all indicators.
        // Output vector must be aligned to the input series.
        virtual std::vector<double> compute(const CandleSeries& series) const = 0;

        // New additive contract (non-breaking):
        // Indicators MAY override this to support vector-backed or view-backed evaluation.
        //
        // Default behavior:
        // - If the view is backed by a CandleSeries, forward to compute(series).
        // - Otherwise, return a NaN vector of view.size() to signal "not supported" unless overridden.
        virtual std::vector<double> compute(const SeriesView& view) const {
            if (view.has_series()) {
                return compute(view.series());
            }

            // Close-only view: default is "unsupported" unless indicator overrides.
            std::vector<double> out(view.size(), std::numeric_limits<double>::quiet_NaN());
            return out;
        }
    };

    using IndicatorPtr = std::shared_ptr<Indicator>;

} // namespace sugar