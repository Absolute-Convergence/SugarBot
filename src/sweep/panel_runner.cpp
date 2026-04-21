#include "sweep/panel_runner.h"

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

#include "ingest/data_view.h"
#include "sweep/plan_sweep.h"

namespace sugar {

    namespace {
        double compute_panel_ranking_score(
            const BacktestResult& result,
            const PanelRankingSpec& ranking) {
            switch (ranking.metric) {
            case PanelRankingMetric::Pnl:
                return result.pnl;
            case PanelRankingMetric::PnlDd:
                return result.pnl - ranking.dd_weight * result.max_drawdown;
            }
            return result.pnl;
        }
    } // namespace

    std::string_view panel_ranking_metric_name(PanelRankingMetric metric) {
        switch (metric) {
        case PanelRankingMetric::Pnl:
            return "pnl";
        case PanelRankingMetric::PnlDd:
            return "pnl_dd";
        }
        return "unknown";
    }

    PanelResult run_ranked_multi_entity_custom_planspec_over_dataview(
        const DataView& dv,
        const PlanSpec& spec,
        const PlanSweepRuntimeOptions& opt,
        ColumnId primary_col_id,
        std::size_t topk,
        const PanelRankingSpec& ranking)
    {
        PanelResult out{};
        out.ranking = ranking;

        const auto& b = dv.bundle();
        out.entities_total = b.series_keys.size();

        std::vector<std::string> keys = b.series_keys;
        std::sort(keys.begin(), keys.end());

        std::vector<PanelEntityResult> all_ok;
        all_ok.reserve(keys.size());

        for (const auto& k : keys) {
            if (opt.cancel_requested && opt.cancel_requested()) {
                out.canceled = true;
                out.cancel_message = "panel execution canceled";
                break;
            }
            const std::size_t series_index = b.find_series_index(k);
            const std::string entity_key =
                (series_index != static_cast<std::size_t>(-1))
                ? std::string(b.entity_key_for_series_index(series_index))
                : k;

            const ColumnRef primary{ k, primary_col_id };
            if (!dv.has_col(primary)) {
                out.failures.push_back({ k, entity_key, "primary column missing in DataView series" });
                continue;
            }

            BacktestResult result{};
            std::ostringstream run_log;
            if (!run_custom_plan_spec(dv, primary, spec, opt, run_log, &result)) {
                std::string reason = run_log.str();
                if (reason.empty()) reason = "custom PlanSpec execution failed";
                if (opt.cancel_requested && opt.cancel_requested()) {
                    out.canceled = true;
                    out.cancel_message = reason;
                    break;
                }
                out.failures.push_back({ k, entity_key, reason });
                continue;
            }

            if (opt.cancel_requested && opt.cancel_requested()) {
                out.canceled = true;
                out.cancel_message = "panel execution canceled";
                break;
            }
            const double ranking_score = compute_panel_ranking_score(result, ranking);
            all_ok.push_back({ k, entity_key, result, ranking.metric, ranking_score });
        }

        out.entities_ok = all_ok.size();
        out.entities_failed = out.failures.size();

        std::sort(all_ok.begin(), all_ok.end(),
            [](const PanelEntityResult& a, const PanelEntityResult& b) {
                if (a.ranking_score != b.ranking_score) return a.ranking_score > b.ranking_score;
                if (a.entity_key != b.entity_key) return a.entity_key < b.entity_key;
                return a.series_key < b.series_key;
            });

        const std::size_t keep = std::min<std::size_t>(topk, all_ok.size());
        out.topk.assign(all_ok.begin(), all_ok.begin() + keep);
        return out;
    }

    PanelResult run_panel_custom_planspec_over_dataview(
        const DataView& dv,
        const PlanSpec& spec,
        const PlanSweepRuntimeOptions& opt,
        ColumnId primary_col_id,
        std::size_t topk,
        double dd_weight)
    {
        PanelRankingSpec ranking{};
        ranking.metric = PanelRankingMetric::PnlDd;
        ranking.dd_weight = dd_weight;
        return run_ranked_multi_entity_custom_planspec_over_dataview(
            dv,
            spec,
            opt,
            primary_col_id,
            topk,
            ranking);
    }

} // namespace sugar
