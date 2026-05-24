#include "sweep/evaluation_report.h"

#include <algorithm>
#include <cctype>
#include <ostream>
#include <string>
#include <utility>

#include "evaluation_report_internal.h"

namespace sugar {

    namespace {
        std::string lower_copy(std::string_view text) {
            std::string out(text);
            std::transform(out.begin(), out.end(), out.begin(),
                [](unsigned char ch) {
                    return static_cast<char>(std::tolower(ch));
                });
            return out;
        }
    } // namespace

    std::string_view evaluation_report_kind_name(EvaluationReportKind kind) {
        switch (kind) {
        case EvaluationReportKind::None:
            return "none";
        case EvaluationReportKind::Run:
            return "run";
        case EvaluationReportKind::Sweep:
            return "sweep";
        case EvaluationReportKind::Panel:
            return "panel";
        case EvaluationReportKind::Ensemble:
            return "ensemble";
        case EvaluationReportKind::DynamicDag:
            return "dynamic_dag";
        }
        return "unknown";
    }

    std::string_view evaluation_report_projection_name(
        EvaluationReportProjection projection) {
        switch (projection) {
        case EvaluationReportProjection::Full:
            return "full";
        case EvaluationReportProjection::Compact:
            return "compact";
        case EvaluationReportProjection::CandidateTable:
            return "candidate_table";
        case EvaluationReportProjection::ObjectiveBreakdown:
            return "objective_breakdown";
        case EvaluationReportProjection::DiversityDiagnostics:
            return "diversity";
        case EvaluationReportProjection::DuplicateTradePaths:
            return "duplicate_trade_paths";
        case EvaluationReportProjection::SearchGeometry:
            return "search_geometry";
        case EvaluationReportProjection::ParetoFront:
            return "pareto";
        case EvaluationReportProjection::PosteriorDiagnostics:
            return "posterior";
        case EvaluationReportProjection::TopologyDiagnostics:
            return "topology";
        case EvaluationReportProjection::ReplayTrace:
            return "trace";
        case EvaluationReportProjection::Json:
            return "json";
        }
        return "unknown";
    }

    std::string_view evaluation_report_projection_usage() {
        return "full|compact|table|objective|diversity|duplicates|geometry|pareto|posterior|topology|trace|json";
    }

    std::string_view evaluation_report_projection_error_list() {
        return "full | compact | table | objective | diversity | duplicates | geometry | pareto | posterior | topology | trace | json";
    }

    bool parse_evaluation_report_projection(
        std::string_view text,
        EvaluationReportProjection& out) {
        const std::string value = lower_copy(text);
        if (value == "full" || value == "default") {
            out = EvaluationReportProjection::Full;
            return true;
        }
        if (value == "compact" || value == "summary") {
            out = EvaluationReportProjection::Compact;
            return true;
        }
        if (value == "candidate_table" || value == "candidates" || value == "table") {
            out = EvaluationReportProjection::CandidateTable;
            return true;
        }
        if (value == "objective_breakdown" ||
            value == "objective" ||
            value == "breakdown") {
            out = EvaluationReportProjection::ObjectiveBreakdown;
            return true;
        }
        if (value == "diversity" || value == "diversity_diagnostics") {
            out = EvaluationReportProjection::DiversityDiagnostics;
            return true;
        }
        if (value == "duplicate_trade_paths" ||
            value == "duplicates" ||
            value == "trade_paths") {
            out = EvaluationReportProjection::DuplicateTradePaths;
            return true;
        }
        if (value == "search_geometry" ||
            value == "geometry" ||
            value == "basins" ||
            value == "robustness") {
            out = EvaluationReportProjection::SearchGeometry;
            return true;
        }
        if (value == "pareto" ||
            value == "pareto_front" ||
            value == "front" ||
            value == "multi_objective") {
            out = EvaluationReportProjection::ParetoFront;
            return true;
        }
        if (value == "posterior" ||
            value == "posterior_diagnostics" ||
            value == "bayesian" ||
            value == "bayesian_posterior") {
            out = EvaluationReportProjection::PosteriorDiagnostics;
            return true;
        }
        if (value == "topology" ||
            value == "topology_diagnostics" ||
            value == "topologies" ||
            value == "neighborhoods" ||
            value == "neighborhood") {
            out = EvaluationReportProjection::TopologyDiagnostics;
            return true;
        }
        if (value == "trace" ||
            value == "replay" ||
            value == "replay_trace" ||
            value == "proposal_trace" ||
            value == "adaptive_replay") {
            out = EvaluationReportProjection::ReplayTrace;
            return true;
        }
        if (value == "json" || value == "raw_json" || value == "raw") {
            out = EvaluationReportProjection::Json;
            return true;
        }
        return false;
    }

    EvaluationReport make_run_evaluation_report(RunReport report) {
        EvaluationReport envelope{};
        envelope.kind = EvaluationReportKind::Run;
        envelope.run = std::move(report);
        return envelope;
    }

    EvaluationReport make_sweep_evaluation_report(SweepReport report) {
        EvaluationReport envelope{};
        envelope.kind = EvaluationReportKind::Sweep;
        envelope.sweep = std::move(report);
        return envelope;
    }

    EvaluationReport make_panel_evaluation_report(PanelReport report) {
        EvaluationReport envelope{};
        envelope.kind = EvaluationReportKind::Panel;
        envelope.panel = std::move(report);
        return envelope;
    }

    void print_evaluation_report(
        const EvaluationReport& report,
        EvaluationReportProjection projection,
        std::ostream& out) {
        if (evaluation_report_projection_is_machine_readable(projection)) {
            evaluation_report_internal::append_json_evaluation_report(out, report);
            out << "\n";
            return;
        }

        evaluation_report_internal::print_text_evaluation_report(
            report,
            projection,
            out);
    }

} // namespace sugar
