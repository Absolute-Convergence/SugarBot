#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "indicator.h"
#include "series_view.h"

namespace sugar {

    class PrecomputedIndicator final : public Indicator {
    public:
        using Fn = std::function<std::vector<double>(const SeriesView&)>;

        PrecomputedIndicator(std::string name, Fn fn)
            : name_(std::move(name)), fn_(std::move(fn)) {
        }

        std::vector<double> compute(const CandleSeries& series) const override {
            return fn_(SeriesView::from_series(series));
        }

        std::vector<double> compute(const SeriesView& view) const override {
            return fn_(view);
        }

        const std::string& name() const { return name_; }

    private:
        std::string name_;
        Fn fn_;
    };

    inline IndicatorPtr make_precomputed(std::string name, PrecomputedIndicator::Fn fn) {
        return std::make_shared<PrecomputedIndicator>(std::move(name), std::move(fn));
    }

} // namespace sugar
