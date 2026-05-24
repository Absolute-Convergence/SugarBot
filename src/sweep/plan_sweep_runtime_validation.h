#pragma once

#include <string>

#include "ingest/data_view.h"
#include "sweep/plan_spec.h"

namespace sugar {

    void normalize_plan_sweep_runtime_aliases(PlanSpec& spec);

    bool validate_planspec_runtime_requirements_for_dataview(
        const PlanSpec& spec,
        const DataView& dv,
        const std::string& default_series,
        const ParamMap* overrides,
        std::string& err);

} // namespace sugar
