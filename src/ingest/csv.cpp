#include "ingest/csv.h"
#include "ingest/utils.h"
#include <fstream>
#include <stdexcept>
#include <algorithm>
#include <string>
#include <string_view>
#include <cctype>

namespace sugar {

    namespace {
        bool parse_required_double(std::string_view sv, double& out) {
            try {
                std::size_t idx = 0;
                const std::string s(sv);
                out = std::stod(s, &idx);
                for (std::size_t i = idx; i < s.size(); ++i) {
                    if (!std::isspace(static_cast<unsigned char>(s[i]))) return false;
                }
                return true;
            }
            catch (...) {
                return false;
            }
        }
    }

    // Minimal CSV splitter:
        // Handles double quotes to allow commas inside quoted fields.
        // Does not handle escaped quotes inside quoted fields
    std::vector<std::string> split_csv_line(std::string_view sv) {
        std::vector<std::string> out;                                                                           // vector of string objects to be returned later 
        std::string cur;                                                                                        // temp string to read and store characters into a string
        bool in_quotes = false;                                                                                 // bool condition 'in_quotes' for the loop
        for (char c : sv) {                                                                                     // iterate over every element in sv and store that element in char c                                                       
            if (c == '"') { in_quotes = !in_quotes; continue; }                                                 // read c, if sv has reached a '"' symbol, toggle in_quotes, skip the quote
            if (c == ',' && !in_quotes) {                                                                       // if a comma is reached, and not in_quotes
                    out.push_back(cur);                                                                         // store cur(string) in last element of out
                    cur.clear(); }                                                                              // clear the string cur
            else { cur.push_back(c); }                                                                          // add each element read into c, into the string cur 
        }
        out.push_back(cur);                                                                                     // store the final string (possibly empty) read from sv into cur through c into the final element of out
        return out;                                                                                             // return the vector of strings out
    }

    // Tiny heuristic: if the first column name contains "date", treat first row as header.
    static bool maybe_header(const std::vector<std::string>& cols) {                                            // accepts a vector of strings, and checks to see if it fits a header pattern
        if (cols.empty()) return true;                                                                          // first line check, if the columns vector is empty, skip it and exit the function
        std::string first = cols[0];                                                                            // string, initialized with the first string in function parameter
        std::transform(first.begin(), first.end(), first.begin(),                                               // STL transform function, iterates over 'first' from .begin to .end, destination start at .begin 
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });                              // lambda variable 'unsigned char ch', lambda function: return tolower(ch) for every char in string variable first
        return first.find("date") != std::string::npos || first.find("time") != std::string::npos;              // once every character in the header string is lowercase, search for 'date' or 'time' return a position/index to that location, or npos
    }

    // Read CSV file into a vector<Candle>.
    // Required columns by index: 0=Date, 1=Open, 2=High, 3=Low, 4=Close, 5=Volume (optional)
    std::vector<Candle> load_candles_csv(const std::string& path, CandleCsvDiagnostics* diagnostics) {         // the actual candle loader function, accepts a compatable string object argument, passed by const reference
                                                                                                                // Two-step open avoids MSVC "vexing parse" and name-collision weirdness.
        std::ifstream ifs;                                                                                      // in file stream object, to be used later for reading the csv
        ifs.open(path, std::ios::in);                                                                           // opens file stream for reading path
        if (!ifs.is_open()) {                                                                                   // test with implicit verification, if fail, exit early with error code
            throw std::runtime_error("Failed to open CSV: " + path);                                            // exemption, the error code, and feedback on the invalid path, exit the program 
        }

        if (diagnostics) {
            *diagnostics = CandleCsvDiagnostics{};
        }

        std::vector<Candle> rows;                                                                               // vector of candle objects called 'rows' (to be returned later)
        std::string line;                                                                                       // temp string called 'line'
        bool first_line = true;                                                                                 // bool variable to used with the 'maybe_header' function later

        while (std::getline(ifs, line)) {                                                                       // create a while loop dependant on the implicit pass/fail state of the getline() function, pass each line from ifs into line.
            if (diagnostics) {
                ++diagnostics->physical_line_count;
            }
            if (line.empty()) {
                if (diagnostics) {
                    ++diagnostics->blank_line_count;
                }
                continue;
            }                                                                                                   // if line is empty, skip this loop and restart it
            if (!line.empty() && line.back() == '\r') line.pop_back();                                          // strip a trailing \r if present (CRLF files)

            auto cols = split_csv_line(line);                                                                   // a vector of strings, populated with each element of a line from the csv file
            if (first_line) {                                                                                   // test if it is the first line of the csv file
                first_line = false;                                                                             // set to false after first case of true
                if (maybe_header(cols)) {
                    if (diagnostics) {
                        diagnostics->had_header = true;
                    }
                    continue;
                }                                                               // test if first line is a header, if true, skip the rest of the loop
            }
            if (diagnostics) {
                ++diagnostics->data_row_count;
            }
            if (cols.size() < 5) {
                if (diagnostics) {
                    ++diagnostics->skipped_short_row_count;
                }
                continue;
            }                                                                      // not enough fields → skip

            Candle c{};                                                                                         // candle object
            {
                // parse_yyyymmdd now returns std::optional<int>
                const auto d = parse_yyyymmdd(cols[0]);                                                         // acceptable formats: YYYYMMDD, YYYY-MM-DD, YYYY/MM/DD, MM/DD/YYYY
                if (!d) {
                    if (diagnostics) {
                        ++diagnostics->skipped_bad_date_row_count;
                    }
                    continue;
                }                                                                               // skip row if date cannot be parsed
                c.date = static_cast<decltype(c.date)>(*d);
            }
            if (!parse_required_double(cols[1], c.open)) {
                if (diagnostics) {
                    ++diagnostics->skipped_bad_numeric_row_count;
                }
                continue;
            }                                              // store open
            if (!parse_required_double(cols[2], c.high)) {
                if (diagnostics) {
                    ++diagnostics->skipped_bad_numeric_row_count;
                }
                continue;
            }                                              // store high
            if (!parse_required_double(cols[3], c.low)) {
                if (diagnostics) {
                    ++diagnostics->skipped_bad_numeric_row_count;
                }
                continue;
            }                                               // store low
            if (!parse_required_double(cols[4], c.close)) {
                if (diagnostics) {
                    ++diagnostics->skipped_bad_numeric_row_count;
                }
                continue;
            }                                             // store close
            if (cols.size() >= 6 && !cols[5].empty()) {                                                         // if the column count is ≥ 6, parse cols[5], else set 0.0
                try {
                    c.volume = std::stod(cols[5]);
                }
                catch (...) {
                    c.volume = 0.0;
                }
            }
            else {
                c.volume = 0.0;
            }

            rows.push_back(c);                                                                                  // store each candle object in rows
            if (diagnostics) {
                ++diagnostics->kept_row_count;
            }
        }
        return rows;                                                                                            // return vector of candles in rows
    }

} // namespace sugar
