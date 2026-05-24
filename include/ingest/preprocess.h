#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ingest/data_spec.h"
#include "ingest/numeric_frame.h"
#include "ingest/raw_table.h"

namespace sugar {

    struct ValueColumnQuality {
        std::string name{};
        std::size_t total_values{ 0 };
        std::size_t finite_values{ 0 };
        std::size_t missing_values{ 0 };
        bool has_nonzero_finite{ false };
    };

    struct PreprocessIssueSample {
        std::size_t csv_data_row{ 0 };
        std::string column{};
        std::string token{};
        std::string category{};
        std::string entity{};
        std::string key{};
    };

    struct PreprocessIssueAggregate {
        std::string code{};
        std::string column{};
        std::string category{};
        std::string entity{};
        std::string key{};
        std::size_t count{ 0 };
        std::vector<PreprocessIssueSample> samples{};
    };

    struct PreprocessResult {
        std::optional<NumericFrame> frame;
        std::optional<FrameSet> frameset;
        std::string err;
        std::string error_code{};
        std::vector<PreprocessIssueAggregate> error_aggregates{};
        CsvLoadDiagnostics csv_load{};
        bool csv_had_header{ false };
        std::vector<std::string> parsed_columns{};
        std::size_t dropped_row_count{ 0 };
        std::vector<ValueColumnQuality> value_column_quality{};
        std::vector<std::string> warnings{};
    };

    PreprocessResult preprocess_csv(const DataSpec& spec,
        const std::string& csv_path,
        std::shared_ptr<FrameSchema> schema);

} // namespace sugar
