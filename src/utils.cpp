#include "utils.h"
#include <charconv>
#include <limits>
#include <vector>
#include <cstddef>
#include <algorithm>
#include <cctype>

namespace sugar {

    // ---- helpers -------------------------------------------------------------

    static inline bool parse_int(std::string_view sv, int& out) {
        // from_chars: no allocs, no locale
        const char* b = sv.data();
        const char* e = sv.data() + sv.size();
        auto res = std::from_chars(b, e, out);
        return (res.ec == std::errc());
    }

    static inline bool parse_2(std::string_view sv, int& out) {
        if (sv.size() != 2) return false;
        return parse_int(sv, out);
    }
    static inline bool parse_4(std::string_view sv, int& out) {
        if (sv.size() != 4) return false;
        return parse_int(sv, out);
    }

    // Parse "YYYY-MM-DD" into YYYYMMDD (no validation beyond ranges).
    static inline int pack_date(int y, int m, int d) {
        if (y <= 0 || m < 1 || m > 12 || d < 1 || d > 31) return -1;
        return y * 10000 + m * 100 + d;
    }

    // Try parse just the date portion at s[0..9] where s[4]=='-' and s[7]=='-'.
    static inline int try_parse_yyyy_mm_dd(std::string_view s) {
        if (s.size() < 10) return -1;
        if (!(s[4] == '-' && s[7] == '-')) return -1;
        int y = -1, m = -1, d = -1;
        if (!parse_4(s.substr(0, 4), y)) return -1;
        if (!parse_2(s.substr(5, 2), m)) return -1;
        if (!parse_2(s.substr(8, 2), d)) return -1;
        return pack_date(y, m, d);
    }

    // ---- public API ----------------------------------------------------------

    std::optional<int> parse_yyyymmdd(std::string_view s) {
        // 1) "YYYYMMDD"
        if (s.size() == 8) {
            int v = -1;
            if (parse_int(s, v) && v > 0) return v;
        }

        // 2) "YYYY-MM-DD"
        if (s.size() == 10 && s[4] == '-' && s[7] == '-') {
            const int v = try_parse_yyyy_mm_dd(s);
            return (v > 0) ? std::optional<int>(v) : std::nullopt;
        }

        // 3) "YYYY/MM/DD"
        if (s.size() == 10 && s[4] == '/' && s[7] == '/') {
            int y = -1, m = -1, d = -1;
            if (!parse_4(s.substr(0, 4), y)) return std::nullopt;
            if (!parse_2(s.substr(5, 2), m)) return std::nullopt;
            if (!parse_2(s.substr(8, 2), d)) return std::nullopt;
            const int v = pack_date(y, m, d);
            return (v > 0) ? std::optional<int>(v) : std::nullopt;
        }

        // 4) "MM/DD/YYYY"
        if (s.size() == 10 && s[2] == '/' && s[5] == '/') {
            int m = -1, d = -1, y = -1;
            if (!parse_2(s.substr(0, 2), m)) return std::nullopt;
            if (!parse_2(s.substr(3, 2), d)) return std::nullopt;
            if (!parse_4(s.substr(6, 4), y)) return std::nullopt;
            const int v = pack_date(y, m, d);
            return (v > 0) ? std::optional<int>(v) : std::nullopt;
        }

        // 5) ISO-8601 datetime variants: "YYYY-MM-DDTHH:MM[:SS][Z|+/-HH:MM]"
        if (s.size() >= 19 && s[4] == '-' && s[7] == '-' && s[10] == 'T') {
            const int v = try_parse_yyyy_mm_dd(s.substr(0, 10));
            return (v > 0) ? std::optional<int>(v) : std::nullopt;
        }

        return std::nullopt; // invalid
    }

    void format_yyyymmdd(int yyyymmdd, char out[9]) {
        // Render as 8 ASCII digits, most significant first.
        for (int i = 7; i >= 0; --i) {
            out[i] = char('0' + (yyyymmdd % 10));
            yyyymmdd /= 10;
        }
        out[8] = '\0';
    }

    // --- Full ISO-8601 date-time parser ---------------------------------

    // Expected: YYYY-MM-DD 'T' HH:MM [':' SS]? ( 'Z' | ('+'|'-') HH ':' MM )?
    // Returns true on success and fills outputs.
    bool parse_iso8601_datetime(std::string_view s,
        int& yyyymmdd,
        int& seconds_since_mid,
        int& tz_offset_minutes)
    {
        // Date
        if (s.size() < 16) return false;           // minimal: YYYY-MM-DDTHH:MM
        if (!(s[4] == '-' && s[7] == '-' && s[10] == 'T')) return false;

        yyyymmdd = try_parse_yyyy_mm_dd(s.substr(0, 10));
        if (yyyymmdd < 0) return false;

        // Time HH:MM[(:SS)?]
        int hh = -1, mm = -1, ss = 0;
        if (!parse_2(s.substr(11, 2), hh)) return false;
        if (s[13] != ':') return false;
        if (!parse_2(s.substr(14, 2), mm)) return false;

        std::size_t pos = 16;
        if (pos < s.size() && s[pos] == ':') {
            // seconds present
            if (pos + 3 > s.size()) return false;
            if (!parse_2(s.substr(pos + 1, 2), ss)) return false;
            pos += 3; // move past ":SS"
        }

        seconds_since_mid = hh * 3600 + mm * 60 + ss;

        // Timezone: optional
        tz_offset_minutes = 0; // default to 0 if absent
        if (pos >= s.size()) return true;

        char tzch = s[pos];
        if (tzch == 'Z') {
            // Zulu/UTC
            if (pos + 1 != s.size()) {
                // allow trailing char only if Z is last
                // if more stuff, reject
                return false;
            }
            tz_offset_minutes = 0;
            return true;
        }

        if (tzch == '+' || tzch == '-') {
            // Expect +/-HH:MM
            if (pos + 6 != s.size()) return false;
            int tzh = -1, tzm = -1;
            if (!parse_2(s.substr(pos + 1, 2), tzh)) return false;
            if (s[pos + 3] != ':') return false;
            if (!parse_2(s.substr(pos + 4, 2), tzm)) return false;
            int sign = (tzch == '-') ? -1 : 1;
            tz_offset_minutes = sign * (tzh * 60 + tzm);
            return true;
        }

        // Anything else after time don't accept.

        return false;
    }

    std::vector<std::size_t> make_range(std::size_t start, std::size_t stop, std::size_t step) {
        std::vector<std::size_t> v;
        if (step == 0 || start > stop) return v;
        v.reserve((stop - start) / step + 1);
        for (std::size_t x = start; x <= stop; x += step) v.push_back(x);
        return v;
    }

    std::vector<double> make_drange(double start, double stop, double step) {
        std::vector<double> v;
        if (step <= 0 || start > stop) return v;
        for (double x = start; x <= stop + 1e-12; x += step) v.push_back(x);
        return v;
    }

    void trim_in_place(std::string& s) {
        auto not_space = [](unsigned char ch) {
            return !std::isspace(ch);
            };

        s.erase(
            s.begin(),
            std::find_if(s.begin(), s.end(), not_space)
        );

        s.erase(
            std::find_if(s.rbegin(), s.rend(), not_space).base(),
            s.end()
        );
    }

    std::string strip_wrapping_quotes(std::string s) {
        trim_in_place(s);

        if (s.size() >= 2) {
            const char a = s.front();
            const char b = s.back();

            if ((a == '"' && b == '"') || (a == '\'' && b == '\'')) {
                s = s.substr(1, s.size() - 2);
                trim_in_place(s);
            }
        }
        return s;
    }


    std::string normalize_path_for_runtime(std::string in) {
        in = strip_wrapping_quotes(std::move(in));

#ifdef __linux__
        // Backslashes -> slashes
        std::replace(in.begin(), in.end(), '\\', '/');

        // Handle drive letter paths: C:/... -> /mnt/c/...
        if (in.size() >= 3 &&
            std::isalpha(static_cast<unsigned char>(in[0])) &&
            in[1] == ':' &&
            in[2] == '/')
        {
            char drive = static_cast<char>(std::tolower(static_cast<unsigned char>(in[0])));
            std::string rest = in.substr(3); // skip "C:/"
            return "/mnt/" + std::string(1, drive) + "/" + rest;
        }
#endif

        return in;
    }

    bool prompt_yes_no(std::istream& in, std::ostream& out, std::string_view question, bool default_no) {
        out << question << (default_no ? " [y/N]: " : " [Y/n]: ");
        out.flush();

        std::string line;
        std::getline(in, line);
        if (line.empty()) return !default_no;

        char c = static_cast<char>(std::tolower(static_cast<unsigned char>(line[0])));
        if (c == 'y') return true;
        if (c == 'n') return false;
        return !default_no;
    }


} // namespace sugar
