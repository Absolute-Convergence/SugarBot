#pragma once

#include <vector>

#include "sweep/plan_sweep.h"
#include "sweep/sweep.h"

namespace sugar {

    SweepSearchGeometrySummary build_plan_sweep_search_geometry(
        const std::vector<IndexedCandidate>& retained_pool,
        const std::vector<IndexedCandidate>& returned_candidates,
        const PlanSpecSweepDims& dims,
        const PlanSweepSpec& sweep);

} // namespace sugar
