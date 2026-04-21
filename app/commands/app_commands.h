#pragma once

#include <iosfwd>

#include "cli/app_cli.h"

namespace sugar::app {

    int run_help_command(std::ostream& out);

    int run_obs_report_command(const AppCommand& cmd,
        std::ostream& out,
        std::ostream& err);

    int run_plan_report_command(const AppCommand& cmd,
        std::ostream& out,
        std::ostream& err);

    int run_feature_dump_command(const AppCommand& cmd,
        std::ostream& out,
        std::ostream& err);

    int run_dump_layout_command(std::ostream& out);

    int run_dump_plan_command(std::ostream& out);

    int run_unified_cli_command(const AppCommand& cmd,
        std::ostream& out,
        std::ostream& err);

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
