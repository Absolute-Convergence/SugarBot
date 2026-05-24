#pragma once

#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "sweep/metrics.h"
#include "sweep/panel_runner.h"
#include "sweep/plan_sweep.h"

namespace sugar {

    enum class EvaluationReportKind {
        None = 0,
        Run = 1,
        Sweep = 2,
        Panel = 3,
        Ensemble = 4,
        DynamicDag = 5,
    };

    enum class EvaluationReportProjection {
        Full = 0,
        Compact = 1,
        CandidateTable = 2,
        ObjectiveBreakdown = 3,
        DiversityDiagnostics = 4,
        DuplicateTradePaths = 5,
        SearchGeometry = 6,
        ParetoFront = 7,
        PosteriorDiagnostics = 8,
        TopologyDiagnostics = 9,
        ReplayTrace = 10,
        Json = 11,
    };

    struct EvaluationReport {
        EvaluationReportKind kind{ EvaluationReportKind::None };
        std::optional<RunReport> run{};
        std::optional<SweepReport> sweep{};
        std::optional<PanelReport> panel{};
        std::vector<std::string> warnings{};
    };

    inline constexpr std::string_view evaluation_report_schema_version() noexcept {
        return "sugarbot.evaluation_report.v1";
    }

    inline constexpr EvaluationReportProjection default_evaluation_report_projection() noexcept {
        return EvaluationReportProjection::Full;
    }

    inline constexpr bool evaluation_report_projection_is_machine_readable(
        EvaluationReportProjection projection) noexcept {
        return projection == EvaluationReportProjection::Json;
    }

    std::string_view evaluation_report_kind_name(EvaluationReportKind kind);
    std::string_view evaluation_report_projection_name(EvaluationReportProjection projection);
    std::string_view evaluation_report_projection_usage();
    std::string_view evaluation_report_projection_error_list();
    bool parse_evaluation_report_projection(std::string_view text, EvaluationReportProjection& out);

    EvaluationReport make_run_evaluation_report(RunReport report);
    EvaluationReport make_sweep_evaluation_report(SweepReport report);
    EvaluationReport make_panel_evaluation_report(PanelReport report);

    void print_evaluation_report(
        const EvaluationReport& report,
        EvaluationReportProjection projection,
        std::ostream& out);

} // namespace sugar
