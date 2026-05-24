#pragma once

#include "sweep/metrics.h"
#include "sweep/plan_spec.h"

namespace sugar {

    RankingObjectiveSpec resolve_plan_sweep_objective(const PlanSweepSpec& sweep);

    bool plan_sweep_objective_needs_numeric_outputs(const RankingObjectiveSpec& objective);

} // namespace sugar
