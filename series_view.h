#pragma once

#include <span>
#include <cassert>
#include <cstddef>
#include "series.h"

namespace sugar {

    // SeriesView: non-owning "lens" over aligned numeric columns.
    // - Can be backed by a CandleSeries (preferred; provides OHLCV spans + series pointer).
    // - Can be backed by a close-only span (vector mode; no CandleSeries pointer).
    //
    // Design intent:
    // - Indicators can unify their math by consuming SeriesView.
    // - Existing indicators remain CandleSeries-based and do NOT need to change yet.
    class SeriesView {
    public:
        // ---- Constructors / factories ----

        // Full OHLCV view backed by CandleSeries caches (Hybrid AoS + SoA).
        static SeriesView from_series(const CandleSeries& s) {
            SeriesView v;
            v.series_ = &s;
            v.open_ = std::span<const double>(s.opens().data(), s.opens().size());
            v.high_ = std::span<const double>(s.highs().data(), s.highs().size());
            v.low_ = std::span<const double>(s.lows().data(), s.lows().size());
            v.close_ = std::span<const double>(s.closes().data(), s.closes().size());
            v.volume_ = std::span<const double>(s.volumes().data(), s.volumes().size());
            v.validate_sizes_();
            return v;
        }

        // Close-only view (vector mode). No CandleSeries backing.
        static SeriesView from_close(std::span<const double> close) {
            SeriesView v;
            v.series_ = nullptr;
            v.close_ = close;
            // open/high/low/volume remain empty spans
            return v;
        }

        // ---- Introspection ----
        std::size_t size() const { return close_.size(); }
        bool empty() const { return close_.empty(); }

        // Backing series is optional. Present if created from CandleSeries.
        bool has_series() const { return series_ != nullptr; }
        const CandleSeries& series() const {
            assert(series_ && "SeriesView::series() called but view has no backing CandleSeries.");
            return *series_;
        }

        // Column presence checks
        bool has_open() const { return !open_.empty(); }
        bool has_high() const { return !high_.empty(); }
        bool has_low() const { return !low_.empty(); }
        bool has_close() const { return !close_.empty(); } // should always be true for meaningful views
        bool has_volume() const { return !volume_.empty(); }

        // ---- Column accessors ----
        // Close is the canonical required column for most indicators.
        std::span<const double> close() const { return close_; }

        // Optional OHLCV columns. If not present, span will be empty.
        std::span<const double> open() const { return open_; }
        std::span<const double> high() const { return high_; }
        std::span<const double> low() const { return low_; }
        std::span<const double> volume() const { return volume_; }

    private:
        SeriesView() = default;

        void validate_sizes_() const {
#ifndef NDEBUG
            // If OHLCV spans are present, they must align to close by index.
            const std::size_t n = close_.size();
            if (!open_.empty())   assert(open_.size() == n);
            if (!high_.empty())   assert(high_.size() == n);
            if (!low_.empty())    assert(low_.size() == n);
            if (!volume_.empty()) assert(volume_.size() == n);
#endif
        }

        const CandleSeries* series_ = nullptr;

        std::span<const double> open_;
        std::span<const double> high_;
        std::span<const double> low_;
        std::span<const double> close_;
        std::span<const double> volume_;
    };

} // namespace sugar