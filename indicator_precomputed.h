#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "indicator.h"
#include "series_view.h"

namespace sugar {

    // PrecomputedIndicator:
    // - A "leaf" indicator whose values are produced by a custom kernel.
    // - This is the clean bridge between stateful strategy logic and the graph world.
    //
    // Design intent:
    // - Use inside strategy plan builders to inject "wires" that are derived from
    //   pivot/state machines, timers, counters, blackout windows, etc.
    // - Still returns a normal numeric series, so it composes with bge/ble/cross/map_*.
    class PrecomputedIndicator final : public Indicator {
    public:
        using Fn = std::function<std::vector<double>(const SeriesView&)>;

        PrecomputedIndicator(std::string name, Fn fn)
            : name_(std::move(name)), fn_(std::move(fn)) {
        }

        // CandleSeries contract (required): forward into SeriesView kernel.
        std::vector<double> compute(const CandleSeries& series) const override {
            return fn_(SeriesView::from_series(series));
        }

        // SeriesView-first (supports vector-only mode if view has columns needed by fn).
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
