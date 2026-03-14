#pragma once

#include <iosfwd>

#include "app_cli.h"

namespace sugar::app {

    int run_dump_ops_command(std::ostream& out);

    int run_contract_suite_command(const AppCommand& cmd,
        std::ostream& out,
        std::ostream& err);

    int run_prep_dump_command(const AppCommand& cmd,
        std::ostream& out,
        std::ostream& err);

    int run_align_dump_command(const AppCommand& cmd,
        std::ostream& out,
        std::ostream& err);

    int run_panel_command(const AppCommand& cmd,
        std::istream& in,
        std::ostream& out,
        std::ostream& err);

} // namespace sugar::app
