#include "legacy_runtime_adapters.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "numeric_frame.h"

namespace sugar {

    bool make_single_series_dataview_from_series_view(
        const SeriesView& src,
        std::shared_ptr<align::AlignedBundle>& bundle_out,
        ColumnRef& primary_out) {
        if (!src.has_close()) return false;

        auto schema = std::make_shared<FrameSchema>();
        std::vector<TimeKey> time(src.size(), 0);
        if (src.has_time()) {
            const auto t = src.time();
            if (t.size() == time.size()) {
                std::copy(t.begin(), t.end(), time.begin());
            }
        }

        NumericFrame frame(*schema, std::move(time));
        frame.set_index_schema(IndexSchema::make_time_only());

        const ColumnId close_id = schema->columns().get_or_add("close");
        {
            auto out = frame.ensure_col(close_id);
            const auto in = src.close();
            std::copy(in.begin(), in.end(), out.begin());
        }

        if (src.has_open()) {
            const ColumnId id = schema->columns().get_or_add("open");
            auto out = frame.ensure_col(id);
            const auto in = src.open();
            std::copy(in.begin(), in.end(), out.begin());
        }
        if (src.has_high()) {
            const ColumnId id = schema->columns().get_or_add("high");
            auto out = frame.ensure_col(id);
            const auto in = src.high();
            std::copy(in.begin(), in.end(), out.begin());
        }
        if (src.has_low()) {
            const ColumnId id = schema->columns().get_or_add("low");
            auto out = frame.ensure_col(id);
            const auto in = src.low();
            std::copy(in.begin(), in.end(), out.begin());
        }
        if (src.has_volume()) {
            const ColumnId id = schema->columns().get_or_add("volume");
            auto out = frame.ensure_col(id);
            const auto in = src.volume();
            std::copy(in.begin(), in.end(), out.begin());
        }

        std::vector<std::pair<std::string, NumericFrame>> inputs;
        inputs.emplace_back("__single__", std::move(frame));

        align::AlignConfig cfg{};
        cfg.calendar = align::CalendarPolicy::Intersection;
        cfg.missing = align::MissingPolicy::NaN;
        auto bundle = std::make_shared<align::AlignedBundle>(
            align::align_frames(schema, inputs, cfg));

        primary_out.series = "__single__";
        primary_out.col = close_id;
        bundle_out = std::move(bundle);
        return true;
    }

    bool make_single_series_dataview_from_candles(
        const CandleSeries& series,
        std::shared_ptr<align::AlignedBundle>& bundle_out,
        ColumnRef& primary_out) {
        auto schema = std::make_shared<FrameSchema>();
        NumericFrame frame = frame_from_candles(series, *schema);

        ColumnId close_id{};
        if (!schema->columns().find("close", close_id) || !frame.has_col(close_id)) {
            return false;
        }

        std::vector<std::pair<std::string, NumericFrame>> inputs;
        inputs.emplace_back("__single__", std::move(frame));

        align::AlignConfig cfg{};
        cfg.calendar = align::CalendarPolicy::Intersection;
        cfg.missing = align::MissingPolicy::NaN;
        auto bundle = std::make_shared<align::AlignedBundle>(
            align::align_frames(schema, inputs, cfg));

        primary_out.series = "__single__";
        primary_out.col = close_id;
        bundle_out = std::move(bundle);
        return true;
    }

} // namespace sugar
