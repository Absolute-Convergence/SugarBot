#pragma once

#include <string>
#include <vector>

#include "engine/program_evaluator.h"
#include "sweep/metrics.h"

namespace sugar {

    struct ProgramEvaluationBuffers {
        std::vector<NumericBuffer> numeric_slots{};
        std::vector<BoolBuffer> bool_slots{};
        ProgramSignalOutputs signals{};
        RunDynamicExecutionSummary dynamic_execution{};
    };

    bool evaluate_strategy_plan_program_buffers(
        const DataView& dv,
        const ColumnRef& primary,
        const StrategyPlan& plan,
        ProgramEvaluationBuffers& out,
        std::string& err);

} // namespace sugar
