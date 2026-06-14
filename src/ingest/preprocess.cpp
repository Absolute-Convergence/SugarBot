#include "ingest/preprocess.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ingest/raw_table.h"
#include "ingest/utils.h"

namespace sugar {

    namespace {

        struct ParsedRow {
            TimeKey t{};
            std::vector<std::optional<double>> vals;
            std::size_t ordinal{};
        };

        std::string trim_copy(std::string s) {
            auto not_ws = [](unsigned char c) { return !std::isspace(c); };
            auto b = std::find_if(s.begin(), s.end(), not_ws);
            auto e = std::find_if(s.rbegin(), s.rend(), not_ws).base();
            if (b >= e) return {};
            return std::string(b, e);
        }

        bool iequals(const std::string& a, const std::string& b) {
            if (a.size() != b.size()) return false;
            for (std::size_t i = 0; i < a.size(); ++i) {
                if (std::tolower(static_cast<unsigned char>(a[i])) !=
                    std::tolower(static_cast<unsigned char>(b[i]))) return false;
            }
            return true;
        }

        bool is_missing_token(const std::string& tok, const DataSpec& spec) {
            if (tok.empty()) return true;
            for (const auto& x : spec.missing_tokens) {
                if (iequals(tok, x)) return true;
            }
            return false;
        }

        bool parse_double_strict(const std::string& s, double& out) {
            try {
                std::size_t idx = 0;
                out = std::stod(s, &idx);
                while (idx < s.size() && std::isspace(static_cast<unsigned char>(s[idx]))) ++idx;
                return idx == s.size();
            }
            catch (...) {
                return false;
            }
        }

        const char* time_kind_code(TimeKind kind) {
            switch (kind) {
            case TimeKind::YyyyMmDd: return "yyyymmdd";
            case TimeKind::Iso8601: return "iso8601";
            case TimeKind::YearQuarter: return "year_quarter";
            case TimeKind::YearMonth: return "year_month";
            case TimeKind::Index: return "index";
            }
            return "unknown";
        }

        std::string numeric_token_category(const std::string& raw) {
            const std::string tok = trim_copy(raw);
            if (tok.empty()) return "empty";
            if (tok.size() >= 2 && tok.front() == '(' && tok.back() == ')') {
                return "parenthesized_negative";
            }
            for (const char c : tok) {
                switch (c) {
                case '$':
                    return "currency_symbol";
                case '%':
                    return "percent";
                case ',':
                    return "grouping_comma";
                default:
                    break;
                }
            }
            for (const unsigned char c : tok) {
                if (std::isalpha(c)) return "suffix";
            }
            return "invalid_format";
        }

        PreprocessIssueAggregate& ensure_issue_aggregate(
            std::vector<PreprocessIssueAggregate>& aggregates,
            const std::string& code,
            const std::string& column,
            const std::string& category,
            const std::string& entity,
            const std::string& key) {
            auto it = std::find_if(
                aggregates.begin(),
                aggregates.end(),
                [&](const PreprocessIssueAggregate& existing) {
                    return existing.code == code &&
                        existing.column == column &&
                        existing.category == category &&
                        existing.entity == entity &&
                        existing.key == key;
                });
            if (it != aggregates.end()) {
                return *it;
            }

            PreprocessIssueAggregate aggregate{};
            aggregate.code = code;
            aggregate.column = column;
            aggregate.category = category;
            aggregate.entity = entity;
            aggregate.key = key;
            aggregates.push_back(std::move(aggregate));
            return aggregates.back();
        }

        void add_preprocess_issue(
            PreprocessResult& result,
            const std::string& code,
            std::size_t csv_data_row,
            const std::string& column,
            const std::string& token,
            const std::string& category,
            const std::string& entity = {},
            const std::string& key = {}) {
            if (result.error_code.empty()) {
                result.error_code = code;
            }

            auto& aggregate = ensure_issue_aggregate(
                result.error_aggregates,
                code,
                column,
                category,
                entity,
                key);
            ++aggregate.count;
            constexpr std::size_t kSampleLimit = 5;
            if (aggregate.samples.size() >= kSampleLimit) {
                return;
            }

            PreprocessIssueSample sample{};
            sample.csv_data_row = csv_data_row;
            sample.column = column;
            sample.token = token;
            sample.category = category;
            sample.entity = entity;
            sample.key = key;
            aggregate.samples.push_back(std::move(sample));
        }

        std::string sample_rows_text(const std::vector<std::size_t>& ordinals) {
            std::ostringstream out;
            const std::size_t limit = std::min<std::size_t>(5, ordinals.size());
            for (std::size_t i = 0; i < limit; ++i) {
                if (i) out << ",";
                out << (ordinals[i] + 1);
            }
            if (ordinals.size() > limit) {
                out << ",...";
            }
            return out.str();
        }

        bool parse_year_quarter(std::string s, TimeKey& out) {
            s = trim_copy(std::move(s));
            std::string cleaned;
            cleaned.reserve(s.size());
            for (char c : s) {
                if (!std::isspace(static_cast<unsigned char>(c)) && c != '-') cleaned.push_back(c);
            }

            const auto pos = cleaned.find_first_of("Qq");
            if (pos == std::string::npos || pos == 0 || pos + 1 >= cleaned.size()) return false;

            const std::string ys = cleaned.substr(0, pos);
            const std::string qs = cleaned.substr(pos + 1);
            int year = 0;
            int q = 0;
            try {
                year = std::stoi(ys);
                q = std::stoi(qs);
            }
            catch (...) {
                return false;
            }
            if (q < 1 || q > 4) return false;
            out = static_cast<TimeKey>(year * 10 + q);
            return true;
        }

        bool parse_year_month(std::string s, TimeKey& out) {
            s = trim_copy(std::move(s));
            std::string cleaned;
            cleaned.reserve(s.size());
            for (char c : s) {
                if (!std::isspace(static_cast<unsigned char>(c)) && c != '-') cleaned.push_back(c);
            }
            if (cleaned.size() != 6) return false;
            int year = 0;
            int month = 0;
            try {
                year = std::stoi(cleaned.substr(0, 4));
                month = std::stoi(cleaned.substr(4, 2));
            }
            catch (...) {
                return false;
            }
            if (month < 1 || month > 12) return false;
            out = static_cast<TimeKey>(year * 100 + month);
            return true;
        }

        bool unpack_yyyymmdd(int yyyymmdd, int& y, int& m, int& d) {
            if (yyyymmdd <= 0) return false;
            y = yyyymmdd / 10000;
            m = (yyyymmdd / 100) % 100;
            d = yyyymmdd % 100;
            if (m < 1 || m > 12) return false;
            if (d < 1 || d > 31) return false;
            return true;
        }

        // Civil date -> days since Unix epoch (1970-01-01), Howard Hinnant algorithm.
        std::int64_t days_from_civil(int y, unsigned m, unsigned d) {
            y -= (m <= 2) ? 1 : 0;
            const int era = (y >= 0 ? y : y - 399) / 400;
            const unsigned yoe = static_cast<unsigned>(y - era * 400);            // [0, 399]
            const int mp = static_cast<int>(m) + ((m > 2) ? -3 : 9);
            const unsigned doy = (153u * static_cast<unsigned>(mp) + 2u) / 5u + d - 1u; // [0, 365]
            const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;        // [0, 146096]
            return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
        }

        bool parse_iso8601_time_key(std::string_view tok, TimeKey& out) {
            int yyyymmdd = 0;
            int seconds_since_mid = 0;
            int tz_offset_minutes = 0;
            if (!parse_iso8601_datetime(tok, yyyymmdd, seconds_since_mid, tz_offset_minutes)) {
                return false;
            }

            int y = 0, m = 0, d = 0;
            if (!unpack_yyyymmdd(yyyymmdd, y, m, d)) {
                return false;
            }

            const std::int64_t days = days_from_civil(y, static_cast<unsigned>(m), static_cast<unsigned>(d));
            const std::int64_t epoch_seconds =
                days * 86400LL +
                static_cast<std::int64_t>(seconds_since_mid) -
                static_cast<std::int64_t>(tz_offset_minutes) * 60LL;

            out = static_cast<TimeKey>(epoch_seconds);
            return true;
        }

        bool parse_time_key(const std::string& tok, TimeKind kind, std::size_t row_idx, TimeKey& out) {
            switch (kind) {
            case TimeKind::YyyyMmDd: {
                const auto d = parse_yyyymmdd(tok);
                if (!d) return false;
                out = static_cast<TimeKey>(*d);
                return true;
            }
            case TimeKind::Iso8601: {
                return parse_iso8601_time_key(tok, out);
            }
            case TimeKind::YearQuarter:
                return parse_year_quarter(tok, out);
            case TimeKind::YearMonth:
                return parse_year_month(tok, out);
            case TimeKind::Index: {
                std::string t = trim_copy(tok);
                if (t.empty()) {
                    out = static_cast<TimeKey>(row_idx);
                    return true;
                }
                std::int64_t v = 0;
                const auto* begin = t.data();
                const auto* end = t.data() + t.size();
                auto r = std::from_chars(begin, end, v);
                if (r.ec != std::errc() || r.ptr != end) return false;
                out = static_cast<TimeKey>(v);
                return true;
            }
            }
            return false;
        }

        std::string time_parse_hint(const std::string& tok, TimeKind kind) {
            switch (kind) {
            case TimeKind::YyyyMmDd:
                return "hint: use time_kind \"YyyyMmDd\" or \"YYYY-MM-DD\" for date-only values such as 20211014, 2021-10-14, or 2021/10/14";
            case TimeKind::Iso8601:
                if (parse_yyyymmdd(tok).has_value()) {
                    return "hint: token looks date-only; use time_kind \"YyyyMmDd\" or \"YYYY-MM-DD\" for YYYY-MM-DD or YYYYMMDD dates";
                }
                return "hint: use time_kind \"Iso8601\" or \"ISO-8601\" for full timestamps such as 2024-10-09T07:00:00Z";
            case TimeKind::YearQuarter:
                return "hint: use time_kind \"YearQuarter\" for values such as 2024Q1 or 2024-Q1";
            case TimeKind::YearMonth:
                return "hint: use time_kind \"YearMonth\" for values such as 202410 or 2024-10";
            case TimeKind::Index:
                return "hint: use time_kind \"Index\" for integer row keys; blank values fall back to the row index";
            }
            return {};
        }

        bool parse_value_token(const std::string& tok, const DataSpec& spec, std::optional<double>& out_val, std::string& err) {
            const std::string t = trim_copy(tok);
            if (is_missing_token(t, spec)) {
                out_val.reset();
                return true;
            }
            double d = 0.0;
            if (!parse_double_strict(t, d)) {
                err = "invalid numeric token: '" + tok + "'";
                return false;
            }
            out_val = d;
            return true;
        }

        std::string join_csv(const std::vector<std::string>& items) {
            std::string out;
            for (std::size_t i = 0; i < items.size(); ++i) {
                if (i) out += ", ";
                out += items[i];
            }
            return out;
        }

        std::string available_columns_suffix(const std::vector<std::string>& columns) {
            if (columns.empty()) return {};
            return " (available columns: " + join_csv(columns) + ")";
        }

        std::shared_ptr<IndexSchema> make_index_schema_for_spec(const DataSpec& spec, bool has_entity) {
            auto is = std::make_shared<IndexSchema>();
            if (data_spec_has_time_axis(spec)) {
                AxisDef t{};
                t.name = "time";
                t.kind = AxisKind::Time;
                t.ordered = true;
                t.align_hint = AxisAlignmentHint::Calendar;
                is->add_axis(std::move(t));
            }
            if (has_entity) {
                AxisDef e{};
                e.name = "entity";
                e.kind = AxisKind::Entity;
                e.ordered = true;
                is->add_axis(std::move(e));
            }
            if (data_spec_has_spatial_axis(spec)) {
                is->add_spatial_axis_2d(*spec.spatial_x_col, *spec.spatial_y_col);
            }
            return is;
        }

        bool build_frame_for_group(
            const std::string& entity,
            const DataSpec& spec,
            const std::shared_ptr<FrameSchema>& schema,
            const std::vector<ColumnId>& value_ids,
            std::vector<ParsedRow> rows,
            NumericFrame& out,
            std::size_t& out_dropped_rows,
            PreprocessResult& diagnostics,
            std::string& err)
        {
            out_dropped_rows = 0;
            const bool has_time_axis = data_spec_has_time_axis(spec);

            if (has_time_axis && spec.sort_by_time) {
                std::stable_sort(rows.begin(), rows.end(),
                    [](const ParsedRow& a, const ParsedRow& b) {
                        if (a.t != b.t) return a.t < b.t;
                        return a.ordinal < b.ordinal;
                    });
            }

            if (has_time_axis && spec.dedupe == DataSpec::DedupePolicy::Error) {
                std::map<TimeKey, std::vector<std::size_t>> rows_by_key;
                for (const auto& r : rows) {
                    rows_by_key[r.t].push_back(r.ordinal);
                }

                for (const auto& [key, ordinals] : rows_by_key) {
                    if (ordinals.size() <= 1) {
                        continue;
                    }

                    const std::string key_text = std::to_string(key);
                    for (const auto ordinal : ordinals) {
                        add_preprocess_issue(
                            diagnostics,
                            "preprocess.duplicate_time_key",
                            ordinal + 1,
                            spec.time_col,
                            key_text,
                            "duplicate_time_key",
                            entity,
                            key_text);
                    }
                    err = "duplicate time key for entity '" + entity +
                        "': key " + key_text +
                        " appears " + std::to_string(ordinals.size()) +
                        " times; sample rows=" + sample_rows_text(ordinals);
                    return false;
                }
            }

            std::vector<ParsedRow> deduped;
            deduped.reserve(rows.size());

            if (has_time_axis) {
                std::unordered_map<TimeKey, std::size_t> seen;
                for (const auto& r : rows) {
                    auto it = seen.find(r.t);
                    if (it == seen.end()) {
                        seen.emplace(r.t, deduped.size());
                        deduped.push_back(r);
                        continue;
                    }

                    if (spec.dedupe == DataSpec::DedupePolicy::Error) {
                        err = "duplicate time key for entity '" + entity + "'";
                        return false;
                    }
                    if (spec.dedupe == DataSpec::DedupePolicy::Last) {
                        deduped[it->second] = r;
                    }
                    // First: keep existing
                }
            }
            else {
                for (const auto& r : rows) {
                    deduped.push_back(r);
                }
            }

            const std::size_t ncols = value_ids.size();
            std::vector<std::vector<double>> cols(ncols);
            std::vector<std::optional<double>> last(ncols);
            std::size_t kept_rows = 0;

            const double qnan = std::numeric_limits<double>::quiet_NaN();

            for (const auto& r : deduped) {
                bool row_missing = false;
                for (const auto& v : r.vals) {
                    if (!v.has_value()) { row_missing = true; break; }
                }
                if (row_missing && spec.missing_default.kind == MissingKind::DropRow) {
                    ++out_dropped_rows;
                    continue;
                }

                ++kept_rows;
                for (std::size_t c = 0; c < ncols; ++c) {
                    double v = qnan;
                    if (r.vals[c].has_value()) {
                        v = *r.vals[c];
                    }
                    else {
                        switch (spec.missing_default.kind) {
                        case MissingKind::NaN:
                            v = qnan;
                            break;
                        case MissingKind::Const:
                            v = spec.missing_default.const_value;
                            break;
                        case MissingKind::Ffill:
                            v = last[c].has_value() ? *last[c] : qnan;
                            break;
                        case MissingKind::DropRow:
                            v = qnan;
                            break;
                        }
                    }
                    cols[c].push_back(v);
                    if (std::isfinite(v)) {
                        last[c] = v;
                    }
                }
            }

            if (has_time_axis) {
                std::vector<TimeKey> times;
                times.reserve(kept_rows);
                for (const auto& r : deduped) {
                    bool row_missing = false;
                for (const auto& v : r.vals) {
                    if (!v.has_value()) { row_missing = true; break; }
                }
                if (row_missing && spec.missing_default.kind == MissingKind::DropRow) {
                    continue;
                    }
                    times.push_back(r.t);
                }
                out = NumericFrame(schema, std::move(times));
            }
            else {
                out = NumericFrame(schema, kept_rows);
            }
            for (std::size_t c = 0; c < ncols; ++c) {
                auto dst = out.ensure_col(value_ids[c]);
                std::copy(cols[c].begin(), cols[c].end(), dst.begin());
            }
            return true;
        }

        int rounded_missing_percent(const ValueColumnQuality& stats) {
            if (stats.total_values == 0) return 0;
            return static_cast<int>(
                std::round(
                    (100.0 * static_cast<double>(stats.missing_values)) /
                    static_cast<double>(stats.total_values)));
        }

        void accumulate_column_quality(
            const NumericFrame& frame,
            ColumnId column,
            ValueColumnQuality& stats) {
            const auto values = frame.col(column);
            stats.total_values += values.size();
            for (const double value : values) {
                if (!std::isfinite(value)) {
                    ++stats.missing_values;
                    continue;
                }
                ++stats.finite_values;
                if (value != 0.0) {
                    stats.has_nonzero_finite = true;
                }
            }
        }

        void append_value_column_quality(
            const std::vector<std::string>& ingest_cols,
            const std::vector<ColumnId>& value_ids,
            std::size_t value_col_count,
            const PreprocessResult& result,
            std::vector<ValueColumnQuality>& quality,
            std::vector<std::string>& warnings) {
            const std::size_t ncols = std::min(
                value_col_count,
                std::min(ingest_cols.size(), value_ids.size()));
            quality.clear();
            quality.reserve(ncols);
            for (std::size_t i = 0; i < ncols; ++i) {
                ValueColumnQuality stats{};
                stats.name = ingest_cols[i];
                if (result.frame.has_value()) {
                    accumulate_column_quality(*result.frame, value_ids[i], stats);
                }
                if (result.frameset.has_value()) {
                    for (const auto& kv : result.frameset->by_entity) {
                        accumulate_column_quality(kv.second, value_ids[i], stats);
                    }
                }

                quality.push_back(stats);

                if (stats.total_values == 0) {
                    continue;
                }
                if (stats.finite_values == 0) {
                    warnings.push_back(
                        "value column '" + ingest_cols[i] + "' contains no finite values after preprocessing");
                }
                else if (!stats.has_nonzero_finite) {
                    warnings.push_back(
                        "value column '" + ingest_cols[i] + "' is uniformly zero after preprocessing");
                }
                else if (stats.missing_values * 2 > stats.total_values) {
                    warnings.push_back(
                        "value column '" + ingest_cols[i] + "' has " +
                        std::to_string(rounded_missing_percent(stats)) +
                        "% missing values after preprocessing (" +
                        std::to_string(stats.missing_values) + "/" +
                        std::to_string(stats.total_values) + " rows)");
                }
            }
        }

    } // namespace

    PreprocessResult preprocess_csv(const DataSpec& spec,
        const std::string& csv_path,
        std::shared_ptr<FrameSchema> schema)
    {
        return preprocess_tabular(spec, csv_path, TabularFileFormat::Csv, std::move(schema));
    }

    PreprocessResult preprocess_tabular(const DataSpec& spec,
        const std::string& input_path,
        TabularFileFormat input_format,
        std::shared_ptr<FrameSchema> schema)
    {
        PreprocessResult r{};
        auto fail = [&](std::string code, std::string message) {
            r.error_code = std::move(code);
            r.err = std::move(message);
            return r;
        };

        if (!schema) {
            return fail("preprocess.schema_missing", "schema must not be null");
        }

        bool had_header = false;
        std::string load_err;
        CsvHeaderPolicy raw_policy = CsvHeaderPolicy::Required;
        switch (spec.header_policy) {
        case HeaderPolicy::RequiredHeaders:
            raw_policy = CsvHeaderPolicy::Required;
            break;
        case HeaderPolicy::OptionalHeaders:
            raw_policy = CsvHeaderPolicy::AutoDetect;
            break;
        case HeaderPolicy::HeaderlessPositional:
                raw_policy = CsvHeaderPolicy::None;
            break;
        }

        TabularLoadDiagnostics input_load{};
        RawTable tbl = load_tabular_table(input_path, input_format, raw_policy, had_header, load_err, &input_load);
        r.input_format = input_load.resolved_format;
        r.input_row_count = input_load.data_row_count;
        r.csv_load.physical_line_count = input_load.physical_line_count;
        r.csv_load.blank_line_count = input_load.blank_line_count;
        r.csv_load.data_row_count = input_load.data_row_count;
        r.csv_had_header = had_header;
        if (!load_err.empty()) {
            const std::string code = input_load.resolved_format == TabularFileFormat::Parquet
                ? "parquet.load_failed"
                : "csv.load_failed";
            return fail(code, load_err);
        }
        if (spec.header_policy == HeaderPolicy::RequiredHeaders && !had_header) {
            return fail(
                "preprocess.header_required",
                "header_policy=RequiredHeaders requires a CSV header row");
        }

        const bool using_positional_columns = !had_header;
        if (input_load.blank_line_count > 0) {
            r.warnings.push_back(
                "csv loader skipped " + std::to_string(input_load.blank_line_count) + " blank row(s)");
        }
        if (using_positional_columns && spec.header_policy != HeaderPolicy::RequiredHeaders) {
            r.warnings.push_back("csv input used headerless positional mapping");
        }
        if (using_positional_columns && spec.columns_by_index.empty()) {
            if (spec.header_policy == HeaderPolicy::HeaderlessPositional) {
                return fail(
                    "dataspec.columns_by_index_required",
                    "header_policy=HeaderlessPositional requires non-empty columns_by_index mapping");
            }
            else {
                return fail(
                    "dataspec.columns_by_index_required",
                    "CSV appears headerless; provide columns_by_index or set header_policy=RequiredHeaders");
            }
        }

        std::unordered_map<std::string, std::size_t> idx_by_name;
        std::vector<std::string> available_columns;
        if (had_header) {
            std::vector<std::string> canonical_headers;
            canonical_headers.reserve(tbl.headers.size());
            for (const auto& raw : tbl.headers) {
                auto it = spec.rename.find(raw);
                canonical_headers.push_back(it == spec.rename.end() ? raw : it->second);
            }

            for (std::size_t i = 0; i < canonical_headers.size(); ++i) {
                const auto key = trim_copy(canonical_headers[i]);
                if (key.empty()) continue;
                if (!idx_by_name.emplace(key, i).second) {
                    return fail(
                        "dataspec.duplicate_canonical_column",
                        "duplicate canonical column name: " + key);
                }
                available_columns.push_back(key);
            }
        }
        else {
            std::vector<std::pair<std::size_t, std::string>> columns_by_position;
            columns_by_position.reserve(spec.columns_by_index.size());
            for (const auto& kv : spec.columns_by_index) {
                columns_by_position.emplace_back(kv.second, kv.first);
            }
            std::sort(columns_by_position.begin(), columns_by_position.end());
            for (const auto& kv : columns_by_position) {
                const auto key = trim_copy(kv.second);
                if (key.empty()) continue;
                idx_by_name[key] = kv.first;
                available_columns.push_back(key);
            }
        }

        r.parsed_columns = available_columns;

        auto find_idx = [&](const std::string& name, std::size_t& out_idx) -> bool {
            auto it = idx_by_name.find(name);
            if (it == idx_by_name.end()) return false;
            out_idx = it->second;
            return true;
        };

        const bool has_time_axis = data_spec_has_time_axis(spec);
        std::size_t time_idx = 0;
        if (has_time_axis) {
            if (!find_idx(spec.time_col, time_idx)) {
                return fail(
                    "preprocess.missing_time_col",
                    "time_col not found after rename: " + spec.time_col +
                        available_columns_suffix(available_columns));
            }
        }

        std::size_t entity_idx = 0;
        const bool has_entity = spec.entity_col.has_value();
        if (has_entity && !find_idx(*spec.entity_col, entity_idx)) {
            return fail(
                "preprocess.missing_entity_col",
                "entity_col not found after rename: " + *spec.entity_col +
                    available_columns_suffix(available_columns));
        }

        if (spec.value_cols.empty()) {
            return fail("dataspec.value_cols_empty", "value_cols must not be empty");
        }

        if (std::find(spec.value_cols.begin(), spec.value_cols.end(), spec.primary_value_col) == spec.value_cols.end()) {
            return fail(
                "dataspec.primary_value_not_in_value_cols",
                "primary_value_col must be included in value_cols");
        }

        std::vector<std::string> ingest_cols = spec.value_cols;
        if (spec.spatial_x_col && spec.spatial_y_col) {
            auto has_col_name = [&](const std::string& n) {
                return std::find(ingest_cols.begin(), ingest_cols.end(), n) != ingest_cols.end();
            };
            if (!has_col_name(*spec.spatial_x_col)) ingest_cols.push_back(*spec.spatial_x_col);
            if (!has_col_name(*spec.spatial_y_col)) ingest_cols.push_back(*spec.spatial_y_col);
        }

        if (using_positional_columns) {
            std::set<std::string> required;
            if (has_time_axis) required.insert(spec.time_col);
            for (const auto& c : ingest_cols) required.insert(c);
            if (spec.entity_col) required.insert(*spec.entity_col);

            std::vector<std::string> missing;
            for (const auto& name : required) {
                if (idx_by_name.find(name) == idx_by_name.end()) {
                    missing.push_back(name);
                }
            }
            if (!missing.empty()) {
                return fail(
                    "dataspec.columns_by_index_incomplete",
                    "incomplete columns_by_index mapping; missing: " + join_csv(missing));
            }
        }

        std::vector<std::size_t> value_idx;
        std::vector<ColumnId> value_ids;
        value_idx.reserve(ingest_cols.size());
        value_ids.reserve(ingest_cols.size());
        for (const auto& name : ingest_cols) {
            std::size_t idx = 0;
            if (!find_idx(name, idx)) {
                return fail(
                    "preprocess.missing_value_col",
                    "value_col not found after rename: " + name +
                        available_columns_suffix(available_columns));
            }
            value_idx.push_back(idx);
            value_ids.push_back(schema->columns().get_or_add(name));
        }

        std::optional<std::size_t> spatial_x_ingest_idx;
        std::optional<std::size_t> spatial_y_ingest_idx;
        if (spec.spatial_x_col && spec.spatial_y_col) {
            for (std::size_t i = 0; i < ingest_cols.size(); ++i) {
                if (ingest_cols[i] == *spec.spatial_x_col) spatial_x_ingest_idx = i;
                if (ingest_cols[i] == *spec.spatial_y_col) spatial_y_ingest_idx = i;
            }
            if (!spatial_x_ingest_idx || !spatial_y_ingest_idx) {
                return fail(
                    "preprocess.spatial_axis_resolution_failed",
                    "spatial_x_col/spatial_y_col resolution failed");
            }
        }

        std::unordered_map<std::string, std::vector<ParsedRow>> groups;
        const std::string single_key = "__single__";
        std::size_t total_dropped_rows = 0;

        for (std::size_t ri = 0; ri < tbl.rows.size(); ++ri) {
            const auto& raw = tbl.rows[ri];
            auto get_cell = [&](std::size_t i) -> std::string {
                if (i >= raw.size()) return {};
                return trim_copy(raw[i]);
            };

            TimeKey tk{};
            if (has_time_axis) {
                const std::string tcell = get_cell(time_idx);
                if (!parse_time_key(tcell, spec.time_kind, ri, tk)) {
                    add_preprocess_issue(
                        r,
                        "preprocess.time_parse_failed",
                        ri + 1,
                        spec.time_col,
                        tcell,
                        time_kind_code(spec.time_kind));
                    return fail(
                        "preprocess.time_parse_failed",
                        "time parse failed at row " + std::to_string(ri + 1) + ": '" + tcell + "'; " +
                            time_parse_hint(tcell, spec.time_kind));
                }
            }
            const std::string ekey = has_entity ? get_cell(entity_idx) : single_key;

            ParsedRow pr{};
            pr.t = tk;
            pr.ordinal = ri;
            pr.vals.resize(value_idx.size());

            bool row_has_parse_error = false;
            std::string first_row_parse_error;
            for (std::size_t c = 0; c < value_idx.size(); ++c) {
                const std::string tok = get_cell(value_idx[c]);
                std::optional<double> v;
                std::string e;
                if (!parse_value_token(tok, spec, v, e)) {
                    row_has_parse_error = true;
                    if (first_row_parse_error.empty()) {
                        first_row_parse_error =
                            "value parse failed at row " + std::to_string(ri + 1) +
                            ", col '" + ingest_cols[c] + "': " + e;
                    }
                    add_preprocess_issue(
                        r,
                        "preprocess.value_parse_failed",
                        ri + 1,
                        ingest_cols[c],
                        tok,
                        numeric_token_category(tok),
                        ekey);
                    continue;
                }

                const bool is_spatial_col =
                    (spatial_x_ingest_idx && c == *spatial_x_ingest_idx) ||
                    (spatial_y_ingest_idx && c == *spatial_y_ingest_idx);
                if (is_spatial_col && !v.has_value()) {
                    row_has_parse_error = true;
                    if (first_row_parse_error.empty()) {
                        first_row_parse_error =
                            "spatial axis parse failed at row " + std::to_string(ri + 1) +
                            ", col '" + ingest_cols[c] + "': value is missing/non-numeric";
                    }
                    add_preprocess_issue(
                        r,
                        "preprocess.spatial_axis_parse_failed",
                        ri + 1,
                        ingest_cols[c],
                        tok,
                        "missing_spatial_value",
                        ekey);
                    continue;
                }
                pr.vals[c] = v;
            }

            if (row_has_parse_error) {
                if (r.err.empty() && !first_row_parse_error.empty()) {
                    r.err = first_row_parse_error;
                }
                continue;
            }

            groups[ekey].push_back(std::move(pr));
        }

        if (!r.error_aggregates.empty()) {
            if (r.err.empty()) {
                r.err = "preprocess failed with structured parse diagnostics";
            }
            return r;
        }

        // TODO(data-spec): resampling/aggregation hook goes here (no behavior yet).

        if (!has_entity) {
            NumericFrame out(schema);
            std::size_t dropped_rows = 0;
            std::string e;
            if (!build_frame_for_group(
                    single_key,
                    spec,
                    schema,
                    value_ids,
                    groups[single_key],
                    out,
                    dropped_rows,
                    r,
                    e)) {
                if (r.error_code.empty()) {
                    r.error_code = "preprocess.frame_build_failed";
                }
                r.err = e;
                return r;
            }
            total_dropped_rows += dropped_rows;
            out.set_index_schema(make_index_schema_for_spec(spec, false));
            r.frame = std::move(out);
            r.dropped_row_count = total_dropped_rows;
            if (total_dropped_rows > 0) {
                r.warnings.push_back(
                    "drop-row missing policy removed " + std::to_string(total_dropped_rows) + " row(s)");
            }
            append_value_column_quality(
                ingest_cols,
                value_ids,
                spec.value_cols.size(),
                r,
                r.value_column_quality,
                r.warnings);
            return r;
        }

        FrameSet fs{};
        fs.schema = schema;
        fs.index_schema = make_index_schema_for_spec(spec, true);
        for (auto& kv : groups) {
            NumericFrame out(schema);
            std::size_t dropped_rows = 0;
            std::string e;
            if (!build_frame_for_group(
                    kv.first,
                    spec,
                    schema,
                    value_ids,
                    std::move(kv.second),
                    out,
                    dropped_rows,
                    r,
                    e)) {
                if (r.error_code.empty()) {
                    r.error_code = "preprocess.frame_build_failed";
                }
                r.err = e;
                return r;
            }
            total_dropped_rows += dropped_rows;
            out.set_index_schema(fs.index_schema);
            fs.by_entity.emplace(kv.first, std::move(out));
        }

        r.frameset = std::move(fs);
        r.dropped_row_count = total_dropped_rows;
        if (total_dropped_rows > 0) {
            r.warnings.push_back(
                "drop-row missing policy removed " + std::to_string(total_dropped_rows) + " row(s)");
        }
        append_value_column_quality(
            ingest_cols,
            value_ids,
            spec.value_cols.size(),
            r,
            r.value_column_quality,
            r.warnings);
        return r;
    }

} // namespace sugar
