#pragma once

#include <cstddef>
#include <string>

#include "ingest/align.h"
#include "contracts/contract_suite.h"
#include "ingest/data_spec.h"

namespace sugar::app {

    enum class CommandKind {
        Help,
        Run,
        ObsReport,
        PlanReport,
        FeatureDump,
        DumpLayout,
        DumpPlan,
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

    struct RunCommand {
        std::string layout_path{};
        std::string logic_path{};
        std::string csv_path{};
    };

    struct ObsReportCommand {
        std::string layout_path{};
        std::string csv_path{};
    };

    struct PlanReportCommand {
        std::string layout_path{};
        std::string logic_path{};
        std::string csv_path{};
    };

    struct FeatureDumpCommand {
        std::string layout_path{};
        std::string logic_path{};
        std::string csv_path{};
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
        RunCommand run{};
        ObsReportCommand obs_report{};
        PlanReportCommand plan_report{};
        FeatureDumpCommand feature_dump{};
        PrepDumpCommand prep_dump{};
        AlignDumpCommand align_dump{};
        RunPanelCommand run_panel{};
        InteractiveCommand interactive{};
    };

    bool parse_app_command(int argc, char** argv, AppCommand& out, std::string& err);

} // namespace sugar::app
