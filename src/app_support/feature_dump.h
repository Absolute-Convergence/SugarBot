#pragma once

#include <iosfwd>
#include <string>

#include "ingest/run_pipeline.h"
#include "sweep/plan_spec.h"

namespace sugar::app {

    bool dump_feature_table(
        const sugar::ResolvedRuntimeInput& runtime,
        const sugar::PlanSpec& spec,
        std::ostream& out,
        std::string& err);

    bool dump_feature_table_parquet(
        const sugar::ResolvedRuntimeInput& runtime,
        const sugar::PlanSpec& spec,
        const std::string& output_path,
        std::string& err);

} // namespace sugar::app
