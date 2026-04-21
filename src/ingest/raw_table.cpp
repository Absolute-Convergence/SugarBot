#include "ingest/raw_table.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <ostream>
#include <string>
#include <string_view>

#include "ingest/csv.h" // split_csv_line

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

    } // namespace

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
