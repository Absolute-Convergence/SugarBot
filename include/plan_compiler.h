#pragma once

#include <string>

#include "plan_spec.h"
#include "op_registry.h"

namespace sugar {

    struct CompileResult {
        StrategyPlan plan;
        ExecSettings exec; // from spec
    };

    bool compile_plan(const PlanSpec& spec,
        const OperatorRegistry& registry,
        const CompileEnv& env,
        CompileResult& out,
        std::string& err);

} // namespace sugar
