#include "plan_sweep_objective.h"

#include <utility>

namespace sugar {

    RankingObjectiveSpec resolve_plan_sweep_objective(const PlanSweepSpec& sweep) {
        if (sweep.objective_spec.has_value()) {
            return *sweep.objective_spec;
        }

        RankingObjectiveSpec objective{};
        objective.primary.source = ObjectiveValueSource::ReportedMetric;
        objective.primary.name = "pnl";
        objective.direction = ObjectiveDirection::Maximize;

        if (sweep.objective == "pnl") {
            return objective;
        }

        ObjectivePenaltySpec penalty{};
        penalty.value.source = ObjectiveValueSource::ReportedMetric;
        penalty.value.name = "max_drawdown";
        penalty.weight = sweep.dd_weight;
        objective.penalties.push_back(std::move(penalty));
        return objective;
    }

    bool plan_sweep_objective_needs_numeric_outputs(const RankingObjectiveSpec& objective) {
        if (objective.primary.source == ObjectiveValueSource::NumericNodeOutput) {
            return true;
        }
        for (const auto& penalty : objective.penalties) {
            if (penalty.value.source == ObjectiveValueSource::NumericNodeOutput) {
                return true;
            }
        }
        for (const auto& tie_break : objective.tie_breaks) {
            if (tie_break.value.source == ObjectiveValueSource::NumericNodeOutput) {
                return true;
            }
        }
        return false;
    }

} // namespace sugar
