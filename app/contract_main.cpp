#include <iostream>
#include <string_view>

#include "contract_suite.h"

int main(int argc, char** argv) {
    sugar::ContractSuiteOptions options{};
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) != "--contract-section") continue;
        if (i + 1 >= argc) {
            std::cerr << "Error: --contract-section requires <key>\n";
            return 1;
        }
        options.sections.emplace_back(argv[++i]);
    }

    const bool ok = sugar::run_contract_suite(std::cout, options);
    return ok ? 0 : 1;
}
