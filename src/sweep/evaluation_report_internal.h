#pragma once

#include <iosfwd>
#include <string>
#include <string_view>
#include <vector>

#include "sweep/evaluation_report.h"

namespace sugar::evaluation_report_internal {

    const char* sweep_engine_name(PlanSweepEngineKind kind);

    std::string param_value_to_text(const ParamValue& value);

    bool run_provenance_has_data(const RunProvenanceSummary& provenance);

    std::vector<SweepCandidateReport> sweep_candidate_reports_for(
        const SweepReport& report);

    void print_text_evaluation_report(
        const EvaluationReport& report,
        EvaluationReportProjection projection,
        std::ostream& out);

    void print_sweep_geometry(
        const SweepReport& report,
        std::ostream& out);

    void append_json_string(std::ostream& out, std::string_view text);

    void append_json_number(std::ostream& out, double value);

    void append_json_string_array(
        std::ostream& out,
        const std::vector<std::string>& values);

    void append_json_size_array(
        std::ostream& out,
        const std::vector<std::size_t>& values);

    void append_json_parameter_settings(
        std::ostream& out,
        const std::vector<SweepParameterSetting>& settings);

    void append_json_sweep_geometry(
        std::ostream& out,
        const SweepSearchGeometrySummary& geometry);

    void append_json_evaluation_report(
        std::ostream& out,
        const EvaluationReport& report);

} // namespace sugar::evaluation_report_internal
