#pragma once

#include <cstddef>
#include <memory>

#include "compiled_program.h"
#include "strategy_plan.h"

namespace sugar {

    struct CompileEnvStorage;

    struct StrategyPlanStorage {
        std::shared_ptr<const CompileEnvStorage> env{};
        CompiledProgram program{};
    };

} // namespace sugar
