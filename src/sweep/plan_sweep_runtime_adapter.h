#pragma once

#include <string>
#include <vector>

#include "ingest/data_view.h"
#include "plan_compiler_internal.h"
#include "sweep/plan_sweep.h"

namespace sugar {

    struct CompiledPlanRuntimeTemplate {
        CompiledPlanTemplate plan_template{};
        std::vector<NamedNumericOutput> numeric_outputs{};
    };

    bool build_compiled_plan_runtime_template(
        const PlanSpec& spec,
        const DataView& dv,
        const std::string& default_series,
        bool collect_numeric_outputs,
        CompiledPlanRuntimeTemplate& out,
        std::string& err_out);

    bool run_instantiated_plan_once(
        const CompiledPlanRuntimeTemplate& runtime_template,
        const ParamMap* overrides,
        const DataView& dv,
        const std::string& default_series,
        ColumnId primary_col_id,
        const PlanSweepRuntimeOptions& opt,
        BacktestResult& result_out,
        std::string& err_out);

} // namespace sugar
