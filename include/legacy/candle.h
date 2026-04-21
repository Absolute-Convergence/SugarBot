#pragma once
#include "ingest/time_key.h"

namespace sugar {

                                                                    // Candle is the atomic data row indicators/strategies consume.
                                                                    // Keeping date as an int (YYYYMMDD) avoids time libs.
    struct Candle {
        TimeKey date;                                               // e.g. 20250101
        double open{};
        double high{};
        double low{};
        double close{};
        double volume{};
    };

} // namespace sugar
