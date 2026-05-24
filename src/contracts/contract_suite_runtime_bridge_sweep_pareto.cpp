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

    bool run_compile_bridge_sweep_pareto_contracts(ContractAsserts& t) {
        {
            const std::string pareto_json = R"JSON(
{
  "version": 1,
  "name": "contract_pareto_mixed_objectives",
  "trade_mode": "LongOnly",
  "params": { "alpha": 1, "beta": 1 },
  "numeric_nodes": [
    { "id": "zero", "op": "core.const", "params": { "value": 0.0 } },
    { "id": "one", "op": "core.const", "params": { "value": 1.0 } },
    { "id": "return_value", "op": "core.const", "params": { "value": { "ref": "alpha" } } },
    { "id": "risk_value", "op": "core.const", "params": { "value": { "ref": "beta" } } }
  ],
  "bool_nodes": [
    { "id": "entry", "op": "cmp.gt", "args": ["one", "zero"] }
  ],
  "bindings": { "entry_long": "entry", "exit_long": "", "entry_short": "", "exit_short": "", "allow_trade": "" },
  "sweep": {
    "enabled": true,
    "top_k": 2,
    "max_evals": 9,
    "objective": { "value": { "source": "numeric_node_output", "name": "return_value" }, "direction": "maximize" },
    "pareto": {
      "retained_candidate_limit": 9,
      "output_depollution": {
        "enabled": false,
        "dedupe_trade_hash": false,
        "basin_aware": true,
        "max_per_basin": 2,
        "param_bucket_size": 3,
        "min_param_distance": 1,
        "min_trades": 0
      },
      "primary_sort_index": 0,
      "objectives": [
        { "value": { "source": "numeric_node_output", "name": "return_value" }, "direction": "maximize" },
        { "value": { "source": "numeric_node_output", "name": "risk_value" }, "direction": "minimize" }
      ]
    },
    "params": {
      "alpha": { "values": [1, 2, 3], "is_int": true },
      "beta": { "values": [1, 2, 3], "is_int": true }
    }
  }
}
)JSON";

            PlanSpec pareto_spec{};
            std::string pareto_parse_err{};
            const bool pareto_parsed =
                parse_plan_spec_json(pareto_json, pareto_spec, pareto_parse_err);
            t.expect(pareto_parsed,
                "milestone D PlanSpec parser accepts opt-in Pareto sweep config");
            if (!pareto_parsed) {
                t.out << "    milestone D pareto parse error: "
                    << pareto_parse_err << "\n";
            }
            if (pareto_parsed && pareto_spec.sweep) {
                t.expect(pareto_spec.sweep->pareto.enabled &&
                        pareto_spec.sweep->pareto.objectives.size() == 2 &&
                        pareto_spec.sweep->pareto.objectives[1].direction ==
                            ObjectiveDirection::Minimize,
                    "milestone D Pareto config preserves mixed objective directions");
                t.expect(pareto_spec.sweep->pareto.output_depollution &&
                        !pareto_spec.sweep->pareto.output_depollution->enabled &&
                        !pareto_spec.sweep->pareto.output_depollution->dedupe_trade_hash &&
                        pareto_spec.sweep->pareto.output_depollution->basin_aware &&
                        pareto_spec.sweep->pareto.output_depollution->max_per_basin == 2 &&
                        pareto_spec.sweep->pareto.output_depollution->param_bucket_size == 3 &&
                        pareto_spec.sweep->pareto.output_depollution->min_param_distance == 1,
                    "milestone D Pareto parser accepts explicit output-depollution overrides");
            }

            std::vector<double> close;
            close.reserve(8);
            for (std::size_t i = 0; i < 8; ++i) {
                close.push_back(100.0 + static_cast<double>(i));
            }

            std::shared_ptr<align::AlignedBundle> bundle;
            ColumnRef primary{};
            const bool okdv = make_single_series_dataview_from_close(
                close,
                bundle,
                primary);
            t.expect(okdv && bundle != nullptr,
                "milestone D Pareto fixture creates temporal DataView input");
            if (pareto_parsed && okdv && bundle) {
                std::ostringstream pareto_log;
                BacktestResult pareto_best{};
                PlanSweepReport pareto_report{};
                const bool pareto_ok = run_custom_plan_spec(
                    DataView(*bundle),
                    primary,
                    pareto_spec,
                    PlanSweepRuntimeOptions{},
                    pareto_log,
                    &pareto_best,
                    &pareto_report);
                t.expect(pareto_ok,
                    "milestone D Pareto sweep executes through custom runtime");
                if (pareto_ok) {
                    t.expect(pareto_report.top_candidates.size() == 2 &&
                            pareto_report.result_diversity.retained_candidate_pool == 2 &&
                            nearly_equal(pareto_report.top_candidates.front().score, 3.0),
                        "milestone D Pareto collection preserves scalar Top-K retention");
                    t.expect(pareto_report.pareto.requested &&
                            pareto_report.pareto.computed &&
                            pareto_report.pareto.source ==
                                SweepParetoSummarySource::EvaluatedCandidateObservations &&
                            pareto_report.pareto.observed_candidate_count == 9 &&
                            pareto_report.pareto.unique_candidate_count == 9 &&
                            pareto_report.pareto.duplicate_observation_count == 0 &&
                            pareto_report.pareto.retained_candidate_pool == 2 &&
                            pareto_report.pareto.finite_candidate_count == 9 &&
                            pareto_report.pareto.front_row_limit == 9 &&
                            pareto_report.pareto.emitted_front_row_count == 1 &&
                            !pareto_report.pareto.front_rows_capped,
                        "milestone D Pareto summary records evaluated candidates without expanding scalar retention");
                    t.expect(pareto_report.pareto.non_dominated_candidate_count == 1 &&
                            pareto_report.pareto.dominated_candidate_count == 8 &&
                            pareto_report.pareto.front.size() == 1,
                        "milestone D Pareto dominance counts mixed maximize/minimize objectives");
                    if (!pareto_report.pareto.front.empty()) {
                        const auto& front = pareto_report.pareto.front.front();
                        t.expect(front.candidate_index == 2 &&
                                front.objective_values.size() == 2 &&
                                nearly_equal(front.objective_values[0].value, 3.0) &&
                                nearly_equal(front.objective_values[1].value, 1.0) &&
                                front.dominates_count == 8,
                            "milestone D Pareto front exposes objective vectors and dominated counts");
                    }
                    t.expect(pareto_log.str().find("Pareto front:") != std::string::npos &&
                            pareto_log.str().find("primary sort objective") != std::string::npos,
                        "milestone D full sweep report prints requested Pareto facts additively");
                    {
                        std::ostringstream pareto_projection;
                        print_evaluation_report(
                            make_sweep_evaluation_report(pareto_report),
                            EvaluationReportProjection::ParetoFront,
                            pareto_projection);
                        const std::string pareto_text = pareto_projection.str();
                        t.expect(pareto_text.find("Sweep Pareto front:") != std::string::npos &&
                                pareto_text.find("source=evaluated_candidate_observations") != std::string::npos &&
                                pareto_text.find("observed_candidates=9") != std::string::npos &&
                                pareto_text.find("non_dominated_candidates=1") != std::string::npos &&
                                pareto_text.find("emitted_front_rows=1") != std::string::npos &&
                                pareto_text.find("output_depollution_source=none") != std::string::npos &&
                                pareto_text.find("objective_ranges:") != std::string::npos &&
                                pareto_text.find("front_rows_capped=false") != std::string::npos &&
                                pareto_text.find("risk_value=1") != std::string::npos,
                            "milestone D Pareto projection prints focused non-dominated front output");
                    }
                    {
                        std::ostringstream json;
                        print_evaluation_report(
                            make_sweep_evaluation_report(pareto_report),
                            EvaluationReportProjection::Json,
                            json);
                        const std::string text = json.str();
                        t.expect(text.find("\"pareto\":{") != std::string::npos &&
                                text.find("\"source\":\"evaluated_candidate_observations\"") != std::string::npos &&
                                text.find("\"observed_candidate_count\":9") != std::string::npos &&
                                text.find("\"retained_candidate_pool\":2") != std::string::npos &&
                                text.find("\"non_dominated_candidate_count\":1") != std::string::npos &&
                                text.find("\"emitted_front_row_count\":1") != std::string::npos &&
                                text.find("\"output_depollution_source\":\"none\"") != std::string::npos &&
                                text.find("\"objective_ranges\":[") != std::string::npos &&
                                text.find("\"label\":\"risk_value\"") != std::string::npos &&
                                text.find("\"front_rows_capped\":false") != std::string::npos &&
                                text.find("\"primary_sort_objective\":{") != std::string::npos &&
                                text.find("\"objective_values\":[") != std::string::npos,
                            "milestone D JSON projection serializes Pareto front structure");
                    }
                }
            }

            const std::string pareto_tie_json = R"JSON(
{
  "version": 1,
  "name": "contract_pareto_deterministic_ties",
  "trade_mode": "LongOnly",
  "params": { "alpha": 1, "beta": 1 },
  "numeric_nodes": [
    { "id": "zero", "op": "core.const", "params": { "value": 0.0 } },
    { "id": "one", "op": "core.const", "params": { "value": 1.0 } },
    { "id": "return_value", "op": "core.const", "params": { "value": { "ref": "alpha" } } },
    { "id": "risk_value", "op": "core.const", "params": { "value": { "ref": "alpha" } } }
  ],
  "bool_nodes": [
    { "id": "entry", "op": "cmp.gt", "args": ["one", "zero"] }
  ],
  "bindings": { "entry_long": "entry", "exit_long": "", "entry_short": "", "exit_short": "", "allow_trade": "" },
  "sweep": {
    "enabled": true,
    "top_k": 2,
    "max_evals": 6,
    "objective": { "value": { "source": "numeric_node_output", "name": "return_value" }, "direction": "maximize" },
    "pareto": {
      "retained_candidate_limit": 6,
      "primary_sort_index": 0,
      "objectives": [
        { "label": "return", "value": { "source": "numeric_node_output", "name": "return_value" }, "direction": "maximize" },
        { "label": "risk", "value": { "source": "numeric_node_output", "name": "risk_value" }, "direction": "minimize" }
      ]
    },
    "params": {
      "alpha": { "values": [1, 2, 3], "is_int": true },
      "beta": { "values": [1, 2], "is_int": true }
    }
  }
}
)JSON";

            PlanSpec pareto_tie_spec{};
            std::string pareto_tie_parse_err{};
            const bool pareto_tie_parsed =
                parse_plan_spec_json(
                    pareto_tie_json,
                    pareto_tie_spec,
                    pareto_tie_parse_err);
            t.expect(pareto_tie_parsed,
                "milestone D PlanSpec parser accepts Pareto tie-order fixture");
            if (pareto_tie_parsed && pareto_tie_spec.sweep) {
                t.expect(pareto_tie_spec.sweep->pareto.front_row_limit == 6,
                    "milestone D Pareto parser maps retained_candidate_limit compatibility spelling to front_row_limit");
                t.expect(pareto_tie_spec.sweep->pareto.objectives.size() == 2 &&
                        pareto_tie_spec.sweep->pareto.objectives[0].label == "return" &&
                        pareto_tie_spec.sweep->pareto.objectives[1].label == "risk",
                    "milestone D Pareto parser preserves optional objective labels");
            }
            if (pareto_tie_parsed && okdv && bundle) {
                std::ostringstream pareto_tie_log;
                BacktestResult pareto_tie_best{};
                PlanSweepReport pareto_tie_report{};
                const bool pareto_tie_ok = run_custom_plan_spec(
                    DataView(*bundle),
                    primary,
                    pareto_tie_spec,
                    PlanSweepRuntimeOptions{},
                    pareto_tie_log,
                    &pareto_tie_best,
                    &pareto_tie_report);
                t.expect(pareto_tie_ok,
                    "milestone D Pareto tie-order sweep executes through custom runtime");
                if (pareto_tie_ok) {
                    t.expect(pareto_tie_report.pareto.front.size() == 6 &&
                            pareto_tie_report.pareto.dominated_candidate_count == 0,
                        "milestone D Pareto collector keeps tradeoff-equivalent candidates non-dominated");
                    t.expect(pareto_tie_report.pareto.front[0].candidate_index == 2 &&
                            pareto_tie_report.pareto.front[1].candidate_index == 5 &&
                            pareto_tie_report.pareto.front[2].candidate_index == 1,
                        "milestone D Pareto front ordering is deterministic for objective ties");
                    t.expect(pareto_tie_report.pareto.objective_ranges.size() == 2 &&
                            pareto_tie_report.pareto.objective_ranges[0].label == "return" &&
                            pareto_tie_report.pareto.objective_ranges[0].raw_non_dominated_has_value &&
                            nearly_equal(
                                pareto_tie_report.pareto.objective_ranges[0].raw_non_dominated_min,
                                1.0) &&
                            nearly_equal(
                                pareto_tie_report.pareto.objective_ranges[0].raw_non_dominated_max,
                                3.0) &&
                            nearly_equal(
                                pareto_tie_report.pareto.objective_ranges[0].raw_non_dominated_range,
                                2.0) &&
                            pareto_tie_report.pareto.objective_ranges[0].emitted_front_has_value &&
                            nearly_equal(
                                pareto_tie_report.pareto.objective_ranges[0].emitted_front_min,
                                1.0) &&
                            nearly_equal(
                                pareto_tie_report.pareto.objective_ranges[0].emitted_front_max,
                                3.0),
                        "milestone D Pareto reports raw and emitted objective ranges");

                    auto run_pareto_variant =
                        [&](const PlanSpec& variant,
                            PlanSweepReport& report,
                            const char* label) {
                            std::ostringstream log;
                            BacktestResult best{};
                            const bool ok = run_custom_plan_spec(
                                DataView(*bundle),
                                primary,
                                variant,
                                PlanSweepRuntimeOptions{},
                                log,
                                &best,
                                &report);
                            t.expect(ok, label);
                            return ok;
                        };

                    PlanSpec pareto_capped_spec = pareto_tie_spec;
                    pareto_capped_spec.name = "contract_pareto_front_row_cap";
                    pareto_capped_spec.sweep->pareto.front_row_limit = 3;
                    PlanSweepReport pareto_capped_report{};
                    if (run_pareto_variant(
                            pareto_capped_spec,
                            pareto_capped_report,
                            "milestone D Pareto capped-front fixture executes through custom runtime")) {
                        t.expect(pareto_capped_report.pareto.non_dominated_candidate_count == 6 &&
                                pareto_capped_report.pareto.emitted_front_row_count == 3 &&
                                pareto_capped_report.pareto.front.size() == 3 &&
                                pareto_capped_report.pareto.front_rows_capped &&
                                pareto_capped_report.pareto.suppressed_row_limit == 3,
                            "milestone D Pareto front row cap limits emitted rows without changing raw front count");
                    }

                    PlanSpec pareto_uncapped_spec = pareto_capped_spec;
                    pareto_uncapped_spec.name = "contract_pareto_uncapped_front_rows";
                    pareto_uncapped_spec.sweep->pareto.front_row_limit = 0;
                    PlanSweepReport pareto_uncapped_report{};
                    if (run_pareto_variant(
                            pareto_uncapped_spec,
                            pareto_uncapped_report,
                            "milestone D Pareto uncapped-front fixture executes through custom runtime")) {
                        t.expect(pareto_uncapped_report.pareto.non_dominated_candidate_count == 6 &&
                                pareto_uncapped_report.pareto.emitted_front_row_count == 6 &&
                                pareto_uncapped_report.pareto.front.size() == 6 &&
                                !pareto_uncapped_report.pareto.front_rows_capped &&
                                pareto_uncapped_report.pareto.suppressed_row_limit == 0,
                            "milestone D Pareto front_row_limit zero emits the full raw front");
                    }

                    PlanSpec pareto_trade_hash_spec = pareto_tie_spec;
                    pareto_trade_hash_spec.name =
                        "contract_pareto_uses_trade_hash_output_depollution";
                    auto& trade_hash_sweep = *pareto_trade_hash_spec.sweep;
                    trade_hash_sweep.pareto.front_row_limit = 6;
                    trade_hash_sweep.result_diversity.enabled = true;
                    trade_hash_sweep.result_diversity.dedupe_trade_hash = true;
                    trade_hash_sweep.result_diversity.basin_aware = false;
                    PlanSweepReport pareto_trade_hash_report{};
                    if (run_pareto_variant(
                            pareto_trade_hash_spec,
                            pareto_trade_hash_report,
                            "milestone D Pareto trade-hash depollution fixture executes through custom runtime")) {
                        t.expect(pareto_trade_hash_report.pareto.non_dominated_candidate_count == 6 &&
                                pareto_trade_hash_report.pareto.emitted_front_row_count == 1 &&
                                pareto_trade_hash_report.pareto.front.size() == 1 &&
                                pareto_trade_hash_report.pareto.output_depollution_requested &&
                                pareto_trade_hash_report.pareto.output_depollution_used_result_diversity &&
                                pareto_trade_hash_report.pareto.suppressed_duplicate_trade_hash == 5,
                            "milestone D Pareto emitted rows reuse trade-hash output depollution");
                    }

                    PlanSpec pareto_basin_spec = pareto_tie_spec;
                    pareto_basin_spec.name =
                        "contract_pareto_uses_basin_output_depollution";
                    auto& basin_sweep = *pareto_basin_spec.sweep;
                    basin_sweep.pareto.front_row_limit = 6;
                    basin_sweep.result_diversity.enabled = true;
                    basin_sweep.result_diversity.dedupe_trade_hash = false;
                    basin_sweep.result_diversity.basin_aware = true;
                    basin_sweep.result_diversity.max_per_basin = 1;
                    basin_sweep.result_diversity.param_bucket_size = 4;
                    basin_sweep.result_diversity.min_param_distance = 0;
                    PlanSweepReport pareto_basin_report{};
                    if (run_pareto_variant(
                            pareto_basin_spec,
                            pareto_basin_report,
                            "milestone D Pareto basin depollution fixture executes through custom runtime")) {
                        t.expect(pareto_basin_report.pareto.non_dominated_candidate_count == 6 &&
                                pareto_basin_report.pareto.emitted_front_row_count == 1 &&
                                pareto_basin_report.pareto.front.size() == 1 &&
                                pareto_basin_report.pareto.emitted_basin_count == 1 &&
                                pareto_basin_report.pareto.suppressed_basin == 5,
                            "milestone D Pareto emitted rows reuse basin-aware output depollution");
                    }

                    PlanSpec pareto_specific_trade_hash_spec = pareto_tie_spec;
                    pareto_specific_trade_hash_spec.name =
                        "contract_pareto_output_depollution_trade_hash_override";
                    auto& pareto_specific_trade_hash_sweep =
                        *pareto_specific_trade_hash_spec.sweep;
                    pareto_specific_trade_hash_sweep.pareto.front_row_limit = 6;
                    pareto_specific_trade_hash_sweep.result_diversity.enabled = false;
                    PlanSweepResultDiversityConfig pareto_trade_hash_config{};
                    pareto_trade_hash_config.enabled = true;
                    pareto_trade_hash_config.dedupe_trade_hash = true;
                    pareto_trade_hash_config.basin_aware = false;
                    pareto_specific_trade_hash_sweep.pareto.output_depollution =
                        pareto_trade_hash_config;
                    PlanSweepReport pareto_specific_trade_hash_report{};
                    if (run_pareto_variant(
                            pareto_specific_trade_hash_spec,
                            pareto_specific_trade_hash_report,
                            "milestone D Pareto-specific trade-hash depollution fixture executes through custom runtime")) {
                        t.expect(pareto_specific_trade_hash_report.pareto.non_dominated_candidate_count == 6 &&
                                pareto_specific_trade_hash_report.pareto.emitted_front_row_count == 1 &&
                                pareto_specific_trade_hash_report.pareto.output_depollution_requested &&
                                !pareto_specific_trade_hash_report.pareto.output_depollution_used_result_diversity &&
                                pareto_specific_trade_hash_report.pareto.output_depollution_source ==
                                    SweepParetoOutputDepollutionSource::ParetoOutputDepollution &&
                                pareto_specific_trade_hash_report.pareto.output_depollution_dedupe_trade_hash &&
                                pareto_specific_trade_hash_report.pareto.suppressed_duplicate_trade_hash == 5,
                            "milestone D Pareto output_depollution overrides inherited global depollution");
                    }

                    PlanSpec pareto_specific_basin_spec = pareto_tie_spec;
                    pareto_specific_basin_spec.name =
                        "contract_pareto_output_depollution_relaxes_basin";
                    auto& pareto_specific_basin_sweep =
                        *pareto_specific_basin_spec.sweep;
                    pareto_specific_basin_sweep.pareto.front_row_limit = 6;
                    pareto_specific_basin_sweep.result_diversity.enabled = false;
                    PlanSweepResultDiversityConfig pareto_basin_config{};
                    pareto_basin_config.enabled = true;
                    pareto_basin_config.dedupe_trade_hash = false;
                    pareto_basin_config.basin_aware = true;
                    pareto_basin_config.max_per_basin = 2;
                    pareto_basin_config.param_bucket_size = 4;
                    pareto_basin_config.min_param_distance = 0;
                    pareto_specific_basin_sweep.pareto.output_depollution =
                        pareto_basin_config;
                    PlanSweepReport pareto_specific_basin_report{};
                    if (run_pareto_variant(
                            pareto_specific_basin_spec,
                            pareto_specific_basin_report,
                            "milestone D Pareto-specific basin depollution fixture executes through custom runtime")) {
                        t.expect(pareto_specific_basin_report.pareto.non_dominated_candidate_count == 6 &&
                                pareto_specific_basin_report.pareto.emitted_front_row_count == 2 &&
                                pareto_specific_basin_report.pareto.front.size() == 2 &&
                                pareto_specific_basin_report.pareto.output_depollution_source ==
                                    SweepParetoOutputDepollutionSource::ParetoOutputDepollution &&
                                pareto_specific_basin_report.pareto.output_depollution_basin_aware &&
                                pareto_specific_basin_report.pareto.output_depollution_max_per_basin == 2 &&
                                pareto_specific_basin_report.pareto.suppressed_basin == 4,
                            "milestone D Pareto basin depollution exposes tunable max-per-basin output");
                    }

                    PlanSpec pareto_disabled_override_spec = pareto_tie_spec;
                    pareto_disabled_override_spec.name =
                        "contract_pareto_output_depollution_disabled_override";
                    auto& pareto_disabled_override_sweep =
                        *pareto_disabled_override_spec.sweep;
                    pareto_disabled_override_sweep.pareto.front_row_limit = 6;
                    pareto_disabled_override_sweep.result_diversity.enabled = true;
                    pareto_disabled_override_sweep.result_diversity.dedupe_trade_hash = true;
                    pareto_disabled_override_sweep.result_diversity.basin_aware = true;
                    PlanSweepResultDiversityConfig pareto_disabled_config{};
                    pareto_disabled_config.enabled = false;
                    pareto_disabled_override_sweep.pareto.output_depollution =
                        pareto_disabled_config;
                    PlanSweepReport pareto_disabled_override_report{};
                    if (run_pareto_variant(
                            pareto_disabled_override_spec,
                            pareto_disabled_override_report,
                            "milestone D Pareto disabled-output-depollution fixture executes through custom runtime")) {
                        t.expect(pareto_disabled_override_report.pareto.non_dominated_candidate_count == 6 &&
                                pareto_disabled_override_report.pareto.emitted_front_row_count == 6 &&
                                !pareto_disabled_override_report.pareto.output_depollution_requested &&
                                !pareto_disabled_override_report.pareto.output_depollution_used_result_diversity &&
                                pareto_disabled_override_report.pareto.output_depollution_source ==
                                    SweepParetoOutputDepollutionSource::None &&
                                pareto_disabled_override_report.pareto.suppressed_duplicate_trade_hash == 0 &&
                                pareto_disabled_override_report.pareto.suppressed_basin == 0,
                            "milestone D Pareto output_depollution can disable inherited depollution");
                    }

                    PlanSpec pareto_duplicate_spec = pareto_tie_spec;
                    pareto_duplicate_spec.name =
                        "contract_pareto_duplicate_observations";
                    auto& duplicate_sweep = *pareto_duplicate_spec.sweep;
                    duplicate_sweep.engine = PlanSweepEngineKind::CoarseFine;
                    duplicate_sweep.top_k = 1;
                    duplicate_sweep.max_evals = 4;
                    duplicate_sweep.coarse_fine.k_stage1 = 2;
                    duplicate_sweep.coarse_fine.k_stage2 = 2;
                    duplicate_sweep.coarse_fine.depollute_stage1 = false;
                    duplicate_sweep.coarse_fine.dedupe_stage2 = false;
                    duplicate_sweep.coarse_fine.stage2_hops = 1;
                    duplicate_sweep.params.clear();
                    SweepParamRange alpha_range{};
                    alpha_range.values = { 1.0, 2.0 };
                    alpha_range.is_int = true;
                    duplicate_sweep.params["alpha"] = std::move(alpha_range);

                    std::ostringstream pareto_duplicate_log;
                    BacktestResult pareto_duplicate_best{};
                    PlanSweepReport pareto_duplicate_report{};
                    const bool pareto_duplicate_ok = run_custom_plan_spec(
                        DataView(*bundle),
                        primary,
                        pareto_duplicate_spec,
                        PlanSweepRuntimeOptions{},
                        pareto_duplicate_log,
                        &pareto_duplicate_best,
                        &pareto_duplicate_report);
                    t.expect(pareto_duplicate_ok,
                        "milestone D Pareto duplicate-observation fixture executes through custom runtime");
                    if (pareto_duplicate_ok) {
                        t.expect(pareto_duplicate_report.top_candidates.size() == 1 &&
                                pareto_duplicate_report.pareto.observed_candidate_count == 4 &&
                                pareto_duplicate_report.pareto.unique_candidate_count == 2 &&
                                pareto_duplicate_report.pareto.duplicate_observation_count == 2,
                            "milestone D Pareto collector observes repeated evaluations while deduping candidate indices");
                        t.expect(pareto_duplicate_report.pareto.front.size() == 2 &&
                                pareto_duplicate_report.pareto.front[0].candidate_index == 1 &&
                                pareto_duplicate_report.pareto.front[1].candidate_index == 0,
                            "milestone D Pareto duplicate observations do not duplicate front candidates");
                    }
                }
            }
        }

        return true;
    }

} // namespace sugar::contract_suite_internal
