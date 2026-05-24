#include "evaluation_report_internal.h"

#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace sugar::evaluation_report_internal {

    const char* sweep_engine_name(PlanSweepEngineKind kind) {
        switch (kind) {
        case PlanSweepEngineKind::CoarseFine: return "coarse_fine";
        case PlanSweepEngineKind::MonteCarlo: return "monte_carlo";
        case PlanSweepEngineKind::SuccessiveHalving: return "successive_halving";
        case PlanSweepEngineKind::BayesianPosterior: return "bayesian_posterior";
        case PlanSweepEngineKind::Exhaustive:
        default: return "exhaustive";
        }
    }

    std::string param_value_to_text(const ParamValue& value) {
        return std::visit([](const auto& current) -> std::string {
            using Current = std::decay_t<decltype(current)>;
            if constexpr (std::is_same_v<Current, std::nullptr_t>) {
                return "null";
            }
            else if constexpr (std::is_same_v<Current, bool>) {
                return current ? "true" : "false";
            }
            else if constexpr (std::is_same_v<Current, std::int64_t>) {
                return std::to_string(current);
            }
            else if constexpr (std::is_same_v<Current, double>) {
                std::ostringstream out;
                out << std::setprecision(12) << current;
                return out.str();
            }
            else if constexpr (std::is_same_v<Current, std::string>) {
                return current;
            }
            else {
                return std::string("ref:") + current.name;
            }
        }, value);
    }

    bool run_provenance_has_data(const RunProvenanceSummary& provenance) {
        return !provenance.primary_series_key.empty() ||
            !provenance.primary_selection_mode.empty() ||
            !provenance.time_kind.empty() ||
            !provenance.time_resolution.empty() ||
            provenance.series_count > 0 ||
            provenance.has_time_axis ||
            provenance.has_entity_axis ||
            provenance.has_spatial_axis ||
            provenance.input_warning_count > 0 ||
            provenance.csv_had_header ||
            provenance.csv_physical_line_count > 0 ||
            provenance.csv_blank_line_count > 0 ||
            provenance.csv_data_row_count > 0 ||
            provenance.dropped_row_count > 0 ||
            !provenance.input_warnings.empty() ||
            provenance.alignment_applied ||
            !provenance.alignment_calendar.empty() ||
            !provenance.alignment_missing.empty() ||
            !provenance.alignment_non_temporal_multi_series.empty() ||
            !provenance.topology_names.empty() ||
            provenance.topology_usage_count > 0 ||
            !provenance.topologies.empty() ||
            !provenance.topology_usages.empty();
    }

    std::vector<SweepCandidateReport> sweep_candidate_reports_for(
        const SweepReport& report) {
        if (!report.candidate_reports.empty()) {
            return report.candidate_reports;
        }
        return build_sweep_candidate_reports(
            report.top_candidates,
            &report.resolved_objective,
            &report.run_provenance);
    }

} // namespace sugar::evaluation_report_internal
