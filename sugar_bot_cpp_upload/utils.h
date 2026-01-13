#pragma once
#include <string_view>
#include <cstdint>
#include <vector>
#include <string>
#include <cstddef>
#include <sstream>
#include <string>


namespace sugar {

                                                        // Parse "YYYYMMDD" or "YYYY-MM-DD" or ISO-8601 "YYYY-MM-DDTHH:MM[:SS][Z|±HH:MM]"
                                                            // Returns YYYYMMDD as int, or -1 on failure.
    int parse_yyyymmdd(std::string_view s);

                                                        // Format YYYYMMDD -> "YYYYMMDD" (8 chars).
                                                            // Caller owns the fixed 9-byte buffer (8 chars; no null required for printing).
    void format_yyyymmdd(int yyyymmdd, char out[9]);

                                                        // --- optional for intraday ------------------------------------------
                                                            // Parse ISO-8601 datetime strings like:
                                                            //   2024-10-09T01:00:00-06:00
                                                            //   2025-09-12T17:30:00-06:00
                                                            //   2025-10-03T11:09:15-06:00
                                                            // Seconds are optional; timezone can be 'Z' or ±HH:MM. Offset minutes negative
                                                            // for west of UTC (e.g., -06:00 -> -360).
                                                            //
                                                            // On success, returns true and fills:
                                                            //   yyyymmdd            -> date as int YYYYMMDD
                                                            //   seconds_since_mid   -> seconds since local *clock time* in the string (00:00:00 = 0)
                                                            //   tz_offset_minutes   -> minutes offset from UTC (e.g., -360 for -06:00). If no TZ
                                                            //                           present, set to 0.
                                                            // On failure, returns false and leaves outputs unspecified.
    bool parse_iso8601_datetime(std::string_view s,
        int& yyyymmdd,
        int& seconds_since_mid,
        int& tz_offset_minutes);

    // Inclusive integer range: [start, stop] step>0
    std::vector<std::size_t> make_range(std::size_t start, std::size_t stop, std::size_t step);

    // Inclusive double range with tiny epsilon to avoid FP miss on the last point
    std::vector<double> make_drange(double start, double stop, double step);

    // Helper: parse "min max step" from a line, with type T (size_t or double)
    template <typename T>
    bool parse_triple_line(const std::string& line, T& a, T& b, T& c) {
        std::istringstream iss(line);
        return static_cast<bool>(iss >> a >> b >> c);
    }

    // Trim leading/trailing whitespace (in-place)
    void trim_in_place(std::string& s);

    // Strip wrapping quotes and trim whitespace
    std::string strip_wrapping_quotes(std::string s);

} // namespace sugar
