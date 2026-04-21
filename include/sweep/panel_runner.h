#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "sweep/metrics.h"
#include "sweep/plan_spec.h"
#include "sweep/plan_sweep.h"
#include "ingest/data_view.h"
#include "ingest/numeric_frame.h"

namespace sugar {

    enum class PanelRankingMetric {
        Pnl = 0,
        PnlDd = 1,
    };

    std::string_view panel_ranking_metric_name(PanelRankingMetric metric);

    struct PanelRankingSpec {
        PanelRankingMetric metric{ PanelRankingMetric::PnlDd };
        double dd_weight{ 0.25 };
    };

    struct PanelEntityResult {
        std::string series_key;
        std::string entity_key;
        BacktestResult metrics;
        PanelRankingMetric ranking_metric{ PanelRankingMetric::PnlDd };
        double ranking_score{ 0.0 };
    };

    struct PanelEntityFailure {
        std::string series_key;
        std::string entity_key;
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
