#pragma once

#include <memory>
#include <optional>
#include <string>

#include "data_spec.h"
#include "numeric_frame.h"

namespace sugar {

    struct PreprocessResult {
        std::optional<NumericFrame> frame;
        std::optional<FrameSet> frameset;
        std::string err;
    };

    PreprocessResult preprocess_csv(const DataSpec& spec,
        const std::string& csv_path,
        std::shared_ptr<FrameSchema> schema);

} // namespace sugar

