#pragma once

#include <cstddef>
#include <string>

#include "ingest/align.h"
#include "contracts/contract_suite.h"
#include "ingest/data_spec.h"
#include "sweep/evaluation_report.h"

namespace sugar::app {

    enum class CommandKind {
        Help,
        Onramp,
        HelpDiagnostics,
        HelpReports,
        ScaffoldLayout,
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
        EvaluationReportProjection report_projection{ default_evaluation_report_projection() };
    };

    struct ObsReportCommand {
        std::string layout_path{};
        std::string csv_path{};
        bool json_output{ false };
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

    struct ScaffoldLayoutCommand {
        std::string csv_path{};
    };

    enum class DumpOpsSection {
        Full,
        Modules,
        Operations,
        Examples,
    };

    struct DumpOpsCommand {
        DumpOpsSection section{ DumpOpsSection::Full };
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
        ScaffoldLayoutCommand scaffold_layout{};
        ObsReportCommand obs_report{};
        PlanReportCommand plan_report{};
        FeatureDumpCommand feature_dump{};
        DumpOpsCommand dump_ops{};
        PrepDumpCommand prep_dump{};
        AlignDumpCommand align_dump{};
        RunPanelCommand run_panel{};
        InteractiveCommand interactive{};
    };

    bool parse_app_command(int argc, char** argv, AppCommand& out, std::string& err);

} // namespace sugar::app
