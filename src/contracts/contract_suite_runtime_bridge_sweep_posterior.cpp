#include "contract_suite_runtime_bridge_sweep.h"
#include "contract_suite_runtime_shared.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "sweep/evaluation_report.h"
#include "sweep/plan_sweep.h"
#include "sweep/sweep.h"

namespace sugar::contract_suite_internal {

    namespace {
        bool has_policy_config(
            const SweepTraceSummary& trace,
            std::string_view name,
            SweepTracePolicyConfigValueKind value_kind,
            std::string_view value) {
            return std::any_of(
                trace.effective_policy_config.begin(),
                trace.effective_policy_config.end(),
                [&](const SweepTracePolicyConfigEntry& entry) {
                    return entry.name == name &&
                        entry.value_kind == value_kind &&
                        entry.value == value;
                });
        }

        bool top_candidates_equal(
            const std::vector<SweepRankedCandidate>& lhs,
            const std::vector<SweepRankedCandidate>& rhs) {
            if (lhs.size() != rhs.size()) {
                return false;
            }
            for (std::size_t i = 0; i < lhs.size(); ++i) {
                if (lhs[i].candidate_index != rhs[i].candidate_index ||
                    !nearly_equal(lhs[i].score, rhs[i].score)) {
                    return false;
                }
            }
            return true;
        }

        const char* posterior_contract_json() {
            return R"JSON(
{
  "version": 1,
  "name": "contract_bayesian_posterior_scaffold",
  "trade_mode": "LongOnly",
  "params": { "alpha": 1, "beta": 1 },
  "numeric_nodes": [
    { "id": "zero", "op": "core.const", "params": { "value": 0.0 } },
    { "id": "one", "op": "core.const", "params": { "value": 1.0 } },
    { "id": "score_value", "op": "core.const", "params": { "value": { "ref": "alpha" } } }
  ],
  "bool_nodes": [
    { "id": "entry", "op": "cmp.gt", "args": ["one", "zero"] }
  ],
  "bindings": { "entry_long": "entry", "exit_long": "", "entry_short": "", "exit_short": "", "allow_trade": "" },
  "sweep": {
    "enabled": true,
    "engine": "bayesian_posterior",
    "top_k": 2,
    "max_evals": 12,
    "objective": { "value": { "source": "numeric_node_output", "name": "score_value" }, "direction": "maximize" },
    "result_diversity": {
      "dedupe_trade_hash": true,
      "oversample_multiplier": 4
    },
    "pareto": {
      "objectives": [
        { "label": "pnl", "value": { "source": "reported_metric", "name": "pnl" }, "direction": "maximize" }
      ],
      "primary_sort_index": 0,
      "front_row_limit": 5,
      "use_result_diversity": false,
      "output_depollution": {
        "enabled": false,
        "dedupe_trade_hash": false
      }
    },
    "params": {
      "alpha": { "min": 1, "max": 4, "step": 1, "is_int": true },
      "beta": { "min": 1, "max": 3, "step": 1, "is_int": true }
    },
    "engine_config": {
      "bayesian_posterior": {
        "surrogate": "gaussian_process",
        "acquisition": "upper_confidence_bound",
        "initial_design_size": 5,
        "iterations": 7,
        "seed": 17,
        "exploration_weight": 1.5,
        "normalize_inputs": false,
        "top_k": 3
      }
    }
  }
}
)JSON";
        }

        std::string posterior_minimal_json(std::string_view engine) {
            std::string json;
            json.reserve(512);
            json +=
                "{\n"
                "  \"version\": 1,\n"
                "  \"name\": \"contract_bayesian_posterior_defaults\",\n"
                "  \"trade_mode\": \"LongOnly\",\n"
                "  \"params\": { \"alpha\": 1 },\n"
                "  \"numeric_nodes\": [],\n"
                "  \"bool_nodes\": [],\n"
                "  \"bindings\": {\n"
                "    \"entry_long\": \"\",\n"
                "    \"exit_long\": \"\",\n"
                "    \"entry_short\": \"\",\n"
                "    \"exit_short\": \"\",\n"
                "    \"allow_trade\": \"\"\n"
                "  },\n"
                "  \"sweep\": {\n"
                "    \"enabled\": true,\n"
                "    \"engine\": \"";
            json += engine;
            json +=
                "\",\n"
                "    \"params\": {\n"
                "      \"alpha\": { \"values\": [1], \"is_int\": true }\n"
                "    }\n"
                "  }\n"
                "}\n";
            return json;
        }

        const char* posterior_invalid_acquisition_json() {
            return R"JSON(
{
  "version": 1,
  "name": "contract_bayesian_posterior_invalid_acquisition",
  "trade_mode": "LongOnly",
  "params": { "alpha": 1 },
  "numeric_nodes": [],
  "bool_nodes": [],
  "bindings": { "entry_long": "", "exit_long": "", "entry_short": "", "exit_short": "", "allow_trade": "" },
  "sweep": {
    "enabled": true,
    "engine": "bayesian_posterior",
    "params": { "alpha": { "values": [1], "is_int": true } },
    "engine_config": {
      "bayesian_posterior": {
        "acquisition": "mystery"
      }
    }
  }
}
)JSON";
        }
    } // namespace

    bool run_compile_bridge_sweep_posterior_contracts(ContractAsserts& t) {
        {
            PlanSpec defaults_spec{};
            std::string defaults_err{};
            const bool defaults_parsed = parse_plan_spec_json(
                posterior_minimal_json("posterior"),
                defaults_spec,
                defaults_err);
            t.expect(defaults_parsed && defaults_spec.sweep &&
                    defaults_spec.sweep->engine ==
                        PlanSweepEngineKind::BayesianPosterior &&
                    defaults_spec.sweep->bayesian_posterior.surrogate ==
                        PlanSweepPosteriorSurrogateKind::GaussianProcess &&
                    defaults_spec.sweep->bayesian_posterior.acquisition ==
                        PlanSweepPosteriorAcquisitionKind::ExpectedImprovement &&
                    defaults_spec.sweep->bayesian_posterior.initial_design_size == 0 &&
                    defaults_spec.sweep->bayesian_posterior.iterations == 0 &&
                    defaults_spec.sweep->bayesian_posterior.seed == 0 &&
                    defaults_spec.sweep->bayesian_posterior.exploration_weight == 2.0 &&
                    defaults_spec.sweep->bayesian_posterior.normalize_inputs &&
                    defaults_spec.sweep->bayesian_posterior.top_k == 0,
                "milestone E3 parser accepts posterior alias with stable default policy knobs");
        }
        {
            PlanSpec alias_spec{};
            std::string alias_err{};
            const bool alias_parsed = parse_plan_spec_json(
                posterior_minimal_json("bayesian"),
                alias_spec,
                alias_err);
            t.expect(alias_parsed && alias_spec.sweep &&
                    alias_spec.sweep->engine ==
                        PlanSweepEngineKind::BayesianPosterior,
                "milestone E3 parser accepts bayesian alias for the scaffold engine");
        }
        {
            PlanSpec invalid_spec{};
            std::string invalid_err{};
            const bool invalid_parsed = parse_plan_spec_json(
                posterior_invalid_acquisition_json(),
                invalid_spec,
                invalid_err);
            t.expect(!invalid_parsed &&
                    invalid_err.find(
                        "sweep.engine_config.bayesian_posterior.acquisition")
                        != std::string::npos,
                "milestone E3 parser rejects unknown posterior acquisition values with a scoped diagnostic");
        }

        PlanSpec spec{};
        std::string err{};
        const bool parsed =
            parse_plan_spec_json(posterior_contract_json(), spec, err);
        t.expect(parsed,
            "milestone E3 PlanSpec parser accepts bayesian_posterior config");
        if (!parsed || !spec.sweep) {
            return false;
        }

        const PlanSweepSpec& sweep = *spec.sweep;
        t.expect(sweep.engine == PlanSweepEngineKind::BayesianPosterior &&
                sweep.bayesian_posterior.surrogate ==
                    PlanSweepPosteriorSurrogateKind::GaussianProcess &&
                sweep.bayesian_posterior.acquisition ==
                    PlanSweepPosteriorAcquisitionKind::UpperConfidenceBound &&
                sweep.bayesian_posterior.initial_design_size == 5 &&
                sweep.bayesian_posterior.iterations == 7 &&
                sweep.bayesian_posterior.seed == 17 &&
                sweep.bayesian_posterior.exploration_weight == 1.5 &&
                !sweep.bayesian_posterior.normalize_inputs &&
                sweep.bayesian_posterior.top_k == 3,
            "milestone E3 parser preserves typed posterior policy knobs");
        t.expect(sweep.pareto.enabled &&
                sweep.pareto.objectives.size() == 1 &&
                sweep.pareto.front_row_limit == 5 &&
                sweep.pareto.output_depollution.has_value() &&
                !sweep.pareto.output_depollution->enabled &&
                sweep.result_diversity.enabled &&
                sweep.result_diversity.oversample_multiplier == 4,
            "milestone E3 posterior config remains separate from Pareto and output depollution config");

        SweepIndexConfig cfg{};
        cfg.kind = SweepIndexKind::BayesianPosterior;
        cfg.bayesian_posterior.posterior = sweep.bayesian_posterior;
        cfg.bayesian_posterior.max_evaluations = sweep.max_evals;
        const auto posterior_sweep = make_sweep_index(cfg);
        t.expect(posterior_sweep != nullptr &&
                std::string_view(posterior_sweep->name()) ==
                    "bayesian_posterior",
            "milestone E3 factory registers the bayesian_posterior sweep policy");
        if (posterior_sweep) {
            std::size_t direct_eval_calls = 0;
            std::size_t direct_progress_calls = 0;
            std::size_t direct_trace_calls = 0;
            SweepIndexProblem direct_problem{};
            direct_problem.param_count = 4;
            direct_problem.eval = [&](std::size_t) {
                ++direct_eval_calls;
                return BacktestResult{};
                };
            const auto direct_run = posterior_sweep->run(
                direct_problem,
                [](const BacktestResult&) { return 0.0; },
                [&](const SweepProgress&) { ++direct_progress_calls; },
                nullptr,
                [&](const SweepTraceEvent&) { ++direct_trace_calls; });
            t.expect(direct_run.eval_count == 4 &&
                    !direct_run.best_first.empty() &&
                    direct_run.effective_seed_available &&
                    direct_run.has_posterior &&
                    direct_run.posterior.computed &&
                    direct_run.posterior.status ==
                        SweepPosteriorDiagnosticsStatus::Computed &&
                    direct_eval_calls == 4 &&
                    direct_progress_calls > 0 &&
                    direct_trace_calls == 8,
                "milestone E4 factory policy executes deterministic posterior proposals");
        }
        {
            SweepIndexConfig single_cfg{};
            single_cfg.kind = SweepIndexKind::BayesianPosterior;
            single_cfg.bayesian_posterior.max_evaluations = 10;
            single_cfg.bayesian_posterior.posterior.initial_design_size = 5;
            single_cfg.bayesian_posterior.posterior.iterations = 5;
            single_cfg.bayesian_posterior.posterior.seed = 19;
            single_cfg.bayesian_posterior.posterior.top_k = 4;

            const auto single_sweep = make_sweep_index(single_cfg);
            std::size_t single_eval_calls = 0;
            std::vector<std::size_t> single_proposals;
            SweepIndexProblem single_problem{};
            single_problem.param_count = 1;
            single_problem.eval = [&](std::size_t idx) {
                ++single_eval_calls;
                BacktestResult result{};
                result.pnl = 42.0 + static_cast<double>(idx);
                return result;
                };
            const auto single_run = single_sweep->run(
                single_problem,
                [](const BacktestResult& result) {
                    return result.pnl;
                },
                nullptr,
                nullptr,
                [&](const SweepTraceEvent& event) {
                    if (event.kind == SweepTraceEventKind::Proposal) {
                        single_proposals.push_back(event.candidate_index);
                    }
                });
            t.expect(single_run.eval_count == 1 &&
                    single_eval_calls == 1 &&
                    single_proposals.size() == 1 &&
                    single_proposals.front() == 0 &&
                    single_run.best_first.size() == 1 &&
                    single_run.has_posterior &&
                    single_run.posterior.computed &&
                    !single_run.posterior.surrogate_trained &&
                    !single_run.posterior.acquisition_ready &&
                    single_run.posterior.training_observation_count == 1 &&
                    single_run.posterior.finite_observation_count == 1 &&
                    single_run.posterior.surrogate_training_observation_count == 1 &&
                    single_run.posterior.invalid_training_observation_count == 0 &&
                    single_run.posterior.proposal_reasons.size() == 1,
                "milestone E4 posterior handles one-candidate spaces without duplicate proposals or false acquisition readiness");
        }
        {
            SweepIndexConfig nonfinite_cfg{};
            nonfinite_cfg.kind = SweepIndexKind::BayesianPosterior;
            nonfinite_cfg.bayesian_posterior.max_evaluations = 8;
            nonfinite_cfg.bayesian_posterior.posterior.initial_design_size = 3;
            nonfinite_cfg.bayesian_posterior.posterior.iterations = 5;
            nonfinite_cfg.bayesian_posterior.posterior.seed = 23;
            nonfinite_cfg.bayesian_posterior.posterior.top_k = 2;

            const auto nonfinite_sweep = make_sweep_index(nonfinite_cfg);
            std::size_t eval_calls = 0;
            std::vector<std::size_t> proposed_indices;
            SweepIndexProblem nonfinite_problem{};
            nonfinite_problem.param_count = 16;
            nonfinite_problem.eval = [&](std::size_t) {
                ++eval_calls;
                return BacktestResult{};
                };
            const auto nonfinite_run = nonfinite_sweep->run(
                nonfinite_problem,
                [](const BacktestResult&) {
                    return std::numeric_limits<double>::quiet_NaN();
                },
                nullptr,
                nullptr,
                [&](const SweepTraceEvent& event) {
                    if (event.kind == SweepTraceEventKind::Proposal) {
                        proposed_indices.push_back(event.candidate_index);
                    }
                });
            std::vector<std::size_t> unique_indices = proposed_indices;
            std::sort(unique_indices.begin(), unique_indices.end());
            unique_indices.erase(
                std::unique(unique_indices.begin(), unique_indices.end()),
                unique_indices.end());
            const bool used_untrained_fallback = std::any_of(
                nonfinite_run.posterior.diagnostics.begin(),
                nonfinite_run.posterior.diagnostics.end(),
                [](const std::string& diagnostic) {
                    return diagnostic ==
                        "untrained_surrogate_deterministic_exploration";
                });
            t.expect(nonfinite_run.eval_count == 8 &&
                    eval_calls == 8 &&
                    nonfinite_run.best_first.empty() &&
                    nonfinite_run.has_posterior &&
                    nonfinite_run.posterior.computed &&
                    !nonfinite_run.posterior.surrogate_trained &&
                    nonfinite_run.posterior.training_observation_count == 0 &&
                    !nonfinite_run.posterior.acquisition_ready &&
                    nonfinite_run.posterior.proposal_reasons.size() == 8 &&
                    proposed_indices.size() == 8 &&
                    unique_indices.size() == proposed_indices.size() &&
                    used_untrained_fallback,
                "milestone E4 untrained posterior fallback keeps deterministic exploration moving across non-finite observations");
        }
        {
            SweepIndexConfig large_cfg{};
            large_cfg.kind = SweepIndexKind::BayesianPosterior;
            large_cfg.bayesian_posterior.max_evaluations = 10;
            large_cfg.bayesian_posterior.posterior.initial_design_size = 6;
            large_cfg.bayesian_posterior.posterior.iterations = 4;
            large_cfg.bayesian_posterior.posterior.seed = 29;
            large_cfg.bayesian_posterior.posterior.top_k = 3;
            large_cfg.bayesian_posterior.posterior.acquisition =
                PlanSweepPosteriorAcquisitionKind::UpperConfidenceBound;

            const auto large_sweep = make_sweep_index(large_cfg);
            std::size_t eval_calls = 0;
            std::size_t neighbor_calls = 0;
            std::vector<std::size_t> proposed_indices;
            SweepIndexProblem large_problem{};
            large_problem.param_count = 5000;
            large_problem.eval = [&](std::size_t idx) {
                ++eval_calls;
                BacktestResult result{};
                result.pnl = static_cast<double>(idx % 101);
                return result;
                };
            large_problem.features = [&](std::size_t idx) {
                return std::vector<double>{
                    static_cast<double>(idx) /
                    static_cast<double>(large_problem.param_count - 1)
                };
                };
            large_problem.neighbors = [&](std::size_t idx) {
                ++neighbor_calls;
                std::vector<std::size_t> out{ idx };
                if (idx > 0) {
                    out.push_back(idx - 1);
                }
                if (idx + 1 < large_problem.param_count) {
                    out.push_back(idx + 1);
                }
                return out;
                };
            const auto large_run = large_sweep->run(
                large_problem,
                [](const BacktestResult& result) {
                    return result.pnl;
                },
                nullptr,
                nullptr,
                [&](const SweepTraceEvent& event) {
                    if (event.kind == SweepTraceEventKind::Proposal) {
                        proposed_indices.push_back(event.candidate_index);
                    }
                });
            std::vector<std::size_t> unique_indices = proposed_indices;
            std::sort(unique_indices.begin(), unique_indices.end());
            unique_indices.erase(
                std::unique(unique_indices.begin(), unique_indices.end()),
                unique_indices.end());
            const bool used_neighbor_enrichment = std::any_of(
                large_run.posterior.diagnostics.begin(),
                large_run.posterior.diagnostics.end(),
                [](const std::string& diagnostic) {
                    return diagnostic ==
                        "acquisition_pool_local_neighbor_enrichment";
                });
            const bool reported_local_limit = std::any_of(
                large_run.posterior.diagnostics.begin(),
                large_run.posterior.diagnostics.end(),
                [](const std::string& diagnostic) {
                    return diagnostic ==
                        "acquisition_local_neighbor_pool_limit=512";
                });
            t.expect(large_run.eval_count == 10 &&
                    eval_calls == 10 &&
                    neighbor_calls > 0 &&
                    large_run.has_posterior &&
                    large_run.posterior.computed &&
                    large_run.posterior.surrogate_trained &&
                    large_run.posterior.acquisition_ready &&
                    large_run.posterior.proposal_reasons.size() == 10 &&
                    proposed_indices.size() == 10 &&
                    unique_indices.size() == proposed_indices.size() &&
                    used_neighbor_enrichment &&
                    reported_local_limit,
                "milestone E4 large posterior spaces enrich the bounded acquisition pool with deterministic local neighbors");
        }
        {
            SweepIndexConfig fallback_cfg{};
            fallback_cfg.kind = SweepIndexKind::BayesianPosterior;
            fallback_cfg.bayesian_posterior.max_evaluations = 6;
            fallback_cfg.bayesian_posterior.posterior.initial_design_size = 3;
            fallback_cfg.bayesian_posterior.posterior.iterations = 3;
            fallback_cfg.bayesian_posterior.posterior.seed = 31;
            fallback_cfg.bayesian_posterior.posterior.top_k = 2;

            const auto fallback_sweep = make_sweep_index(fallback_cfg);
            std::unordered_set<std::size_t> evaluated_indices;
            std::vector<std::string> proposal_reasons;
            SweepIndexProblem fallback_problem{};
            fallback_problem.param_count = 12;
            fallback_problem.eval = [&](std::size_t idx) {
                evaluated_indices.insert(idx);
                BacktestResult result{};
                result.pnl = static_cast<double>(idx);
                return result;
                };
            fallback_problem.features = [&](std::size_t idx) {
                if (evaluated_indices.find(idx) != evaluated_indices.end()) {
                    return std::vector<double>{
                        static_cast<double>(idx) /
                        static_cast<double>(fallback_problem.param_count - 1)
                    };
                }
                return std::vector<double>{
                    std::numeric_limits<double>::quiet_NaN()
                };
                };
            const auto fallback_run = fallback_sweep->run(
                fallback_problem,
                [](const BacktestResult& result) {
                    return result.pnl;
                },
                nullptr,
                nullptr,
                [&](const SweepTraceEvent& event) {
                    if (event.kind == SweepTraceEventKind::Proposal) {
                        proposal_reasons.push_back(event.reason);
                    }
                });
            const bool used_acquisition_fallback = std::any_of(
                fallback_run.posterior.diagnostics.begin(),
                fallback_run.posterior.diagnostics.end(),
                [](const std::string& diagnostic) {
                    return diagnostic ==
                        "acquisition_unavailable_deterministic_fallback";
                });
            const bool proposal_reason_visible = std::any_of(
                proposal_reasons.begin(),
                proposal_reasons.end(),
                [](const std::string& reason) {
                    return reason ==
                        "bayesian_posterior_acquisition_unavailable_fallback";
                });
            const bool fallback_reason_has_no_acquisition_score = std::any_of(
                fallback_run.posterior.proposal_reasons.begin(),
                fallback_run.posterior.proposal_reasons.end(),
                [](const SweepPosteriorProposalReason& reason) {
                    return reason.reason ==
                        "bayesian_posterior_acquisition_unavailable_fallback" &&
                        !reason.acquisition_score_present;
                });
            t.expect(fallback_run.eval_count == 6 &&
                    fallback_run.has_posterior &&
                    fallback_run.posterior.computed &&
                    fallback_run.posterior.surrogate_trained &&
                    fallback_run.posterior.acquisition_ready &&
                    fallback_run.posterior.training_observation_count == 6 &&
                    fallback_run.posterior.proposal_reasons.size() == 6 &&
                    used_acquisition_fallback &&
                    proposal_reason_visible &&
                    fallback_reason_has_no_acquisition_score,
                "milestone E4 posterior falls back deterministically when trained surrogate acquisition scores are unavailable");
        }
        {
            SweepIndexConfig filtered_cfg{};
            filtered_cfg.kind = SweepIndexKind::BayesianPosterior;
            filtered_cfg.bayesian_posterior.max_evaluations = 8;
            filtered_cfg.bayesian_posterior.posterior.initial_design_size = 5;
            filtered_cfg.bayesian_posterior.posterior.iterations = 3;
            filtered_cfg.bayesian_posterior.posterior.seed = 37;
            filtered_cfg.bayesian_posterior.posterior.top_k = 3;

            const auto filtered_sweep = make_sweep_index(filtered_cfg);
            std::unordered_set<std::size_t> evaluated_indices;
            std::size_t observed_feature_calls = 0;
            std::size_t invalid_feature_observations = 0;
            SweepIndexProblem filtered_problem{};
            filtered_problem.param_count = 14;
            filtered_problem.eval = [&](std::size_t idx) {
                evaluated_indices.insert(idx);
                BacktestResult result{};
                result.pnl =
                    100.0 -
                    std::abs(7.0 - static_cast<double>(idx));
                return result;
                };
            filtered_problem.features = [&](std::size_t idx) {
                if (evaluated_indices.find(idx) != evaluated_indices.end()) {
                    ++observed_feature_calls;
                    if (observed_feature_calls == 1 ||
                        observed_feature_calls == 3) {
                        ++invalid_feature_observations;
                        return std::vector<double>{
                            std::numeric_limits<double>::quiet_NaN()
                        };
                    }
                    if (observed_feature_calls == 5) {
                        ++invalid_feature_observations;
                        return std::vector<double>{
                            static_cast<double>(idx),
                            static_cast<double>(idx + 1)
                        };
                    }
                }
                return std::vector<double>{
                    static_cast<double>(idx) /
                    static_cast<double>(filtered_problem.param_count - 1)
                };
                };
            const auto filtered_run = filtered_sweep->run(
                filtered_problem,
                [](const BacktestResult& result) {
                    return result.pnl;
                },
                nullptr,
                nullptr,
                nullptr);
            const bool reported_feature_filter = std::any_of(
                filtered_run.posterior.diagnostics.begin(),
                filtered_run.posterior.diagnostics.end(),
                [](const std::string& diagnostic) {
                    return diagnostic == "filtered_invalid_training_features";
                });
            const bool acquired_after_filtering = std::any_of(
                filtered_run.posterior.proposal_reasons.begin(),
                filtered_run.posterior.proposal_reasons.end(),
                [](const SweepPosteriorProposalReason& reason) {
                    return reason.acquisition_score_present &&
                        reason.acquisition_score_finite;
                });
            t.expect(filtered_run.eval_count == 8 &&
                    observed_feature_calls == 8 &&
                    invalid_feature_observations == 3 &&
                    filtered_run.has_posterior &&
                    filtered_run.posterior.computed &&
                    filtered_run.posterior.surrogate_trained &&
                    filtered_run.posterior.acquisition_ready &&
                    filtered_run.posterior.training_observation_count == 8 &&
                    filtered_run.posterior.finite_observation_count == 8 &&
                    filtered_run.posterior.surrogate_training_observation_count == 5 &&
                    filtered_run.posterior.invalid_training_observation_count == 3 &&
                    filtered_run.posterior.proposal_reasons.size() == 8 &&
                    reported_feature_filter &&
                    acquired_after_filtering,
                "milestone E4 posterior filters invalid training features while retaining acquisition behavior");
        }

        std::vector<double> close;
        close.reserve(16);
        for (std::size_t i = 0; i < 16; ++i) {
            close.push_back(100.0 + static_cast<double>(i));
        }
        std::shared_ptr<align::AlignedBundle> bundle;
        ColumnRef primary{};
        const bool okdv = make_single_series_dataview_from_close(
            close,
            bundle,
            primary);
        t.expect(okdv && bundle != nullptr,
            "milestone E3 posterior fixture creates temporal DataView input");
        if (!okdv || !bundle) {
            return false;
        }

        std::ostringstream log;
        BacktestResult best{};
        PlanSweepReport report{};
        const bool ok = run_custom_plan_spec(
            DataView(*bundle),
            primary,
            spec,
            PlanSweepRuntimeOptions{},
            log,
            &best,
            &report);
        t.expect(ok,
            "milestone E4 bayesian_posterior executes through the PlanSpec runtime");
        t.expect(log.str().find("Bayesian posterior diagnostics:") !=
                std::string::npos &&
                log.str().find("status: computed") != std::string::npos,
            "milestone E5 full report shows computed posterior diagnostics");
        t.expect(report.requested_sweep.engine ==
                PlanSweepEngineKind::BayesianPosterior &&
                report.evaluations_run == 12,
            "milestone E4 report preserves posterior engine execution accounting");
        t.expect(report.posterior.requested &&
                report.posterior.computed &&
                report.posterior.status ==
                    SweepPosteriorDiagnosticsStatus::Computed &&
                report.posterior.unsupported_reason.empty(),
            "milestone E4 report marks posterior diagnostics computed");
        t.expect(report.posterior.surrogate ==
                PlanSweepPosteriorSurrogateKind::GaussianProcess &&
                report.posterior.acquisition ==
                    PlanSweepPosteriorAcquisitionKind::UpperConfidenceBound &&
                report.posterior.initial_design_size == 5 &&
                report.posterior.iterations == 7 &&
                report.posterior.seed == 17 &&
                report.posterior.top_k == 8 &&
                !report.posterior.normalize_inputs,
            "milestone E4 report preserves effective posterior policy config");
        t.expect(report.posterior.surrogate_trained &&
                report.posterior.training_observation_count == 12 &&
                report.posterior.finite_observation_count == 12 &&
                report.posterior.surrogate_training_observation_count == 12 &&
                report.posterior.invalid_training_observation_count == 0 &&
                report.posterior.acquisition_ready &&
                report.posterior.proposal_reasons.size() == 12,
            "milestone E4 report preserves computed posterior training diagnostics");
        t.expect(report.pareto.requested &&
                report.pareto.computed &&
                report.pareto.front_row_limit == 5 &&
                report.pareto.observed_candidate_count == 12 &&
                report.result_diversity.retained_candidate_pool > 0,
            "milestone E4 posterior results flow into Pareto and result-diversity reporting");
        t.expect(report.proposal_trace.captured &&
                report.proposal_trace.policy_name == "bayesian_posterior" &&
                report.proposal_trace.total_event_count == 24 &&
                report.proposal_trace.proposal_count == 12 &&
                report.proposal_trace.observation_count == 12 &&
                report.proposal_trace.exact_replay_from_report &&
                has_policy_config(
                    report.proposal_trace,
                    "surrogate",
                    SweepTracePolicyConfigValueKind::String,
                    "gaussian_process") &&
                has_policy_config(
                    report.proposal_trace,
                    "acquisition",
                    SweepTracePolicyConfigValueKind::String,
                    "upper_confidence_bound") &&
                has_policy_config(
                    report.proposal_trace,
                    "initial_design_size",
                    SweepTracePolicyConfigValueKind::UnsignedInteger,
                    "5") &&
                has_policy_config(
                    report.proposal_trace,
                    "normalize_inputs",
                    SweepTracePolicyConfigValueKind::Boolean,
                    "false") &&
                has_policy_config(
                    report.proposal_trace,
                    "seed",
                    SweepTracePolicyConfigValueKind::UnsignedInteger,
                    "17") &&
                has_policy_config(
                    report.proposal_trace,
                    "exploration_weight",
                    SweepTracePolicyConfigValueKind::FloatingPoint,
                    "1.500000") &&
                has_policy_config(
                    report.proposal_trace,
                    "top_k",
                    SweepTracePolicyConfigValueKind::UnsignedInteger,
                    "8") &&
                report.proposal_trace.seeds.size() == 1 &&
                report.proposal_trace.seeds.front().stream_name ==
                    "candidate_proposal" &&
                report.proposal_trace.seeds.front().requested_seed == 17 &&
                report.proposal_trace.seeds.front().effective_seed == 17,
            "milestone E4 posterior trace echoes effective policy config and proposal events");

        std::ostringstream json;
        print_evaluation_report(
            make_sweep_evaluation_report(report),
            EvaluationReportProjection::Json,
            json);
        const std::string json_text = json.str();
        t.expect(json_text.find("\"engine\":\"bayesian_posterior\"") !=
                std::string::npos &&
                json_text.find("\"posterior\":{") != std::string::npos &&
                json_text.find("\"status\":\"computed\"") != std::string::npos &&
                json_text.find("\"surrogate\":\"gaussian_process\"") !=
                    std::string::npos &&
                json_text.find("\"acquisition\":\"upper_confidence_bound\"") !=
                    std::string::npos &&
                json_text.find("\"unsupported_reason\":\"\"") !=
                    std::string::npos &&
                json_text.find("\"initial_design_size\":5") !=
                    std::string::npos &&
                json_text.find("\"iterations\":7") != std::string::npos &&
                json_text.find("\"seed\":17") != std::string::npos &&
                json_text.find("\"exploration_weight\":1.5") !=
                    std::string::npos &&
                json_text.find("\"normalize_inputs\":false") !=
                    std::string::npos &&
                json_text.find("\"top_k\":8") != std::string::npos &&
                json_text.find("\"surrogate_trained\":true") !=
                    std::string::npos &&
                json_text.find("\"training_observation_count\":12") !=
                    std::string::npos &&
                json_text.find("\"finite_observation_count\":12") !=
                    std::string::npos &&
                json_text.find("\"surrogate_training_observation_count\":12") !=
                    std::string::npos &&
                json_text.find("\"invalid_training_observation_count\":0") !=
                    std::string::npos &&
                json_text.find("\"acquisition_ready\":true") !=
                    std::string::npos &&
                json_text.find("\"gaussian_process_rbf_surrogate\"") !=
                    std::string::npos &&
                json_text.find("\"proposal_reasons\":[") !=
                    std::string::npos,
            "milestone E4 JSON projection exposes computed posterior diagnostics fields");

        std::ostringstream text;
        print_plan_sweep_report(report, text);
        const std::string full_text = text.str();
        t.expect(full_text.find("Engine: bayesian_posterior") !=
                std::string::npos &&
                full_text.find("Bayesian posterior diagnostics:") !=
                    std::string::npos &&
                full_text.find("status: computed") != std::string::npos &&
                full_text.find("policy config:") != std::string::npos &&
                full_text.find("surrogate: gaussian_process") !=
                    std::string::npos &&
                full_text.find("acquisition: upper_confidence_bound") !=
                    std::string::npos &&
                full_text.find("training data:") != std::string::npos &&
                full_text.find("training observations: 12") !=
                    std::string::npos &&
                full_text.find("finite observations: 12") !=
                    std::string::npos &&
                full_text.find("surrogate training rows: 12") !=
                    std::string::npos &&
                full_text.find("invalid feature observations filtered: 0") !=
                    std::string::npos &&
                full_text.find("proposal summary:") !=
                    std::string::npos &&
                full_text.find("proposal decision details:") !=
                    std::string::npos &&
                full_text.find("acquisition ready: true") !=
                    std::string::npos &&
                full_text.find("proposal decisions: 12") !=
                    std::string::npos,
            "milestone E5 full text report exposes computed posterior diagnostics schema");

        std::ostringstream posterior_text_out;
        print_evaluation_report(
            make_sweep_evaluation_report(report),
            EvaluationReportProjection::PosteriorDiagnostics,
            posterior_text_out);
        const std::string posterior_text = posterior_text_out.str();
        t.expect(posterior_text.find("Bayesian posterior diagnostics:") == 0 &&
                posterior_text.find("policy config:") != std::string::npos &&
                posterior_text.find("training data:") != std::string::npos &&
                posterior_text.find("proposal summary:") != std::string::npos &&
                posterior_text.find("proposal decision details:") != std::string::npos &&
                posterior_text.find("=== Plan Sweep Report ===") == std::string::npos &&
                posterior_text.find("TopK:") == std::string::npos,
            "milestone E5 focused posterior projection exposes only posterior diagnostics");

        {
            PlanSweepReport unsupported_report{};
            unsupported_report.requested_sweep.enabled = true;
            unsupported_report.requested_sweep.engine =
                PlanSweepEngineKind::BayesianPosterior;
            unsupported_report.posterior.requested = true;
            unsupported_report.posterior.computed = false;
            unsupported_report.posterior.status =
                SweepPosteriorDiagnosticsStatus::Unsupported;
            unsupported_report.posterior.unsupported_reason =
                "bayesian_posterior requires a non-empty indexable candidate space";
            std::ostringstream unsupported_text_out;
            print_evaluation_report(
                make_sweep_evaluation_report(unsupported_report),
                EvaluationReportProjection::PosteriorDiagnostics,
                unsupported_text_out);
            const std::string unsupported_text = unsupported_text_out.str();
            t.expect(unsupported_text.find("Bayesian posterior diagnostics:") == 0 &&
                    unsupported_text.find("computed: false") != std::string::npos &&
                    unsupported_text.find("status: unsupported") != std::string::npos &&
                    unsupported_text.find("unsupported reason: bayesian_posterior requires a non-empty indexable candidate space") !=
                        std::string::npos &&
                    unsupported_text.find("TopK:") == std::string::npos,
                "milestone E5 focused posterior projection preserves unsupported diagnostics when available");
        }

        {
            std::ostringstream second_log;
            BacktestResult second_best{};
            PlanSweepReport second_report{};
            const bool second_ok = run_custom_plan_spec(
                DataView(*bundle),
                primary,
                spec,
                PlanSweepRuntimeOptions{},
                second_log,
                &second_best,
                &second_report);
            t.expect(second_ok &&
                    second_report.evaluations_run == report.evaluations_run &&
                    second_report.proposal_trace.trace_digest_available &&
                    report.proposal_trace.trace_digest_available &&
                    second_report.proposal_trace.trace_digest ==
                        report.proposal_trace.trace_digest &&
                    second_report.posterior.proposal_reasons.size() ==
                        report.posterior.proposal_reasons.size() &&
                    top_candidates_equal(
                        second_report.top_candidates,
                        report.top_candidates),
                "milestone E4 seeded posterior runs are reproducible");
        }

        {
            bool cancel_after_progress = false;
            PlanSweepRuntimeOptions cancel_options{};
            cancel_options.progress_cb = [&](const SweepProgress& progress) {
                if (progress.stage == SweepProgressStage::AdaptiveSelection &&
                    progress.stage_done >= 10) {
                    cancel_after_progress = true;
                }
                };
            cancel_options.cancel_requested = [&]() {
                return cancel_after_progress;
                };

            std::ostringstream canceled_log;
            BacktestResult canceled_best{};
            PlanSweepReport canceled_report{};
            const bool canceled_ok = run_custom_plan_spec(
                DataView(*bundle),
                primary,
                spec,
                cancel_options,
                canceled_log,
                &canceled_best,
                &canceled_report);
            t.expect(!canceled_ok &&
                    canceled_report.canceled &&
                    canceled_log.str().find("PlanSpec execution canceled.") !=
                        std::string::npos &&
                    canceled_report.evaluations_run == 10 &&
                    canceled_report.posterior.requested &&
                    canceled_report.posterior.computed &&
                    canceled_report.posterior.training_observation_count == 10 &&
                    canceled_report.posterior.proposal_reasons.size() == 10 &&
                    canceled_report.proposal_trace.proposal_count == 10 &&
                    canceled_report.proposal_trace.observation_count == 10 &&
                    canceled_report.proposal_trace.exact_replay_from_report,
                "milestone E4 cancellation preserves partial posterior diagnostics");

            std::ostringstream canceled_text_out;
            print_evaluation_report(
                make_sweep_evaluation_report(canceled_report),
                EvaluationReportProjection::PosteriorDiagnostics,
                canceled_text_out);
            const std::string canceled_text = canceled_text_out.str();
            t.expect(canceled_text.find("Bayesian posterior diagnostics:") == 0 &&
                    canceled_text.find("training observations: 10") != std::string::npos &&
                    canceled_text.find("finite observations: 10") != std::string::npos &&
                    canceled_text.find("proposal decisions: 10") != std::string::npos &&
                    canceled_text.find("proposal decision details:") != std::string::npos &&
                    canceled_text.find("TopK:") == std::string::npos,
                "milestone E5 focused posterior projection preserves partial canceled diagnostics");
        }

        return true;
    }

} // namespace sugar::contract_suite_internal
