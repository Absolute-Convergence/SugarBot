#pragma once

#include <iosfwd>
#include <string>
#include <vector>

namespace sugar {

    struct ContractSuiteOptions {
        // Empty => run all registered sections.
        std::vector<std::string> sections;
    };

    // Runs deterministic platform-contract checks and prints a compact report.
    // Returns true if all selected checks pass.
    bool run_contract_suite(std::ostream& out, const ContractSuiteOptions& options);

    inline bool run_contract_suite(std::ostream& out) {
        return run_contract_suite(out, ContractSuiteOptions{});
    }

} // namespace sugar
