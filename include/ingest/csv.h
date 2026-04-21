#pragma once
#include <string>
#include <vector>
#include "legacy/candle.h"

namespace sugar {

    struct CandleCsvDiagnostics {
        std::size_t physical_line_count{ 0 };
        std::size_t blank_line_count{ 0 };
        std::size_t data_row_count{ 0 };
        std::size_t kept_row_count{ 0 };
        std::size_t skipped_short_row_count{ 0 };
        std::size_t skipped_bad_date_row_count{ 0 };
        std::size_t skipped_bad_numeric_row_count{ 0 };
        bool had_header{ false };

        std::size_t skipped_row_count() const {
            return skipped_short_row_count +
                skipped_bad_date_row_count +
                skipped_bad_numeric_row_count;
        }
    };

                                                                            // Public API: load candles from a CSV file path.
                                                                            // use const std::string& (not string_view) to keep file-open simple
                                                                            // avoid vexing parse/macros in MSVC for now.
    std::vector<Candle> load_candles_csv(
        const std::string& path,
        CandleCsvDiagnostics* diagnostics = nullptr);

                                                                            // Small helper exposed for convenience/testing:
                                                                            // split a CSV line into fields (double-quote aware, minimalistic).
    std::vector<std::string> split_csv_line(std::string_view line);

} // namespace sugar
