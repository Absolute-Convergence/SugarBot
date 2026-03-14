#pragma once

#include <string>

#include "compiled_program.h"
#include "data_view.h"
#include "strategy_plan.h"

namespace sugar {

    // Per-run evaluator outputs:
    // - buffers are materialized over the active DataView row domain
    // - caller owns the buffers after evaluation returns
    // - missing allow_trade binding defaults to true; other missing bindings default to false
    struct ProgramSignalOutputs {
        BoolBuffer entry_long{};
        BoolBuffer exit_long{};
        BoolBuffer entry_short{};
        BoolBuffer exit_short{};
        BoolBuffer allow_trade{};
    };

    bool evaluate_strategy_plan_program(
        const DataView& dv,
        const ColumnRef& primary,
        const StrategyPlan& plan,
        ProgramSignalOutputs& out,
        std::string& err);

    bool compiled_program_supports_numeric_op(const std::string& op_name);
    bool compiled_program_supports_bool_op(const std::string& op_name);

} // namespace sugar
