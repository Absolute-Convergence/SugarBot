#pragma once

#include <iosfwd>

#include "app_cli.h"

namespace sugar::app {

    int run_interactive_command(const AppCommand& cmd,
        std::istream& in,
        std::ostream& out,
        std::ostream& err);

} // namespace sugar::app
