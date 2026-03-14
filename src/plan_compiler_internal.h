#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "compiled_program.h"
#include "op_registry.h"
#include "plan_compiler.h"

namespace sugar {

    struct NumericProgramTemplateNode {
        std::string node_id{};
        OpEntry op{};
        std::vector<ProgramInputRef> inputs{};
        ParamMap node_params{};
        std::size_t output_slot{ 0 };
    };

    struct BoolProgramTemplateNode {
        std::string node_id{};
        OpEntry op{};
        std::vector<ProgramInputRef> inputs{};
        ParamMap node_params{};
        std::size_t output_slot{ 0 };
    };

    struct CompiledPlanTemplate {
        std::string name{};
        TradeMode mode{ TradeMode::LongShort };
        ExecSettings exec{};
        ParamMap plan_params{};
        CompileEnv env{};
        std::vector<NumericProgramTemplateNode> numeric_nodes{};
        std::vector<BoolProgramTemplateNode> bool_nodes{};
        CompiledProgram program{};
    };

    bool compile_plan_template(const PlanSpec& spec,
        const OperatorRegistry& registry,
        const CompileEnv& env,
        CompiledPlanTemplate& out,
        std::string& err);

    bool instantiate_compiled_plan(const CompiledPlanTemplate& plan_template,
        const ParamMap* overrides,
        CompileResult& out,
        std::string& err);

} // namespace sugar
