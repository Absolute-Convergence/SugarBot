#pragma once

#include <cstddef>
#include <iosfwd>
#include <string>
#include <vector>

namespace sugar {

    enum class CsvHeaderPolicy {
        AutoDetect,
        Required,
        None,
    };

    struct CsvLoadDiagnostics {
        std::size_t physical_line_count{ 0 };
        std::size_t blank_line_count{ 0 };
        std::size_t data_row_count{ 0 };
    };

    struct RawTable {
        std::vector<std::string> headers;
        std::vector<std::vector<std::string>> rows;
    };

    // Loads CSV rows as raw strings. No numeric/date coercion is performed.
    // Returns an empty table on error and sets `err`.
    RawTable load_csv_table(
        const std::string& path,
        CsvHeaderPolicy policy,
        bool& had_header,
        std::string& err,
        CsvLoadDiagnostics* diagnostics = nullptr);

    // Debug helper: prints headers and first `max_rows` data rows.
    void print_raw_table_preview(const RawTable& t, std::ostream& out, std::size_t max_rows = 3);

} // namespace sugar
