#pragma once

#include <cstddef>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "sweep/metrics.h"
#include "sweep/plan_spec.h"
#include "sweep/plan_sweep.h"
#include "ingest/data_view.h"
#include "ingest/numeric_frame.h"

namespace sugar {

    enum class PanelFailureKind {
        MissingPrimaryColumn = 0,
        ExecutionFailed = 1,
    };

    std::string_view panel_failure_kind_name(PanelFailureKind kind);

    struct PanelRankingSpec {
        RankingObjectiveSpec objective{};
    };

    struct PanelEntityResult {
        std::string series_key;
        std::string entity_key;
        BacktestResult metrics;
        double ranking_score{ 0.0 };
    };

    struct PanelEntityFailure {
        std::string series_key;
        std::string entity_key;
        PanelFailureKind kind{ PanelFailureKind::ExecutionFailed };
        std::string reason;
    };

    struct PanelResult {
        PanelRankingSpec ranking{};
        std::vector<PanelEntityResult> topk;
        std::vector<PanelEntityFailure> failures;
        std::size_t entities_total{ 0 };
        std::size_t entities_ok{ 0 };
        std::size_t entities_failed{ 0 };
        bool canceled{ false };
        std::string cancel_message{};
    };

    struct PanelEntityReport {
        std::string series_key{};
        std::string entity_key{};
        double ranking_score{ 0.0 };
        RunReport run{};
    };

    struct PanelFailureReport {
        std::string series_key{};
        std::string entity_key{};
        PanelFailureKind kind{ PanelFailureKind::ExecutionFailed };
        std::string reason{};
    };

    struct PanelReport {
        PanelRankingSpec requested_ranking{};
        RankingObjectiveSpec resolved_objective{};
        RunProvenanceSummary run_provenance{};
        std::vector<PanelEntityReport> top_rows{};
        std::vector<PanelFailureReport> failures{};
        std::size_t entities_total{ 0 };
        std::size_t entities_ok{ 0 };
        std::size_t entities_failed{ 0 };
        bool canceled{ false };
        std::string cancel_message{};
    };

    bool build_panel_ranking_spec(
        const PlanRunSpec& run,
        PanelRankingSpec& out,
        std::string& err);

    RankingObjectiveSpec build_panel_ranking_objective(const PanelRankingSpec& ranking);

    PanelReport build_panel_report(
        const PanelResult& result,
        const RunProvenanceSummary* provenance = nullptr);

    void print_panel_report_summary(const PanelReport& report, std::ostream& out);

    PanelResult run_ranked_multi_entity_custom_planspec_over_dataview(
        const DataView& dv,
        const PlanSpec& spec,
        const PlanSweepRuntimeOptions& opt,
        ColumnId primary_col_id,
        std::size_t topk,
        const PanelRankingSpec& ranking);

    PanelResult run_panel_custom_planspec_over_dataview(
        const DataView& dv,
        const PlanSpec& spec,
        const PlanSweepRuntimeOptions& opt,
        ColumnId primary_col_id,
        std::size_t topk,
        double dd_weight = 0.25);

} // namespace sugar
