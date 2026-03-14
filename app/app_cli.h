#pragma once

#include <cstddef>
#include <string>

#include "align.h"
#include "contract_suite.h"
#include "data_spec.h"

namespace sugar::app {

    enum class CommandKind {
        DumpOps,
        RunContractSuite,
        PrepDump,
        AlignDump,
        RunPanel,
        Interactive,
    };

    struct PrepDumpCommand {
        std::string spec_path{};
        std::string csv_path{};
    };

    struct AlignDumpCommand {
        std::string spec_path{};
        std::string csv_path{};
        std::string series_key_col{};
        align::CalendarPolicy calendar{ align::CalendarPolicy::Intersection };
    };

    struct RunPanelCommand {
        std::string spec_path{};
        std::string csv_path{};
        std::string primary_col{};
        std::size_t topk{ 10 };
    };

    struct InteractiveCommand {
        bool has_runtime_data_spec{ false };
        std::string runtime_data_spec_path{};
        InputProfile requested_format{ InputProfile::Auto };
        std::string csv_path{};
    };

    struct AppCommand {
        CommandKind kind{ CommandKind::Interactive };
        ContractSuiteOptions contract_suite{};
        PrepDumpCommand prep_dump{};
        AlignDumpCommand align_dump{};
        RunPanelCommand run_panel{};
        InteractiveCommand interactive{};
    };

    bool parse_app_command(int argc, char** argv, AppCommand& out, std::string& err);

} // namespace sugar::app
