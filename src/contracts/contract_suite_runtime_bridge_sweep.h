#pragma once

#include "contract_suite_internal.h"

namespace sugar::contract_suite_internal {

    bool run_compile_bridge_sweep_adaptive_contracts(ContractAsserts& t);
    bool run_compile_bridge_sweep_grid_contracts(ContractAsserts& t);
    bool run_compile_bridge_sweep_report_contracts(ContractAsserts& t);
    bool run_compile_bridge_sweep_pareto_contracts(ContractAsserts& t);
    bool run_compile_bridge_sweep_posterior_contracts(ContractAsserts& t);
    bool run_compile_bridge_sweep_stage_metric_contracts(ContractAsserts& t);

} // namespace sugar::contract_suite_internal
