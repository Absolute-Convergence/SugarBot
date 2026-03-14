#include "series.h"

namespace sugar {

    CandleSeries::CandleSeries(std::vector<Candle> rows)
        : rows_(std::move(rows))
    {
        const std::size_t n = rows_.size();

        opens_.resize(n);
        highs_.resize(n);
        lows_.resize(n);
        closes_.resize(n);
        volumes_.resize(n);
        dates_.resize(n);

        for (std::size_t i = 0; i < n; ++i) {
            const Candle& c = rows_[i];
            opens_[i] = c.open;
            highs_[i] = c.high;
            lows_[i] = c.low;
            closes_[i] = c.close;
            volumes_[i] = c.volume;  // 0.0 is valid if CSV had no volume
            dates_[i] = c.date;
        }
    }

} // namespace sugar
