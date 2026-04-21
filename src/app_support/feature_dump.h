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

} // namespace sugar::app
