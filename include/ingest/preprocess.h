#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ingest/data_spec.h"
#include "ingest/numeric_frame.h"
#include "ingest/raw_table.h"

namespace sugar {

    struct PreprocessResult {
        std::optional<NumericFrame> frame;
        std::optional<FrameSet> frameset;
        std::string err;
        CsvLoadDiagnostics csv_load{};
        bool csv_had_header{ false };
        std::size_t dropped_row_count{ 0 };
        std::vector<std::string> warnings{};
    };

    PreprocessResult preprocess_csv(const DataSpec& spec,
        const std::string& csv_path,
        std::shared_ptr<FrameSchema> schema);

} // namespace sugar
