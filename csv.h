#pragma once
#include <string>
#include <vector>
#include "candle.h"

namespace sugar {

                                                                            // Public API: load candles from a CSV file path.
                                                                            // use const std::string& (not string_view) to keep file-open simple
                                                                            // avoid vexing parse/macros in MSVC for now.
    std::vector<Candle> load_candles_csv(const std::string& path);

                                                                            // Small helper exposed for convenience/testing:
                                                                            // split a CSV line into fields (double-quote aware, minimalistic).
    std::vector<std::string> split_csv_line(std::string_view line);

} // namespace sugar
