#pragma once
#include <iostream>
#include <string>
#include <sstream>
#include <string_view>
#include <cstdint>
#include <vector>
#include <cstddef>
#include <limits>
#include <iosfwd>
#include <optional>


namespace sugar {

    // ------------------------------------------------------------
    // Date / time helpers
    // ------------------------------------------------------------

    // Parse YYYYMMDD or YYYY-MM-DD (and a few common variants) into int YYYYMMDD.
    std::optional<int> parse_yyyymmdd(std::string_view s);

    // Format YYYYMMDD -> "YYYYMMDD" (8 chars) into a fixed 9-byte buffer.
    // (We include the 9th byte for convenience; caller may ignore the null.)
    void format_yyyymmdd(int yyyymmdd, char out[9]);

    // Parse ISO-8601 datetime strings like:
    //   2024-10-09T01:00:00-06:00
    //   2025-09-12T17:30:00-06:00
    //   2025-10-03T11:09:15-06:00
    // Seconds optional; timezone can be 'Z' or +/-HH:MM.
    // On success returns true and fills:
    //   yyyymmdd          -> date as int YYYYMMDD
    //   seconds_since_mid -> seconds since local clock time midnight in the string
    //   tz_offset_minutes -> minutes offset from UTC (e.g., -360 for -06:00). If absent, 0.
    bool parse_iso8601_datetime(std::string_view s,
        int& yyyymmdd,
        int& seconds_since_mid,
        int& tz_offset_minutes);

    // ------------------------------------------------------------
    // Ranges
    // ------------------------------------------------------------

    // Inclusive integer range: [start, stop] step>0
    std::vector<std::size_t> make_range(std::size_t start, std::size_t stop, std::size_t step);

    // Inclusive double range with tiny epsilon to avoid FP miss on the last point
    std::vector<double> make_drange(double start, double stop, double step);

    // ------------------------------------------------------------
    // String helpers
    // ------------------------------------------------------------

    // Trim leading/trailing whitespace (in-place)
    void trim_in_place(std::string& s);

    // Strip wrapping quotes and trim whitespace.
    // Example: "C:\\X\\Y.csv" -> C:\X\Y.csv
    std::string strip_wrapping_quotes(std::string s);

    // Convert Windows path (C:\\Dev\\X or C:/Dev/X) to WSL path (/mnt/c/Dev/X)
    // when running on Linux. On Windows builds, returns the input unchanged.
    // Always strips wrapping quotes and trims.
    std::string normalize_path_for_runtime(std::string in);

    // ------------------------------------------------------------
    // Parsing helpers
    // ------------------------------------------------------------

    // Parse a line containing 3 whitespace-separated values.
    // Returns true if at least one value was parsed; missing values remain unchanged.
    // Commonly used for "min max step" style prompts.
    template <typename T>
    inline bool parse_triple_line(const std::string& line, T& a, T& b, T& c) {
        std::istringstream iss(line);
        bool any = false;
        T x{};
        if (iss >> x) { a = x; any = true; }
        if (iss >> x) { b = x; any = true; }
        if (iss >> x) { c = x; any = true; }
        return any;
    }

    // ------------------------------------------------------------
    // Prompt helpers
    // ------------------------------------------------------------

    // Yes/no prompt.
    // - If the user hits Enter: returns !default_no.
    // - Accepts y/Y or n/N as first character; otherwise returns !default_no.
    bool prompt_yes_no(std::istream& in,
        std::ostream& out,
        std::string_view question,
        bool default_no = true);

    // Prompts the user for a range (min, max, step).
    // - If the user hits Enter: keep defaults.
    // - If they enter fewer than 3 tokens: update only those provided.
    // - On bad input: keep defaults and print a small note.
    template <typename T>
    void prompt_range(std::istream& in,
        std::ostream& out,
        std::string_view label,
        T& minv, T& maxv, T& stepv)
    {
        out << label << " [default: " << minv << " " << maxv << " " << stepv << "]: ";
        out.flush();

        std::string line;
        std::getline(in, line);

        if (line.empty()) {
            return; // accept defaults
        }

        std::istringstream iss(line);

        bool any = false;

        T a{};
        if (iss >> a) { minv = a; any = true; }

        T b{};
        if (iss >> b) { maxv = b; any = true; }

        T c{};
        if (iss >> c) { stepv = c; any = true; }

        if (!any) {
            out << "  (Invalid input. Keeping defaults.)\n";
        }
    }

    template <class T>
    void prompt_scalar(std::istream & in, std::ostream & out, const char* label, T & value) {
        out << label << " [current: " << value << "]: ";
        std::string line;
        std::getline(in, line);
        
                        // trim
            auto is_ws = [](unsigned char c) { return std::isspace(c) != 0; };
        while (!line.empty() && is_ws(static_cast<unsigned char>(line.front()))) line.erase(line.begin());
        while (!line.empty() && is_ws(static_cast<unsigned char>(line.back()))) line.pop_back();       
            if (line.empty()) return;
            std::istringstream iss(line);
        T v{};
        iss >> v;
        if (!iss.fail() && iss.eof()) {
            value = v;
        }
        else {
            out << "  (invalid; keeping current)\n";
        }
    }

} // namespace sugar


