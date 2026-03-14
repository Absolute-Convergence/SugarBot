#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace sugar {

    enum class HeaderPolicy {
        RequiredHeaders,
        OptionalHeaders,
        HeaderlessPositional,
    };

    enum class TimeKind {
        YyyyMmDd,
        Iso8601,
        YearQuarter,
        YearMonth,
        Index,
    };

    enum class InputProfile {
        Auto,
        LegacyOhlcvDate,
        IsoOhlcvTime,
    };

    enum class MissingKind {
        DropRow,
        NaN,
        Ffill,
        Const,
    };

    struct MissingPolicy {
        MissingKind kind{ MissingKind::NaN };
        double const_value{ 0.0 };
    };

    struct DataSpec {
        enum class DedupePolicy {
            Error,
            First,
            Last,
        };

        std::string time_col;
        InputProfile input_profile{ InputProfile::Auto };
        HeaderPolicy header_policy{ HeaderPolicy::RequiredHeaders };
        // Positional mapping used when headers are optional/absent.
        // Keys must be canonical column names used by DataSpec fields.
        std::unordered_map<std::string, std::size_t> columns_by_index;
        TimeKind time_kind{ TimeKind::YyyyMmDd };
        std::optional<std::string> entity_col;
        std::optional<std::string> spatial_x_col;
        std::optional<std::string> spatial_y_col;
        std::vector<std::string> value_cols;
        std::unordered_map<std::string, std::string> rename;
        std::string primary_value_col;
        std::vector<std::string> missing_tokens;
        MissingPolicy missing_default{};
        bool sort_by_time{ true };
        DedupePolicy dedupe{ DedupePolicy::Error };

        // TODO(data-spec): add resampling/aggregation policy (no behavior yet).
    };

    bool data_spec_has_time_axis(const DataSpec& spec);
    bool data_spec_has_spatial_axis(const DataSpec& spec);

    bool load_data_spec_file(const std::string& path, DataSpec& out, std::string& err);
    bool parse_data_spec_json(std::string_view json, DataSpec& out, std::string& err);
    bool make_data_spec_profile_defaults(InputProfile profile, DataSpec& out, std::string& err);

    /*
    Example DataSpec JSON:

    {
      "input_profile": "auto",
      "time_col": "date",
      "header_policy": "RequiredHeaders",
      "time_kind": "YyyyMmDd",
      "entity_col": "symbol",
      "spatial_x_col": "x",
      "spatial_y_col": "y",
      "columns_by_index": {
        "date": 0,
        "symbol": 1,
        "open": 2,
        "high": 3,
        "low": 4,
        "close": 5,
        "volume": 6
      },
      "rename": {
        "Date": "date",
        "Adj Close": "close",
        "Volume": "volume"
      },
      "value_cols": ["open", "high", "low", "close", "volume"],
      "primary_value_col": "close",
      "missing_tokens": ["", "NA", "null"],
      "missing_default": { "kind": "NaN" },
      "sort_by_time": true,
      "dedupe": "Last"
    }

    Headerless positional example:

    {
      "input_profile": "legacy_ohlcv_date",
      "time_col": "t",
      "header_policy": "HeaderlessPositional",
      "columns_by_index": {
        "t": 0,
        "open": 1,
        "high": 2,
        "low": 3,
        "close": 4,
        "volume": 5
      },
      "time_kind": "Iso8601",
      "value_cols": ["open", "high", "low", "close", "volume"],
      "primary_value_col": "close",
      "missing_default": { "kind": "NaN" },
      "sort_by_time": true,
      "dedupe": "Error"
    }
    */

} // namespace sugar
