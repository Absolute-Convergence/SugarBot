#pragma once

#include <cstddef>
#include <iosfwd>
#include <string>
#include <string_view>
#include <vector>

namespace sugar {

    enum class TabularFileFormat {
        Auto,
        Csv,
        Parquet,
    };

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

    struct TabularLoadDiagnostics {
        TabularFileFormat requested_format{ TabularFileFormat::Auto };
        TabularFileFormat resolved_format{ TabularFileFormat::Csv };
        std::size_t physical_line_count{ 0 };
        std::size_t blank_line_count{ 0 };
        std::size_t data_row_count{ 0 };
        bool had_header{ false };
    };

    struct RawTable {
        std::vector<std::string> headers;
        std::vector<std::vector<std::string>> rows;
    };

    std::string_view tabular_file_format_name(TabularFileFormat format);
    bool parse_tabular_file_format(std::string_view text, TabularFileFormat& out);
    TabularFileFormat infer_tabular_file_format_from_path(const std::string& path);

    // Loads CSV rows as raw strings. No numeric/date coercion is performed.
    // Returns an empty table on error and sets `err`.
    RawTable load_csv_table(
        const std::string& path,
        CsvHeaderPolicy policy,
        bool& had_header,
        std::string& err,
        CsvLoadDiagnostics* diagnostics = nullptr);

    // Loads a supported tabular file as raw string cells for the existing
    // DataSpec preprocessing boundary. CSV remains the default.
    RawTable load_tabular_table(
        const std::string& path,
        TabularFileFormat format,
        CsvHeaderPolicy policy,
        bool& had_header,
        std::string& err,
        TabularLoadDiagnostics* diagnostics = nullptr);

    // Debug helper: prints headers and first `max_rows` data rows.
    void print_raw_table_preview(const RawTable& t, std::ostream& out, std::size_t max_rows = 3);

} // namespace sugar
