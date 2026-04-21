#include "ingest/data_spec.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <utility>

#include "ingest/json_min.h"

namespace sugar {

    namespace {
        using namespace sugar::json_min;

        std::string to_lower(std::string s) {
            std::transform(s.begin(), s.end(), s.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        }

        bool parse_time_kind(const std::string& s, TimeKind& out) {
            const std::string x = to_lower(s);
            if (x == "yyyymmdd" || x == "yyyy_mm_dd") { out = TimeKind::YyyyMmDd; return true; }
            if (x == "iso8601" || x == "iso_8601") { out = TimeKind::Iso8601; return true; }
            if (x == "yearquarter" || x == "year_quarter") { out = TimeKind::YearQuarter; return true; }
            if (x == "yearmonth" || x == "year_month") { out = TimeKind::YearMonth; return true; }
            if (x == "index") { out = TimeKind::Index; return true; }
            return false;
        }

        bool parse_header_policy(const std::string& s, HeaderPolicy& out) {
            const std::string x = to_lower(s);
            if (x == "requiredheaders" || x == "required_headers") {
                out = HeaderPolicy::RequiredHeaders;
                return true;
            }
            if (x == "optionalheaders" || x == "optional_headers") {
                out = HeaderPolicy::OptionalHeaders;
                return true;
            }
            if (x == "headerlesspositional" || x == "headerless_positional") {
                out = HeaderPolicy::HeaderlessPositional;
                return true;
            }
            return false;
        }

        bool parse_input_profile(const std::string& s, InputProfile& out) {
            const std::string x = to_lower(s);
            if (x == "auto") {
                out = InputProfile::Auto;
                return true;
            }
            if (x == "legacy_ohlcv_date" || x == "legacy" || x == "date_ohlcv") {
                out = InputProfile::LegacyOhlcvDate;
                return true;
            }
            if (x == "iso_ohlcv_time" || x == "iso" || x == "time_ohlcv") {
                out = InputProfile::IsoOhlcvTime;
                return true;
            }
            return false;
        }

        bool parse_missing_kind(const std::string& s, MissingKind& out) {
            const std::string x = to_lower(s);
            if (x == "droprow" || x == "drop_row") { out = MissingKind::DropRow; return true; }
            if (x == "nan") { out = MissingKind::NaN; return true; }
            if (x == "ffill" || x == "forward_fill") { out = MissingKind::Ffill; return true; }
            if (x == "const" || x == "constant") { out = MissingKind::Const; return true; }
            return false;
        }

        bool parse_dedupe(const std::string& s, DataSpec::DedupePolicy& out) {
            const std::string x = to_lower(s);
            if (x == "error") { out = DataSpec::DedupePolicy::Error; return true; }
            if (x == "first") { out = DataSpec::DedupePolicy::First; return true; }
            if (x == "last") { out = DataSpec::DedupePolicy::Last; return true; }
            return false;
        }

        bool parse_alignment_calendar_policy(const std::string& s, align::CalendarPolicy& out) {
            const std::string x = to_lower(s);
            if (x == "intersection") {
                out = align::CalendarPolicy::Intersection;
                return true;
            }
            if (x == "union") {
                out = align::CalendarPolicy::Union;
                return true;
            }
            return false;
        }

        bool parse_non_temporal_multi_series_policy(
            const std::string& s,
            align::NonTemporalMultiSeriesPolicy& out) {
            const std::string x = to_lower(s);
            if (x == "reject") {
                out = align::NonTemporalMultiSeriesPolicy::Reject;
                return true;
            }
            if (x == "equallengthrowdomain" ||
                x == "equal_length_row_domain" ||
                x == "equal_length") {
                out = align::NonTemporalMultiSeriesPolicy::EqualLengthRowDomain;
                return true;
            }
            return false;
        }

        bool parse_string_array(const Value& v, std::vector<std::string>& out, std::string& err, const char* field_name) {
            if (!v.is_arr()) {
                err = std::string(field_name) + " must be an array";
                return false;
            }
            out.clear();
            for (const auto& x : v.as_arr()) {
                if (!x.is_str()) {
                    err = std::string(field_name) + " entries must be strings";
                    return false;
                }
                out.push_back(x.as_str());
            }
            return true;
        }

        bool parse_rename_map(const Value& v, std::unordered_map<std::string, std::string>& out, std::string& err) {
            if (!v.is_obj()) { err = "rename must be object"; return false; }
            out.clear();
            for (const auto& kv : v.as_obj()) {
                if (!kv.second.is_str()) {
                    err = "rename values must be strings";
                    return false;
                }
                out[kv.first] = kv.second.as_str();
            }
            return true;
        }

        bool parse_missing_policy(const Value& v, MissingPolicy& out, std::string& err) {
            if (v.is_str()) {
                MissingKind k{};
                if (!parse_missing_kind(v.as_str(), k)) {
                    err = "missing_default string must be one of DropRow|NaN|Ffill|Const";
                    return false;
                }
                out.kind = k;
                return true;
            }

            if (!v.is_obj()) {
                err = "missing_default must be string or object";
                return false;
            }

            const auto& o = v.as_obj();
            auto* k = obj_get(o, "kind");
            if (!k || !k->is_str()) {
                err = "missing_default.kind must be string";
                return false;
            }
            MissingKind mk{};
            if (!parse_missing_kind(k->as_str(), mk)) {
                err = "missing_default.kind must be one of DropRow|NaN|Ffill|Const";
                return false;
            }
            out.kind = mk;

            if (auto* cv = obj_get(o, "const_value")) {
                if (!cv->is_num()) {
                    err = "missing_default.const_value must be number";
                    return false;
                }
                out.const_value = cv->number_as_f64();
            }
            return true;
        }

        bool parse_columns_by_index(const Value& v,
            std::unordered_map<std::string, std::size_t>& out,
            std::string& err) {
            if (!v.is_obj()) {
                err = "columns_by_index must be object";
                return false;
            }
            out.clear();
            for (const auto& kv : v.as_obj()) {
                const auto& key = kv.first;
                const auto& vv = kv.second;
                if (!vv.is_i64()) {
                    err = "columns_by_index values must be non-negative integers";
                    return false;
                }
                const std::int64_t idx64 = vv.as_i64();
                if (idx64 < 0) {
                    err = "columns_by_index values must be non-negative integers";
                    return false;
                }
                out[key] = static_cast<std::size_t>(idx64);
            }
            return true;
        }

        bool parse_optional_string_field(const Object& o,
            const char* key,
            std::optional<std::string>& out,
            std::string& err) {
            if (auto* p = obj_get(o, key)) {
                if (p->is_null()) {
                    out.reset();
                    return true;
                }
                if (!p->is_str()) {
                    err = std::string(key) + " must be string or null";
                    return false;
                }
                out = p->as_str();
            }
            return true;
        }

        DataSpec profile_defaults(InputProfile p) {
            DataSpec ds{};
            ds.missing_tokens = { "", "NA", "null", "nan", "NaN" };
            ds.missing_default.kind = MissingKind::NaN;
            ds.sort_by_time = true;
            ds.dedupe = DataSpec::DedupePolicy::Error;
            ds.primary_value_col = "close";
            ds.value_cols = { "open", "high", "low", "close", "volume" };

            if (p == InputProfile::IsoOhlcvTime) {
                ds.time_col = "time";
                ds.time_kind = TimeKind::Iso8601;
                ds.rename = {
                    {"Date", "time"},
                    {"Time", "time"},
                    {"Open", "open"},
                    {"High", "high"},
                    {"Low", "low"},
                    {"Close", "close"},
                    {"Volume", "volume"},
                };
            }
            else {
                ds.time_col = "date";
                ds.time_kind = TimeKind::YyyyMmDd;
                ds.rename = {
                    {"Date", "date"},
                    {"Time", "date"},
                    {"time", "date"},
                    {"Open", "open"},
                    {"High", "high"},
                    {"Low", "low"},
                    {"Close", "close"},
                    {"Volume", "volume"},
                };
            }
            return ds;
        }
    } // namespace

    bool data_spec_has_time_axis(const DataSpec& spec) {
        return !spec.time_col.empty();
    }

    bool data_spec_has_spatial_axis(const DataSpec& spec) {
        return spec.spatial_x_col.has_value() && spec.spatial_y_col.has_value();
    }

    bool parse_data_spec_value(const json_min::Value& root, DataSpec& out, std::string& err) {
        using namespace sugar::json_min;

        if (!root.is_obj()) {
            err = "DataSpec root must be object";
            return false;
        }

        DataSpec tmp{};
        const auto& o = root.as_obj();
        bool has_time_kind = false;

        if (auto* p = obj_get(o, "input_profile")) {
            if (!p->is_str()) {
                err = "input_profile must be one of auto|legacy_ohlcv_date|iso_ohlcv_time";
                return false;
            }
            if (!parse_input_profile(p->as_str(), tmp.input_profile)) {
                err = "input_profile must be one of auto|legacy_ohlcv_date|iso_ohlcv_time";
                return false;
            }
        }

        if (auto* p = obj_get(o, "time_col")) {
            if (!p->is_str()) {
                err = "time_col must be string";
                return false;
            }
            tmp.time_col = p->as_str();
        }

        if (auto* p = obj_get(o, "header_policy")) {
            if (!p->is_str()) {
                err = "header_policy must be one of RequiredHeaders|OptionalHeaders|HeaderlessPositional";
                return false;
            }
            if (!parse_header_policy(p->as_str(), tmp.header_policy)) {
                err = "header_policy must be one of RequiredHeaders|OptionalHeaders|HeaderlessPositional";
                return false;
            }
        }

        if (auto* p = obj_get(o, "columns_by_index")) {
            if (!parse_columns_by_index(*p, tmp.columns_by_index, err)) return false;
        }

        if (auto* p = obj_get(o, "time_kind")) {
            if (!p->is_str()) {
                err = "time_kind must be one of YyyyMmDd|Iso8601|YearQuarter|YearMonth|Index";
                return false;
            }
            if (!parse_time_kind(p->as_str(), tmp.time_kind)) {
                err = "time_kind must be one of YyyyMmDd|Iso8601|YearQuarter|YearMonth|Index";
                return false;
            }
            has_time_kind = true;
        }

        if (!parse_optional_string_field(o, "entity_col", tmp.entity_col, err)) return false;
        if (!parse_optional_string_field(o, "spatial_x_col", tmp.spatial_x_col, err)) return false;
        if (!parse_optional_string_field(o, "spatial_y_col", tmp.spatial_y_col, err)) return false;
        if (tmp.spatial_x_col.has_value() != tmp.spatial_y_col.has_value()) {
            err = "spatial_x_col and spatial_y_col must both be set or both be null/omitted";
            return false;
        }

        if (auto* p = obj_get(o, "value_cols")) {
            if (!parse_string_array(*p, tmp.value_cols, err, "value_cols")) return false;
        }

        if (auto* p = obj_get(o, "rename")) {
            if (!parse_rename_map(*p, tmp.rename, err)) return false;
        }

        if (auto* p = obj_get(o, "primary_value_col")) {
            if (!p->is_str()) {
                err = "primary_value_col must be string";
                return false;
            }
            tmp.primary_value_col = p->as_str();
        }

        if (auto* p = obj_get(o, "missing_tokens")) {
            if (!parse_string_array(*p, tmp.missing_tokens, err, "missing_tokens")) return false;
        }

        if (auto* p = obj_get(o, "missing_default")) {
            if (!parse_missing_policy(*p, tmp.missing_default, err)) return false;
        }

        if (auto* p = obj_get(o, "sort_by_time")) {
            if (!p->is_bool()) { err = "sort_by_time must be bool"; return false; }
            tmp.sort_by_time = p->as_bool();
        }

        if (auto* p = obj_get(o, "dedupe")) {
            if (!p->is_str()) { err = "dedupe must be string"; return false; }
            if (!parse_dedupe(p->as_str(), tmp.dedupe)) {
                err = "dedupe must be one of Error|First|Last";
                return false;
            }
        }

        if (auto* p = obj_get(o, "alignment_calendar")) {
            if (!p->is_str()) {
                err = "alignment_calendar must be one of Intersection|Union";
                return false;
            }
            if (!parse_alignment_calendar_policy(p->as_str(), tmp.alignment_calendar)) {
                err = "alignment_calendar must be one of Intersection|Union";
                return false;
            }
        }

        if (auto* p = obj_get(o, "alignment_non_temporal_multi_series")) {
            if (!p->is_str()) {
                err = "alignment_non_temporal_multi_series must be one of Reject|EqualLengthRowDomain";
                return false;
            }
            if (!parse_non_temporal_multi_series_policy(
                    p->as_str(),
                    tmp.alignment_non_temporal_multi_series)) {
                err = "alignment_non_temporal_multi_series must be one of Reject|EqualLengthRowDomain";
                return false;
            }
        }

        if (tmp.input_profile != InputProfile::Auto) {
            const DataSpec prof = profile_defaults(tmp.input_profile);
            if (tmp.time_col.empty()) tmp.time_col = prof.time_col;
            if (!has_time_kind) tmp.time_kind = prof.time_kind;
            if (tmp.value_cols.empty()) tmp.value_cols = prof.value_cols;
            if (tmp.primary_value_col.empty()) tmp.primary_value_col = prof.primary_value_col;
            if (tmp.rename.empty()) tmp.rename = prof.rename;
        }

        const bool has_time_axis = data_spec_has_time_axis(tmp);
        const bool has_spatial_axis = data_spec_has_spatial_axis(tmp);

        if (!has_time_axis && !has_spatial_axis) {
            err = "DataSpec must define at least one observation axis (time_col or spatial_x_col/spatial_y_col)";
            return false;
        }
        if (!has_time_axis && has_time_kind) {
            err = "time_kind cannot be set when time_col is empty";
            return false;
        }
        if (has_time_axis && !has_time_kind && tmp.input_profile == InputProfile::Auto) {
            err = "time_kind (string) is required";
            return false;
        }
        if (tmp.value_cols.empty()) {
            err = "value_cols (array<string>) is required";
            return false;
        }
        if (tmp.primary_value_col.empty()) {
            err = "primary_value_col (string) is required";
            return false;
        }

        if (tmp.header_policy == HeaderPolicy::HeaderlessPositional && tmp.columns_by_index.empty()) {
            err = "columns_by_index is required when header_policy=HeaderlessPositional";
            return false;
        }

        out = std::move(tmp);
        return true;
    }

    bool parse_data_spec_json(std::string_view json, DataSpec& out, std::string& err) {
        using namespace sugar::json_min;

        Value root;
        ParseError perr;
        if (!parse(json, root, perr)) {
            err = perr.message + " (line " + std::to_string(perr.line) + ", col " + std::to_string(perr.col) + ")";
            return false;
        }
        return parse_data_spec_value(root, out, err);
    }

    bool make_data_spec_profile_defaults(InputProfile profile, DataSpec& out, std::string& err) {
        if (profile == InputProfile::Auto) {
            err = "profile defaults require an explicit input_profile";
            return false;
        }

        out = profile_defaults(profile);
        return true;
    }

    bool load_data_spec_file(const std::string& path, DataSpec& out, std::string& err) {
        std::ifstream in(path);
        if (!in) {
            err = "Failed to open file: " + path;
            return false;
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        return parse_data_spec_json(ss.str(), out, err);
    }

} // namespace sugar
