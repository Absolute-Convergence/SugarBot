#include <exception>
#include <iostream>
#include <string>

#include "cli/app_cli.h"
#include "commands/app_commands.h"
#include "interactive/app_interactive.h"

int main(int argc, char** argv) {
    try {
        sugar::app::AppCommand cmd{};
        std::string err;
        if (!sugar::app::parse_app_command(argc, argv, cmd, err)) {
            std::cerr << "Error: " << err << "\n";
            return 1;
        }

        switch (cmd.kind) {
        case sugar::app::CommandKind::Help:
            return sugar::app::run_help_command(std::cout);
        case sugar::app::CommandKind::Run:
            return sugar::app::run_unified_cli_command(cmd, std::cout, std::cerr);
        case sugar::app::CommandKind::ObsReport:
            return sugar::app::run_obs_report_command(cmd, std::cout, std::cerr);
        case sugar::app::CommandKind::PlanReport:
            return sugar::app::run_plan_report_command(cmd, std::cout, std::cerr);
        case sugar::app::CommandKind::FeatureDump:
            return sugar::app::run_feature_dump_command(cmd, std::cout, std::cerr);
        case sugar::app::CommandKind::DumpLayout:
            return sugar::app::run_dump_layout_command(std::cout);
        case sugar::app::CommandKind::DumpPlan:
            return sugar::app::run_dump_plan_command(std::cout);
        case sugar::app::CommandKind::DumpOps:
            return sugar::app::run_dump_ops_command(cmd, std::cout);
        case sugar::app::CommandKind::RunContractSuite:
            return sugar::app::run_contract_suite_command(cmd, std::cout, std::cerr);
        case sugar::app::CommandKind::PrepDump:
            return sugar::app::run_prep_dump_command(cmd, std::cout, std::cerr);
        case sugar::app::CommandKind::AlignDump:
            return sugar::app::run_align_dump_command(cmd, std::cout, std::cerr);
        case sugar::app::CommandKind::RunPanel:
            return sugar::app::run_panel_command(cmd, std::cin, std::cout, std::cerr);
        case sugar::app::CommandKind::Interactive:
        default:
            return sugar::app::run_interactive_command(cmd, std::cin, std::cout, std::cerr);
        }
    }
    catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << "\n";
        return 1;
    }
}
