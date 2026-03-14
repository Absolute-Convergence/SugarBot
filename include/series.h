#pragma once

#include <vector>
#include <cstddef>
#include "candle.h"

namespace sugar {

    class CandleSeries {
    public:
        explicit CandleSeries(std::vector<Candle> rows);

        // --- existing API ---
        const std::vector<Candle>& rows() const { return rows_; }
        std::size_t size() const { return rows_.size(); }
        const Candle& operator[](std::size_t i) const { return rows_[i]; }

        // --- cached SoA accessors ---
        const std::vector<double>& opens() const { return opens_; }
        const std::vector<double>& highs() const { return highs_; }
        const std::vector<double>& lows() const { return lows_; }
        const std::vector<double>& closes() const { return closes_; }
        const std::vector<double>& volumes() const { return volumes_; }
        const std::vector<TimeKey>& dates() const { return dates_; }

    private:
        // AoS source of truth
        std::vector<Candle> rows_;

        // Cached SoA columns (aligned by index)
        std::vector<double> opens_;
        std::vector<double> highs_;
        std::vector<double> lows_;
        std::vector<double> closes_;
        std::vector<double> volumes_;
        std::vector<TimeKey> dates_;
    };

} // namespace sugar
