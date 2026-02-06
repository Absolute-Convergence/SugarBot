#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "indicators_composite_bool.h" // BoolSignal, BoolSeries
#include "series_view.h"

namespace sugar {

    class PrecomputedBoolSignal final : public BoolSignal {
    public:
        using Fn = std::function<BoolSeries(const SeriesView&)>;

        PrecomputedBoolSignal(std::string name, Fn fn)
            : name_(std::move(name)), fn_(std::move(fn)) {
        }

        BoolSeries compute(const CandleSeries& series) const override {
            return fn_(SeriesView::from_series(series));
        }

        BoolSeries compute(const SeriesView& view) const override {
            return fn_(view);
        }

        const std::string& name() const { return name_; }

    private:
        std::string name_;
        Fn fn_;
    };

    inline BoolSignalPtr make_precomputed_bool(std::string name, PrecomputedBoolSignal::Fn fn) {
        return std::make_shared<PrecomputedBoolSignal>(std::move(name), std::move(fn));
    }

} // namespace sugar
