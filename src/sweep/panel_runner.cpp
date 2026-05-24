#include "sweep/panel_runner.h"

#include <algorithm>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

#include "ingest/data_view.h"
#include "sweep/plan_sweep.h"

namespace sugar {

    namespace {
        RankingObjectiveSpec make_reported_metric_objective(std::string name) {
            RankingObjectiveSpec objective{};
            objective.primary.source = ObjectiveValueSource::ReportedMetric;
            objective.primary.name = std::move(name);
            objective.direction = ObjectiveDirection::Maximize;
            return objective;
        }

        RankingObjectiveSpec make_drawdown_weighted_pnl_objective(double dd_weight) {
            RankingObjectiveSpec objective = make_reported_metric_objective("pnl");
            ObjectivePenaltySpec penalty{};
            penalty.value.source = ObjectiveValueSource::ReportedMetric;
            penalty.value.name = "max_drawdown";
            penalty.weight = dd_weight;
            objective.penalties.push_back(std::move(penalty));
            return objective;
        }

        double compute_panel_ranking_score(
            const BacktestResult& result,
            const PanelRankingSpec& ranking) {
            return evaluate_ranking_objective(build_panel_ranking_objective(ranking), result);
        }

        void print_panel_dynamic_execution_line(
            const RunDynamicExecutionSummary& dynamic,
            std::ostream& out) {
            if (!dynamic.enabled) {
                return;
            }
            out << "    dynamic_execution mode="
                << (dynamic.mode.empty() ? "<unknown>" : dynamic.mode)
                << " total_decisions=" << dynamic.total_decisions
                << " decision_samples=" << dynamic.emitted_decision_samples
                << "/" << dynamic.decision_sample_limit
                << " suppressed=" << dynamic.suppressed_decisions
                << " inactive_output_rows=" << dynamic.inactive_output_rows
                << " route_decision_digest=" << dynamic.route_decision_digest
                << "\n";
        }
    } // namespace

    std::string_view panel_failure_kind_name(PanelFailureKind kind) {
        switch (kind) {
        case PanelFailureKind::MissingPrimaryColumn:
            return "missing_primary_column";
        case PanelFailureKind::ExecutionFailed:
            return "execution_failed";
        }
        return "unknown";
    }

    bool build_panel_ranking_spec(
        const PlanRunSpec& run,
        PanelRankingSpec& out,
        std::string& err) {
        if (run.objective_spec.has_value()) {
            out.objective = *run.objective_spec;
            err.clear();
            return true;
        }
        out.objective = make_reported_metric_objective("pnl");
        err.clear();
        return true;
    }

    RankingObjectiveSpec build_panel_ranking_objective(const PanelRankingSpec& ranking) {
        return ranking.objective;
    }

    PanelReport build_panel_report(
        const PanelResult& result,
        const RunProvenanceSummary* provenance) {
        PanelReport report{};
        report.requested_ranking = result.ranking;
        report.resolved_objective = build_panel_ranking_objective(result.ranking);
        if (provenance != nullptr) {
            report.run_provenance = *provenance;
        }
        report.entities_total = result.entities_total;
        report.entities_ok = result.entities_ok;
        report.entities_failed = result.entities_failed;
        report.canceled = result.canceled;
        report.cancel_message = result.cancel_message;

        report.top_rows.reserve(result.topk.size());
        for (const auto& row : result.topk) {
            PanelEntityReport row_report{};
            row_report.series_key = row.series_key;
            row_report.entity_key = row.entity_key;
            row_report.ranking_score = row.ranking_score;
            row_report.run = build_run_report(row.metrics, &report.resolved_objective, &report.run_provenance);
            report.top_rows.push_back(std::move(row_report));
        }

        report.failures.reserve(result.failures.size());
        for (const auto& failure : result.failures) {
            report.failures.push_back(PanelFailureReport{
                failure.series_key,
                failure.entity_key,
                failure.kind,
                failure.reason
            });
        }

        return report;
    }

    void print_panel_report_summary(const PanelReport& report, std::ostream& out) {
        out << "Panel result\n";
        out << "  entities_total=" << report.entities_total
            << " ok=" << report.entities_ok
            << " failed=" << report.entities_failed << "\n";
        out << "  ranking_objective=" << describe_ranking_objective(report.resolved_objective) << "\n";
        print_run_provenance_summary(report.run_provenance, out, "  ");

        for (std::size_t i = 0; i < report.top_rows.size(); ++i) {
            const auto& row = report.top_rows[i];
            out << "  [" << i << "] series=" << row.series_key;
            if (row.entity_key != row.series_key) {
                out << " entity=" << row.entity_key;
            }
            out << " ranking_score=" << row.ranking_score
                << " pnl=" << row.run.execution.pnl
                << " trades=" << row.run.execution.trades;
            if (!row.run.metric_catalog.non_finite_metric_names.empty()) {
                out << " non_finite_metrics=" << row.run.metric_catalog.non_finite_metric_names.size();
            }
            if (!row.run.execution.reported_values.empty()) {
                out << " reported_values=" << row.run.execution.reported_values.size();
            }
            out << " objective_rankability=" << run_rankability_state_name(row.run.objective.rankability);
            out << "\n";
            print_panel_dynamic_execution_line(row.run.execution.dynamic_execution, out);
        }

        if (!report.failures.empty()) {
            out << "  failures:\n";
            for (const auto& failure : report.failures) {
                out << "    series=" << failure.series_key;
                if (failure.entity_key != failure.series_key) {
                    out << " entity=" << failure.entity_key;
                }
                out << ": [" << panel_failure_kind_name(failure.kind) << "] "
                    << failure.reason << "\n";
            }
        }
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
                out.failures.push_back({
                    k,
                    entity_key,
                    PanelFailureKind::MissingPrimaryColumn,
                    "primary column missing in DataView series"
                });
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
                out.failures.push_back({
                    k,
                    entity_key,
                    PanelFailureKind::ExecutionFailed,
                    reason
                });
                continue;
            }

            if (opt.cancel_requested && opt.cancel_requested()) {
                out.canceled = true;
                out.cancel_message = "panel execution canceled";
                break;
            }
            const double ranking_score = compute_panel_ranking_score(result, ranking);
            all_ok.push_back({ k, entity_key, result, ranking_score });
        }

        out.entities_ok = all_ok.size();
        out.entities_failed = out.failures.size();
        const RankingObjectiveSpec objective = build_panel_ranking_objective(ranking);

        std::sort(all_ok.begin(), all_ok.end(),
            [&](const PanelEntityResult& a, const PanelEntityResult& b) {
                if (a.ranking_score != b.ranking_score) return a.ranking_score > b.ranking_score;
                if (ranking_objective_prefers_left(objective, a.metrics, b.metrics)) return true;
                if (ranking_objective_prefers_left(objective, b.metrics, a.metrics)) return false;
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
        ranking.objective = make_drawdown_weighted_pnl_objective(dd_weight);
        return run_ranked_multi_entity_custom_planspec_over_dataview(
            dv,
            spec,
            opt,
            primary_col_id,
            topk,
            ranking);
    }

} // namespace sugar
