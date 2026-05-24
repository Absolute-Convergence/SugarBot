#include "commands/app_commands.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ingest/raw_table.h"
#include "ingest/utils.h"

namespace {

    std::string trim_copy(std::string s) {
        auto not_ws = [](unsigned char c) { return !std::isspace(c); };
        auto b = std::find_if(s.begin(), s.end(), not_ws);
        auto e = std::find_if(s.rbegin(), s.rend(), not_ws).base();
        if (b >= e) return {};
        return std::string(b, e);
    }

    std::string lower_copy(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    std::string normalized_header_token(std::string s) {
        s = lower_copy(trim_copy(std::move(s)));
        std::string out;
        out.reserve(s.size());
        for (unsigned char c : s) {
            if (std::isalnum(c)) {
                out.push_back(static_cast<char>(c));
            }
        }
        return out;
    }

    bool parse_double_strict(std::string s) {
        s = trim_copy(std::move(s));
        if (s.empty()) return false;
        try {
            std::size_t idx = 0;
            (void)std::stod(s, &idx);
            while (idx < s.size() && std::isspace(static_cast<unsigned char>(s[idx]))) {
                ++idx;
            }
            return idx == s.size();
        }
        catch (...) {
            return false;
        }
    }

    bool scaffold_missing_token(std::string token) {
        token = lower_copy(trim_copy(std::move(token)));
        return token.empty() || token == "na" || token == "nan" ||
            token == "null" || token == "none";
    }

    bool is_all_digits(std::string_view s) {
        if (s.empty()) return false;
        for (unsigned char c : s) {
            if (!std::isdigit(c)) return false;
        }
        return true;
    }

    bool sample_matches_dash_date(std::string_view s) {
        return s.size() == 10 &&
            std::isdigit(static_cast<unsigned char>(s[0])) &&
            std::isdigit(static_cast<unsigned char>(s[1])) &&
            std::isdigit(static_cast<unsigned char>(s[2])) &&
            std::isdigit(static_cast<unsigned char>(s[3])) &&
            s[4] == '-' &&
            std::isdigit(static_cast<unsigned char>(s[5])) &&
            std::isdigit(static_cast<unsigned char>(s[6])) &&
            s[7] == '-' &&
            std::isdigit(static_cast<unsigned char>(s[8])) &&
            std::isdigit(static_cast<unsigned char>(s[9]));
    }

    bool sample_matches_slash_date(std::string_view s) {
        return s.size() == 10 &&
            std::isdigit(static_cast<unsigned char>(s[0])) &&
            std::isdigit(static_cast<unsigned char>(s[1])) &&
            std::isdigit(static_cast<unsigned char>(s[2])) &&
            std::isdigit(static_cast<unsigned char>(s[3])) &&
            s[4] == '/' &&
            std::isdigit(static_cast<unsigned char>(s[5])) &&
            std::isdigit(static_cast<unsigned char>(s[6])) &&
            s[7] == '/' &&
            std::isdigit(static_cast<unsigned char>(s[8])) &&
            std::isdigit(static_cast<unsigned char>(s[9]));
    }

    bool sample_matches_year_month(std::string_view s) {
        return (s.size() == 7 &&
                std::isdigit(static_cast<unsigned char>(s[0])) &&
                std::isdigit(static_cast<unsigned char>(s[1])) &&
                std::isdigit(static_cast<unsigned char>(s[2])) &&
                std::isdigit(static_cast<unsigned char>(s[3])) &&
                (s[4] == '-' || s[4] == '/') &&
                std::isdigit(static_cast<unsigned char>(s[5])) &&
                std::isdigit(static_cast<unsigned char>(s[6]))) ||
            (s.size() == 6 && is_all_digits(s));
    }

    bool sample_matches_year_quarter(std::string_view s) {
        if (s.size() == 6 &&
            std::isdigit(static_cast<unsigned char>(s[0])) &&
            std::isdigit(static_cast<unsigned char>(s[1])) &&
            std::isdigit(static_cast<unsigned char>(s[2])) &&
            std::isdigit(static_cast<unsigned char>(s[3])) &&
            (s[4] == 'Q' || s[4] == 'q') &&
            s[5] >= '1' && s[5] <= '4') {
            return true;
        }
        return s.size() == 7 &&
            std::isdigit(static_cast<unsigned char>(s[0])) &&
            std::isdigit(static_cast<unsigned char>(s[1])) &&
            std::isdigit(static_cast<unsigned char>(s[2])) &&
            std::isdigit(static_cast<unsigned char>(s[3])) &&
            s[4] == '-' &&
            (s[5] == 'Q' || s[5] == 'q') &&
            s[6] >= '1' && s[6] <= '4';
    }

    std::optional<std::string> first_non_missing_cell(
        const sugar::RawTable& table,
        std::size_t column_index) {
        for (const auto& row : table.rows) {
            if (column_index >= row.size()) continue;
            std::string cell = trim_copy(row[column_index]);
            if (!scaffold_missing_token(cell)) {
                return cell;
            }
        }
        return std::nullopt;
    }

    std::string infer_scaffold_time_kind(const std::optional<std::string>& sample) {
        if (!sample.has_value()) return "YYYY-MM-DD";
        const std::string value = trim_copy(*sample);
        if (value.find('T') != std::string::npos || value.find(':') != std::string::npos) {
            return "ISO-8601";
        }
        if (sample_matches_dash_date(value)) return "YYYY-MM-DD";
        if (sample_matches_slash_date(value)) return "YYYY-MM-DD";
        if (value.size() == 8 && is_all_digits(value)) return "YYYYMMDD";
        if (sample_matches_year_quarter(value)) return "YearQuarter";
        if (sample_matches_year_month(value)) return "YearMonth";
        if (is_all_digits(value)) return "Index";
        return "YYYY-MM-DD";
    }

    int scaffold_time_header_score(const std::string& normalized) {
        if (normalized == "date") return 100;
        if (normalized == "time" || normalized == "timestamp" || normalized == "datetime") return 95;
        if (normalized == "day" || normalized == "tradedate") return 90;
        if (normalized.find("date") != std::string::npos) return 70;
        if (normalized.find("time") != std::string::npos) return 65;
        return 0;
    }

    bool scaffold_is_entity_header(const std::string& normalized) {
        return normalized == "symbol" ||
            normalized == "ticker" ||
            normalized == "entity" ||
            normalized == "asset" ||
            normalized == "series" ||
            normalized == "region";
    }

    bool scaffold_is_numeric_column(
        const sugar::RawTable& table,
        std::size_t column_index) {
        std::size_t numeric_count = 0;
        for (const auto& row : table.rows) {
            if (column_index >= row.size()) continue;
            const std::string cell = trim_copy(row[column_index]);
            if (scaffold_missing_token(cell)) continue;
            if (!parse_double_strict(cell)) return false;
            ++numeric_count;
        }
        return numeric_count > 0;
    }

    std::optional<std::size_t> find_header_index_by_normalized(
        const std::vector<std::string>& headers,
        const char* normalized_name) {
        for (std::size_t i = 0; i < headers.size(); ++i) {
            if (normalized_header_token(headers[i]) == normalized_name) {
                return i;
            }
        }
        return std::nullopt;
    }

    void append_json_string(std::ostream& out, std::string_view value) {
        out << '"';
        for (const unsigned char ch : value) {
            switch (ch) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (ch < 0x20) {
                    static constexpr char hex[] = "0123456789abcdef";
                    out << "\\u00" << hex[(ch >> 4) & 0x0f] << hex[ch & 0x0f];
                }
                else {
                    out << static_cast<char>(ch);
                }
                break;
            }
        }
        out << '"';
    }

} // namespace

namespace sugar::app {

    int run_scaffold_layout_command(const AppCommand& cmd, std::ostream& out, std::ostream& err) {
        bool had_header = false;
        std::string load_err;
        sugar::CsvLoadDiagnostics diagnostics{};
        sugar::RawTable table = sugar::load_csv_table(
            cmd.scaffold_layout.csv_path,
            sugar::CsvHeaderPolicy::AutoDetect,
            had_header,
            load_err,
            &diagnostics);
        if (!load_err.empty()) {
            err << "Error: " << load_err << "\n";
            return 1;
        }
        if (!had_header || table.headers.empty()) {
            err << "Error: --scaffold expects a CSV header row so it can name layout fields from real columns.\n";
            return 1;
        }

        std::size_t time_index = table.headers.size();
        int best_time_score = 0;
        for (std::size_t i = 0; i < table.headers.size(); ++i) {
            const int score = scaffold_time_header_score(normalized_header_token(table.headers[i]));
            if (score > best_time_score) {
                best_time_score = score;
                time_index = i;
            }
        }

        std::optional<std::size_t> entity_index;
        for (std::size_t i = 0; i < table.headers.size(); ++i) {
            if (i == time_index) continue;
            if (scaffold_is_entity_header(normalized_header_token(table.headers[i])) &&
                !scaffold_is_numeric_column(table, i)) {
                entity_index = i;
                break;
            }
        }

        std::optional<std::size_t> spatial_x_index = find_header_index_by_normalized(table.headers, "x");
        std::optional<std::size_t> spatial_y_index = find_header_index_by_normalized(table.headers, "y");
        const bool use_spatial_axis = time_index >= table.headers.size() &&
            spatial_x_index.has_value() &&
            spatial_y_index.has_value();

        if (time_index >= table.headers.size() && !use_spatial_axis) {
            err << "Error: --scaffold could not find a time-like column such as date, time, timestamp, or datetime.\n";
            return 1;
        }

        std::vector<std::string> value_cols;
        for (std::size_t i = 0; i < table.headers.size(); ++i) {
            if (i == time_index) continue;
            if (entity_index.has_value() && i == *entity_index) continue;
            if (use_spatial_axis &&
                ((spatial_x_index.has_value() && i == *spatial_x_index) ||
                    (spatial_y_index.has_value() && i == *spatial_y_index))) {
                continue;
            }
            if (scaffold_is_numeric_column(table, i)) {
                value_cols.push_back(trim_copy(table.headers[i]));
            }
        }

        if (value_cols.empty()) {
            err << "Error: --scaffold could not infer numeric value columns from the CSV sample rows.\n";
            return 1;
        }

        std::string primary_value_col = value_cols.front();
        for (const auto& col : value_cols) {
            const std::string norm = normalized_header_token(col);
            if (norm == "close" || norm == "adjclose" || norm == "value" || norm == "price") {
                primary_value_col = col;
                break;
            }
        }

        std::optional<std::string> scaffold_time_kind;
        if (time_index < table.headers.size()) {
            scaffold_time_kind = infer_scaffold_time_kind(first_non_missing_cell(table, time_index));
        }

        err << "Scaffolded an explicit LayoutSpec from CSV headers. Review the field names, then validate with `sugarbot --obs-report <layout.json> "
            << cmd.scaffold_layout.csv_path << "`.\n";
        err << "Scaffold assumptions:\n";
        if (time_index < table.headers.size()) {
            err << "  time_col: " << trim_copy(table.headers[time_index]) << "\n"
                << "  time_kind: " << *scaffold_time_kind << "\n";
        }
        else {
            err << "  time_col: <none>\n";
        }
        if (use_spatial_axis) {
            err << "  spatial_x_col: " << trim_copy(table.headers[*spatial_x_index]) << "\n"
                << "  spatial_y_col: " << trim_copy(table.headers[*spatial_y_index]) << "\n";
        }
        err << "  entity_col: ";
        if (entity_index.has_value()) {
            err << trim_copy(table.headers[*entity_index]);
        }
        else {
            err << "<none>";
        }
        err << "\n"
            << "  value_cols: ";
        for (std::size_t i = 0; i < value_cols.size(); ++i) {
            if (i) err << ", ";
            err << value_cols[i];
        }
        err << "\n";
        if (value_cols.size() > 1) {
            err << "  primary_value_col: " << primary_value_col << "\n";
        }
        else {
            err << "  primary_value_col: <omitted; inferred from the single value column>\n";
        }
        err << "No legacy preset or compatibility runtime was used.\n";

        out << "{\n"
            << "  \"ingest\": {\n";
        if (time_index < table.headers.size()) {
            out << "    \"time_col\": ";
            append_json_string(out, trim_copy(table.headers[time_index]));
            out << ",\n"
                << "    \"time_kind\": ";
            append_json_string(out, *scaffold_time_kind);
            out << ",\n";
        }
        if (use_spatial_axis) {
            out << "    \"spatial_x_col\": ";
            append_json_string(out, trim_copy(table.headers[*spatial_x_index]));
            out << ",\n"
                << "    \"spatial_y_col\": ";
            append_json_string(out, trim_copy(table.headers[*spatial_y_index]));
            out << ",\n";
        }
        if (entity_index.has_value()) {
            out << "    \"entity_col\": ";
            append_json_string(out, trim_copy(table.headers[*entity_index]));
            out << ",\n";
        }
        out << "    \"value_cols\": [";
        for (std::size_t i = 0; i < value_cols.size(); ++i) {
            if (i) out << ", ";
            append_json_string(out, value_cols[i]);
        }
        out << "]";
        if (value_cols.size() > 1) {
            out << ",\n"
                << "    \"primary_value_col\": ";
            append_json_string(out, primary_value_col);
        }
        out << "\n"
            << "  },\n"
            << "  \"topologies\": []\n"
            << "}\n";
        return 0;
    }

} // namespace sugar::app
