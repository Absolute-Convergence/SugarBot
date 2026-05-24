#include "contract_suite_runtime_bridge_sweep.h"
#include "contract_suite_runtime_shared.h"
#include "strategy_plan_internal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "op_registry_internal.h"
#include "ops/op_registry.h"
#include "sweep/evaluation_report.h"
#include "sweep/panel_runner.h"
#include "sweep/plan_capability.h"
#include "sweep/sweep.h"
#include "engine/plan_compiler.h"
#include "sweep/plan_sweep.h"
#include "engine/program_evaluator.h"
#include "ingest/utils.h"
#include "ingest/run_manifest.h"

namespace sugar::contract_suite_internal {

    bool run_compile_bridge_sweep_stage_metric_contracts(ContractAsserts& t) {
        {
            std::vector<double> close;
            close.reserve(12);
            for (std::size_t i = 0; i < 12; ++i) {
                close.push_back(100.0 + static_cast<double>(i));
            }

            std::shared_ptr<align::AlignedBundle> bundle;
            ColumnRef primary{};
            const bool okdv = make_single_series_dataview_from_close(close, bundle, primary);
            t.expect(okdv && bundle != nullptr,
                "phase 6 coarse-fine progress fixture creates temporal DataView input");
            if (!okdv || !bundle) return false;

            PlanSpec sweep_spec{};
            sweep_spec.version = 1;
            sweep_spec.name = "contract_phase6_stage_progress";
            sweep_spec.trade_mode = TradeMode::LongOnly;
            sweep_spec.params["alpha"] = static_cast<std::int64_t>(1);

            NodeSpec zero{};
            zero.id = "zero";
            zero.op = "core.const";
            zero.params["value"] = 0.0;

            NodeSpec one{};
            one.id = "one";
            one.op = "core.const";
            one.params["value"] = 1.0;

            NodeSpec score_value{};
            score_value.id = "score_value";
            score_value.op = "core.const";
            score_value.params["value"] = ParamRef{ "alpha" };

            sweep_spec.numeric_nodes = { zero, one, score_value };

            NodeSpec entry{};
            entry.id = "entry";
            entry.op = "cmp.gt";
            entry.args = { "one", "zero" };
            sweep_spec.bool_nodes = { entry };
            sweep_spec.bind.entry_long = "entry";

            PlanSweepSpec sweep_cfg{};
            sweep_cfg.enabled = true;
            sweep_cfg.engine = PlanSweepEngineKind::CoarseFine;
            sweep_cfg.top_k = 2;
            sweep_cfg.max_evals = 3;
            sweep_cfg.coarse_fine.k_stage1 = 2;
            sweep_cfg.coarse_fine.k_stage2 = 2;
            sweep_cfg.coarse_fine.depollute_stage1 = false;
            sweep_cfg.coarse_fine.stage1_stride = 0;
            sweep_cfg.objective_spec = RankingObjectiveSpec{};
            sweep_cfg.objective_spec->primary.source = ObjectiveValueSource::NumericNodeOutput;
            sweep_cfg.objective_spec->primary.name = "score_value";
            sweep_cfg.objective_spec->direction = ObjectiveDirection::Maximize;
            SweepParamRange alpha_range{};
            alpha_range.values = { 1.0, 5.0, 3.0 };
            alpha_range.is_int = true;
            sweep_cfg.params["alpha"] = alpha_range;
            sweep_spec.sweep = sweep_cfg;

            std::vector<SweepProgress> progress_log;
            PlanSweepRuntimeOptions opt{};
            opt.progress_cb = [&](const SweepProgress& progress) {
                progress_log.push_back(progress);
            };

            std::ostringstream sweep_log;
            BacktestResult sweep_best{};
            PlanSweepReport sweep_report{};
            const bool sweep_ok = run_custom_plan_spec(
                DataView(*bundle),
                primary,
                sweep_spec,
                opt,
                sweep_log,
                &sweep_best,
                &sweep_report);
            t.expect(sweep_ok, "phase 6 coarse-fine objective sweep executes through custom runtime");
            if (sweep_ok) {
                const bool saw_coarse_search = std::any_of(
                    progress_log.begin(),
                    progress_log.end(),
                    [](const SweepProgress& progress) {
                        return progress.stage == SweepProgressStage::CoarseSearch;
                    });
                t.expect(saw_coarse_search,
                    "phase 6 progress stream distinguishes the coarse-search stage");
                const bool saw_local_refinement = std::any_of(
                    progress_log.begin(),
                    progress_log.end(),
                    [](const SweepProgress& progress) {
                        return progress.stage == SweepProgressStage::LocalRefinement;
                    });
                t.expect(saw_local_refinement,
                    "phase 6 progress stream distinguishes the local-refinement stage");
                const bool report_has_coarse_stage = std::any_of(
                    sweep_report.stage_summaries.begin(),
                    sweep_report.stage_summaries.end(),
                    [](const SweepStageSummary& stage) {
                        return stage.stage == SweepProgressStage::CoarseSearch;
                    });
                t.expect(report_has_coarse_stage,
                    "phase 6 structured sweep report preserves coarse-search stage totals");
                const bool report_has_refinement_stage = std::any_of(
                    sweep_report.stage_summaries.begin(),
                    sweep_report.stage_summaries.end(),
                    [](const SweepStageSummary& stage) {
                        return stage.stage == SweepProgressStage::LocalRefinement;
                    });
                t.expect(report_has_refinement_stage,
                    "phase 6 structured sweep report preserves local-refinement stage totals");
            }
        }

        {
            const std::string metric_json = R"JSON(
{
  "version": 1,
  "name": "contract_phase6_non_finite_metrics",
  "trade_mode": "LongOnly",
  "params": {
    "dummy": 1
  },
  "numeric_nodes": [
    { "id": "zero", "op": "core.const", "params": { "value": 0.0 } },
    { "id": "one", "op": "core.const", "params": { "value": 1.0 } },
    { "id": "nan_value", "op": "ts.div", "args": ["zero", "zero"] }
  ],
  "bool_nodes": [
    { "id": "entry", "op": "cmp.gt", "args": ["one", "zero"] }
  ],
  "bindings": {
    "entry_long": "entry",
    "exit_long": "",
    "entry_short": "",
    "exit_short": "",
    "allow_trade": ""
  },
  "sweep": {
    "enabled": true,
    "top_k": 1,
    "max_evals": 1,
    "objective": {
      "value": {
        "source": "reported_metric",
        "name": "pnl"
      },
      "direction": "maximize",
      "tie_breaks": [
        {
          "value": {
            "source": "numeric_node_output",
            "name": "nan_value",
            "summary": "last_finite"
          },
          "direction": "maximize"
        }
      ]
    },
    "params": {
      "dummy": {
        "values": [1],
        "is_int": true
      }
    }
  }
}
)JSON";

            PlanSpec metric_spec{};
            std::string parse_err;
            const bool parse_ok = parse_plan_spec_json(metric_json, metric_spec, parse_err);
            t.expect(parse_ok, "phase 6 non-finite metric objective form parses");
            if (!parse_ok) {
                t.out << "    phase 6 non-finite metric parse error: " << parse_err << "\n";
                return false;
            }

            std::vector<double> close;
            close.reserve(8);
            for (std::size_t i = 0; i < 8; ++i) {
                close.push_back(100.0 + static_cast<double>(i));
            }

            std::shared_ptr<align::AlignedBundle> bundle;
            ColumnRef primary{};
            const bool okdv = make_single_series_dataview_from_close(close, bundle, primary);
            t.expect(okdv && bundle != nullptr,
                "phase 6 non-finite metric fixture creates temporal DataView input");
            if (!okdv || !bundle) return false;

            std::ostringstream sweep_log;
            BacktestResult sweep_best{};
            PlanSweepReport sweep_report{};
            const bool sweep_ok = run_custom_plan_spec(
                DataView(*bundle),
                primary,
                metric_spec,
                PlanSweepRuntimeOptions{},
                sweep_log,
                &sweep_best,
                &sweep_report);
            t.expect(sweep_ok, "phase 6 non-finite metric sweep executes through custom runtime");
            if (sweep_ok) {
                const RunReport best_report = build_run_report(sweep_best);
                const auto* non_finite_metric = find_metric_value(best_report, "nan_value");
                t.expect(
                    non_finite_metric != nullptr &&
                    non_finite_metric->kind == ResultMetricKind::ReportedValue &&
                    !non_finite_metric->is_finite,
                    "phase 6 result metric catalog marks non-finite reported metrics explicitly");
                const bool has_non_finite_name = std::find(
                    best_report.metric_catalog.non_finite_metric_names.begin(),
                    best_report.metric_catalog.non_finite_metric_names.end(),
                    "nan_value") != best_report.metric_catalog.non_finite_metric_names.end();
                t.expect(has_non_finite_name,
                    "phase 6 result metric catalog preserves non-finite metric names");
                t.expect(sweep_log.str().find("Non-finite metrics:") != std::string::npos,
                    "phase 6 backtest reporting surfaces explicit non-finite metric summaries");
                t.expect(sweep_report.candidate_outcomes.completed_with_non_finite_metrics == 1 &&
                        sweep_report.candidate_outcomes.failed == 0,
                    "phase 6 structured sweep report counts completed non-finite candidates separately from failures");
                t.expect(sweep_report.candidate_rankability.rankable == 1,
                    "phase 6 structured sweep report preserves primary-objective rankability even when other metrics are non-finite");
                const auto observation_it = std::find_if(
                    sweep_report.proposal_trace.events.begin(),
                    sweep_report.proposal_trace.events.end(),
                    [](const SweepTraceEvent& event) {
                        return event.kind == SweepTraceEventKind::Observation;
                    });
                t.expect(observation_it != sweep_report.proposal_trace.events.end() &&
                        observation_it->run_outcome_present &&
                        observation_it->run_outcome ==
                            RunOutcomeState::CompletedWithNonFiniteMetrics &&
                        observation_it->run_rankability_present &&
                        observation_it->run_rankability ==
                            RunRankabilityState::Rankable &&
                        sweep_report.proposal_trace.observation_payload_complete,
                    "milestone E2 observation payload records non-finite outcome state while preserving rankability");
            }

            RankingObjectiveSpec non_finite_objective{};
            non_finite_objective.primary.source = ObjectiveValueSource::NumericNodeOutput;
            non_finite_objective.primary.name = "nan_value";
            non_finite_objective.primary.summary_mode = NumericNodeSummaryMode::LastFiniteValue;
            non_finite_objective.direction = ObjectiveDirection::Maximize;

            BacktestResult non_finite_result{};
            non_finite_result.reported_values.push_back(
                NamedReportedValue{ "nan_value", std::numeric_limits<double>::quiet_NaN() });
            const RunReport non_finite_objective_report =
                build_run_report(non_finite_result, &non_finite_objective);
            t.expect(non_finite_objective_report.objective.requested,
                "phase 6 run report preserves requested non-finite objective metadata");
            t.expect(non_finite_objective_report.objective.rankability == RunRankabilityState::NonFinitePrimaryValue,
                "phase 6 run report classifies non-finite primary objectives as non-rankable");
            t.expect(non_finite_objective_report.outcome == RunOutcomeState::CompletedWithNonFiniteMetrics,
                "phase 6 run report classifies preserved non-finite metrics in run outcome state");
        }

        return true;
    }

} // namespace sugar::contract_suite_internal
