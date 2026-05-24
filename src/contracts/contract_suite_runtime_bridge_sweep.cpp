#include "contract_suite_runtime_bridge_sweep.h"

namespace sugar::contract_suite_internal {

    bool test_compile_bridge_sweep_runtime_contracts(ContractAsserts& t) {
        t.out << "[compile_bridge_sweep_runtime] compile-bridge sweep runtime contracts\n";

        if (!run_compile_bridge_sweep_grid_contracts(t)) return true;
        if (!run_compile_bridge_sweep_report_contracts(t)) return true;
        if (!run_compile_bridge_sweep_adaptive_contracts(t)) return true;
        if (!run_compile_bridge_sweep_pareto_contracts(t)) return true;
        if (!run_compile_bridge_sweep_posterior_contracts(t)) return true;
        if (!run_compile_bridge_sweep_stage_metric_contracts(t)) return true;

        return true;
    }

} // namespace sugar::contract_suite_internal
