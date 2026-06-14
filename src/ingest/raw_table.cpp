#include "ingest/raw_table.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>

#include "ingest/csv.h" // split_csv_line

#if defined(SUGAR_ENABLE_PARQUET)
#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/reader.h>
#endif

namespace sugar {

    namespace {

        std::string trim_copy(std::string s) {
            auto not_ws = [](unsigned char c) { return !std::isspace(c); };
            auto b = std::find_if(s.begin(), s.end(), not_ws);
            auto e = std::find_if(s.rbegin(), s.rend(), not_ws).base();
            if (b >= e) return {};
            return std::string(b, e);
        }

        bool token_has_alpha(std::string_view sv) {
            for (unsigned char c : sv) {
                if (std::isalpha(c)) return true;
            }
            return false;
        }

        bool maybe_header_generalized(const std::vector<std::string>& cols) {
            if (cols.empty()) return true;

            std::string first = cols.front();
            std::transform(first.begin(), first.end(), first.begin(),
                [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            if (first.find("date") != std::string::npos || first.find("time") != std::string::npos) {
                return true;
            }

            for (const auto& c : cols) {
                if (token_has_alpha(c)) return true;
            }
            return false;
        }

#if defined(SUGAR_ENABLE_PARQUET)
        std::string format_double(double value) {
            std::ostringstream out;
            out << std::setprecision(17) << value;
            return out.str();
        }

        std::string normalize_arrow_time_text(std::string text) {
            if (text.size() >= 19 && text[4] == '-' && text[7] == '-' && text[10] == ' ') {
                text[10] = 'T';
            }
            if (text.size() > 19 && text[4] == '-' && text[7] == '-' && text[10] == 'T') {
                const char next = text[19];
                if (next == '.' || next == '+' || next == '-') {
                    text.resize(19);
                }
            }
            return text;
        }

        bool parquet_column_type_supported(const arrow::DataType& type) {
            switch (type.id()) {
            case arrow::Type::BOOL:
            case arrow::Type::INT8:
            case arrow::Type::INT16:
            case arrow::Type::INT32:
            case arrow::Type::INT64:
            case arrow::Type::UINT8:
            case arrow::Type::UINT16:
            case arrow::Type::UINT32:
            case arrow::Type::UINT64:
            case arrow::Type::FLOAT:
            case arrow::Type::DOUBLE:
            case arrow::Type::STRING:
            case arrow::Type::LARGE_STRING:
            case arrow::Type::DATE32:
            case arrow::Type::DATE64:
            case arrow::Type::TIMESTAMP:
                return true;
            default:
                return false;
            }
        }

        std::string unsupported_parquet_column_message(
            const arrow::DataType& type,
            const std::string& column_name) {
            return "unsupported Parquet column type '" + type.ToString() +
                "' for column '" + column_name +
                "'; supported types are string, integer, floating, bool, date, and timestamp";
        }

        bool scalar_to_cell(
            const std::shared_ptr<arrow::Scalar>& scalar,
            const std::string& column_name,
            std::string& out,
            std::string& err) {
            out.clear();
            if (!scalar || !scalar->is_valid) {
                return true;
            }

            switch (scalar->type->id()) {
            case arrow::Type::BOOL:
                out = static_cast<const arrow::BooleanScalar&>(*scalar).value ? "1" : "0";
                return true;
            case arrow::Type::INT8:
                out = std::to_string(static_cast<const arrow::Int8Scalar&>(*scalar).value);
                return true;
            case arrow::Type::INT16:
                out = std::to_string(static_cast<const arrow::Int16Scalar&>(*scalar).value);
                return true;
            case arrow::Type::INT32:
                out = std::to_string(static_cast<const arrow::Int32Scalar&>(*scalar).value);
                return true;
            case arrow::Type::INT64:
                out = std::to_string(static_cast<const arrow::Int64Scalar&>(*scalar).value);
                return true;
            case arrow::Type::UINT8:
                out = std::to_string(static_cast<const arrow::UInt8Scalar&>(*scalar).value);
                return true;
            case arrow::Type::UINT16:
                out = std::to_string(static_cast<const arrow::UInt16Scalar&>(*scalar).value);
                return true;
            case arrow::Type::UINT32:
                out = std::to_string(static_cast<const arrow::UInt32Scalar&>(*scalar).value);
                return true;
            case arrow::Type::UINT64:
                out = std::to_string(static_cast<const arrow::UInt64Scalar&>(*scalar).value);
                return true;
            case arrow::Type::FLOAT:
                out = format_double(static_cast<const arrow::FloatScalar&>(*scalar).value);
                return true;
            case arrow::Type::DOUBLE:
                out = format_double(static_cast<const arrow::DoubleScalar&>(*scalar).value);
                return true;
            case arrow::Type::STRING:
                out = static_cast<const arrow::StringScalar&>(*scalar).value->ToString();
                return true;
            case arrow::Type::LARGE_STRING:
                out = static_cast<const arrow::LargeStringScalar&>(*scalar).value->ToString();
                return true;
            case arrow::Type::DATE32:
            case arrow::Type::DATE64:
            case arrow::Type::TIMESTAMP:
                out = normalize_arrow_time_text(scalar->ToString());
                return true;
            default:
                err = unsupported_parquet_column_message(*scalar->type, column_name);
                return false;
            }
        }

        RawTable load_parquet_table(
            const std::string& path,
            bool& had_header,
            std::string& err,
            TabularLoadDiagnostics* diagnostics) {
            had_header = true;
            err.clear();

            auto input_result = arrow::io::ReadableFile::Open(path);
            if (!input_result.ok()) {
                err = "Failed to open Parquet: " + path + ": " + input_result.status().ToString();
                return {};
            }

            auto reader_result = parquet::arrow::OpenFile(*input_result, arrow::default_memory_pool());
            if (!reader_result.ok()) {
                err = "Failed to open Parquet reader: " + reader_result.status().ToString();
                return {};
            }

            auto table_result = (*reader_result)->ReadTable();
            if (!table_result.ok()) {
                err = "Failed to read Parquet table: " + table_result.status().ToString();
                return {};
            }

            const std::shared_ptr<arrow::Table> table = *table_result;
            RawTable out{};
            out.headers.reserve(static_cast<std::size_t>(table->num_columns()));
            for (int i = 0; i < table->num_columns(); ++i) {
                const auto field = table->field(i);
                if (!field || !field->type()) {
                    err = "Parquet column at index " + std::to_string(i) + " is missing an Arrow type";
                    return {};
                }
                if (!parquet_column_type_supported(*field->type())) {
                    err = unsupported_parquet_column_message(*field->type(), field->name());
                    return {};
                }
                out.headers.push_back(field->name());
            }

            const std::int64_t row_count = table->num_rows();
            if (row_count < 0) {
                err = "Parquet table reported a negative row count";
                return {};
            }

            out.rows.reserve(static_cast<std::size_t>(row_count));
            for (std::int64_t row = 0; row < row_count; ++row) {
                std::vector<std::string> cells;
                cells.reserve(static_cast<std::size_t>(table->num_columns()));
                for (int col = 0; col < table->num_columns(); ++col) {
                    auto scalar_result = table->column(col)->GetScalar(row);
                    if (!scalar_result.ok()) {
                        err = "Failed to read Parquet scalar at row " +
                            std::to_string(row + 1) + ", column '" +
                            table->field(col)->name() + "': " +
                            scalar_result.status().ToString();
                        return {};
                    }

                    std::string cell;
                    if (!scalar_to_cell(*scalar_result, table->field(col)->name(), cell, err)) {
                        return {};
                    }
                    cells.push_back(std::move(cell));
                }
                out.rows.push_back(std::move(cells));
            }

            if (diagnostics) {
                diagnostics->resolved_format = TabularFileFormat::Parquet;
                diagnostics->had_header = true;
                diagnostics->data_row_count = static_cast<std::size_t>(row_count);
            }
            return out;
        }
#else
        RawTable load_parquet_table(
            const std::string& path,
            bool& had_header,
            std::string& err,
            TabularLoadDiagnostics*) {
            had_header = false;
            err = "Parquet input requires a build configured with SUGAR_ENABLE_PARQUET=ON: " + path;
            return {};
        }
#endif

    } // namespace

    std::string_view tabular_file_format_name(TabularFileFormat format) {
        switch (format) {
        case TabularFileFormat::Auto:
            return "auto";
        case TabularFileFormat::Csv:
            return "csv";
        case TabularFileFormat::Parquet:
            return "parquet";
        }
        return "unknown";
    }

    bool parse_tabular_file_format(std::string_view text, TabularFileFormat& out) {
        std::string normalized(text);
        std::transform(normalized.begin(), normalized.end(), normalized.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (normalized == "auto") {
            out = TabularFileFormat::Auto;
            return true;
        }
        if (normalized == "csv") {
            out = TabularFileFormat::Csv;
            return true;
        }
        if (normalized == "parquet" || normalized == "pq") {
            out = TabularFileFormat::Parquet;
            return true;
        }
        return false;
    }

    TabularFileFormat infer_tabular_file_format_from_path(const std::string& path) {
        std::string ext = std::filesystem::path(path).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (ext == ".parquet" || ext == ".pq") {
            return TabularFileFormat::Parquet;
        }
        return TabularFileFormat::Csv;
    }

    RawTable load_csv_table(
        const std::string& path,
        CsvHeaderPolicy policy,
        bool& had_header,
        std::string& err,
        CsvLoadDiagnostics* diagnostics) {
        had_header = false;
        err.clear();
        if (diagnostics) {
            *diagnostics = CsvLoadDiagnostics{};
        }

        std::ifstream ifs(path);
        if (!ifs.is_open()) {
            err = "Failed to open CSV: " + path;
            return {};
        }

        RawTable out{};
        std::string line;
        bool first_line = true;

        while (std::getline(ifs, line)) {
            if (diagnostics) {
                ++diagnostics->physical_line_count;
            }
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (trim_copy(line).empty()) {
                if (diagnostics) {
                    ++diagnostics->blank_line_count;
                }
                continue;
            }

            auto cols = split_csv_line(line);

            if (first_line) {
                first_line = false;
                const bool treat_as_header =
                    (policy == CsvHeaderPolicy::Required) ||
                    (policy == CsvHeaderPolicy::AutoDetect && maybe_header_generalized(cols));
                if (treat_as_header) {
                    out.headers = std::move(cols);
                    had_header = true;
                    continue;
                }
            }

            out.rows.push_back(std::move(cols));
            if (diagnostics) {
                ++diagnostics->data_row_count;
            }
        }

        return out;
    }

    RawTable load_tabular_table(
        const std::string& path,
        TabularFileFormat format,
        CsvHeaderPolicy policy,
        bool& had_header,
        std::string& err,
        TabularLoadDiagnostics* diagnostics) {
        if (diagnostics) {
            *diagnostics = TabularLoadDiagnostics{};
            diagnostics->requested_format = format;
        }

        const TabularFileFormat resolved =
            format == TabularFileFormat::Auto
                ? infer_tabular_file_format_from_path(path)
                : format;
        if (diagnostics) {
            diagnostics->resolved_format = resolved;
        }

        if (resolved == TabularFileFormat::Parquet) {
            return load_parquet_table(path, had_header, err, diagnostics);
        }

        CsvLoadDiagnostics csv_diag{};
        RawTable out = load_csv_table(path, policy, had_header, err, &csv_diag);
        if (diagnostics) {
            diagnostics->resolved_format = TabularFileFormat::Csv;
            diagnostics->physical_line_count = csv_diag.physical_line_count;
            diagnostics->blank_line_count = csv_diag.blank_line_count;
            diagnostics->data_row_count = csv_diag.data_row_count;
            diagnostics->had_header = had_header;
        }
        return out;
    }

    void print_raw_table_preview(const RawTable& t, std::ostream& out, std::size_t max_rows) {
        out << "RawTable preview\n";
        out << "  headers(" << t.headers.size() << "): ";
        if (t.headers.empty()) {
            out << "(none)";
        }
        else {
            for (std::size_t i = 0; i < t.headers.size(); ++i) {
                if (i) out << " | ";
                out << t.headers[i];
            }
        }
        out << "\n";

        const std::size_t n = std::min(max_rows, t.rows.size());
        out << "  rows shown: " << n << " / " << t.rows.size() << "\n";
        for (std::size_t r = 0; r < n; ++r) {
            out << "  [" << std::setw(2) << r << "] ";
            for (std::size_t c = 0; c < t.rows[r].size(); ++c) {
                if (c) out << " | ";
                out << t.rows[r][c];
            }
            out << "\n";
        }
    }

} // namespace sugar
