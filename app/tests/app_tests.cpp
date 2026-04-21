#include "tests/app_test_support.h"

#include <ios>
#include <ostream>

namespace sugar::app::tests {

    bool run_all(std::ostream& out) {
        out << std::unitbuf;
        out << "=== SugarBot App Tests ===\n";
        TestContext t{ out };

        AppTestRegistry tests{};
        register_cli_tests(tests);
        register_runtime_tests(tests);
        register_command_tests(tests);
        register_interactive_tests(tests);
        register_run_tests(tests);

        for (const auto test : tests) {
            test(t);
        }

        out << "=== App Test Summary ===\n";
        out << "Passed: " << t.passed << "\n";
        out << "Failed: " << t.failed << "\n";
        return t.failed == 0;
    }

} // namespace sugar::app::tests
