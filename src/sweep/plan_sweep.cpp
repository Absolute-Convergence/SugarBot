#include "sweep/plan_sweep.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ingest/data_view.h"
#include "sweep/metrics.h"
#include "plan_sweep_candidate_index.h"
#include "plan_sweep_geometry.h"
#include "plan_sweep_objective.h"
#include "plan_sweep_pareto.h"
#include "plan_sweep_result_diversity.h"
#include "plan_sweep_runtime_adapter.h"
#include "plan_sweep_runtime_validation.h"
#include "plan_sweep_trace.h"
#include "sweep/sweep.h"

namespace sugar {

    namespace {
        void apply_runtime_sweep_defaults(PlanSweepSpec& sweep) {
            if (sweep.engine == PlanSweepEngineKind::MonteCarlo &&
                !sweep.monte_carlo.draws_explicit) {
                sweep.monte_carlo.draws = sweep.max_evals;
            }
        }

        void upsert_stage_summary(
            PlanSweepReport& report,
            SweepProgressStage stage,
            std::size_t done,
            std::size_t total) {
            auto it = std::find_if(
                report.stage_summaries.begin(),
                report.stage_summaries.end(),
                [stage](const SweepStageSummary& summary) {
                    return summary.stage == stage;
                });
            if (it == report.stage_summaries.end()) {
                report.stage_summaries.push_back(SweepStageSummary{ stage, done, total });
                return;
            }
            it->done = done;
            it->total = total;
        }

        void record_trace_config_string(
            PlanSweepTraceRecorder& recorder,
            std::string name,
            std::string value) {
            recorder.record_policy_config(
                std::move(name),
                SweepTracePolicyConfigValueKind::String,
                std::move(value));
        }

        void record_trace_config_bool(
            PlanSweepTraceRecorder& recorder,
            std::string name,
            bool value) {
            recorder.record_policy_config(
                std::move(name),
                SweepTracePolicyConfigValueKind::Boolean,
                value ? "true" : "false");
        }

        void record_trace_config_u64(
            PlanSweepTraceRecorder& recorder,
            std::string name,
            std::uint64_t value) {
            recorder.record_policy_config(
                std::move(name),
                SweepTracePolicyConfigValueKind::UnsignedInteger,
                std::to_string(value));
        }

        void record_trace_config_double(
            PlanSweepTraceRecorder& recorder,
            std::string name,
            double value) {
            recorder.record_policy_config(
                std::move(name),
                SweepTracePolicyConfigValueKind::FloatingPoint,
                std::to_string(value));
        }

        void record_common_trace_config(
            PlanSweepTraceRecorder& recorder,
            std::size_t candidate_space_size,
            std::size_t retained_top_k) {
            record_trace_config_u64(
                recorder,
                "candidate_space_size",
                candidate_space_size);
            record_trace_config_u64(recorder, "retained_top_k", retained_top_k);
        }

        struct TraceObservationPayload {
            RunOutcomeState outcome{ RunOutcomeState::Completed };
            RunRankabilityState rankability{ RunRankabilityState::NotEvaluated };
        };

        void record_posterior_trace_config(
            PlanSweepTraceRecorder& recorder,
            const PlanSweepBayesianPosteriorConfig& posterior) {
            record_trace_config_string(
                recorder,
                "surrogate",
                std::string(plan_sweep_posterior_surrogate_kind_name(
                    posterior.surrogate)));
            record_trace_config_string(
                recorder,
                "acquisition",
                std::string(plan_sweep_posterior_acquisition_kind_name(
                    posterior.acquisition)));
            record_trace_config_u64(
                recorder,
                "initial_design_size",
                posterior.initial_design_size);
            record_trace_config_u64(
                recorder,
                "iterations",
                posterior.iterations);
            record_trace_config_u64(
                recorder,
                "seed",
                posterior.seed);
            record_trace_config_double(
                recorder,
                "exploration_weight",
                posterior.exploration_weight);
            record_trace_config_bool(
                recorder,
                "normalize_inputs",
                posterior.normalize_inputs);
            record_trace_config_u64(recorder, "top_k", posterior.top_k);
        }

    } // namespace

    bool run_custom_plan_spec(
        const DataView& dv,
        const ColumnRef& primary,
        const PlanSpec& spec,
        const PlanSweepRuntimeOptions& opt,
        std::ostream& out,
        BacktestResult* result_out,
        PlanSweepReport* sweep_report_out,
        const RunProvenanceSummary* run_provenance)
    {
        if (!dv.has_col(primary)) {
            out << "PlanSpec input error: primary series/column is missing from DataView.\n";
            return false;
        }
        if (opt.cancel_requested && opt.cancel_requested()) {
            out << "PlanSpec execution canceled.\n";
            return false;
        }
        PlanSpec normalized_spec = spec;
        normalize_plan_sweep_runtime_aliases(normalized_spec);
        const bool has_sweep =
            normalized_spec.sweep &&
            (normalized_spec.sweep->enabled || !normalized_spec.sweep->params.empty());
        const RankingObjectiveSpec sweep_objective =
            has_sweep ? resolve_plan_sweep_objective(*normalized_spec.sweep) : RankingObjectiveSpec{};
        const bool collect_numeric_outputs =
            has_sweep &&
            (plan_sweep_objective_needs_numeric_outputs(sweep_objective) ||
                plan_sweep_pareto_needs_numeric_outputs(*normalized_spec.sweep));
        if (sweep_report_out) {
            *sweep_report_out = PlanSweepReport{};
        }
        const std::string default_series = primary.series;
        std::string runtime_validation_err;
        if (!validate_planspec_runtime_requirements_for_dataview(
            normalized_spec, dv, default_series, nullptr, runtime_validation_err)) {
            out << "PlanSpec runtime validation failed: " << runtime_validation_err << "\n";
            return false;
        }
        const ColumnId primary_col = primary.col;

        CompiledPlanRuntimeTemplate runtime_template{};
        std::string compile_err;
        if (!build_compiled_plan_runtime_template(
            normalized_spec,
            dv,
            default_series,
            collect_numeric_outputs,
            runtime_template,
            compile_err)) {
            out << "PlanSpec compile failed: " << compile_err << "\n";
            return false;
        }
        if (opt.cancel_requested && opt.cancel_requested()) {
            out << "PlanSpec execution canceled.\n";
            return false;
        }

        auto run_one = [&](const ParamMap* overrides, BacktestResult& result_out, std::string& err_out) -> bool {
            if (!validate_planspec_runtime_requirements_for_dataview(
                normalized_spec, dv, default_series, overrides, err_out)) {
                return false;
            }
            return run_instantiated_plan_once(
                runtime_template,
                overrides,
                dv,
                default_series,
                primary_col,
                opt,
                result_out,
                err_out);
        };

        if (normalized_spec.sweep && (normalized_spec.sweep->enabled || !normalized_spec.sweep->params.empty())) {
            PlanSweepSpec sw = *normalized_spec.sweep;
            apply_runtime_sweep_defaults(sw);

            PlanSpecSweepDims dims;
            std::string sweep_err;
            if (!build_planspec_sweep_dims(sw, dims, sweep_err)) {
                out << "PlanSpec sweep error: " << sweep_err << "\n";
                return false;
            }

            const auto sweep_started = std::chrono::steady_clock::now();
            const std::size_t total_combos = dims.total_combinations();
            const bool total_combinations_overflow =
                (total_combos == std::numeric_limits<std::size_t>::max());
            const std::size_t bounded_candidate_space_size =
                total_combinations_overflow ? sw.max_evals : std::min(sw.max_evals, total_combos);
            const bool monte_carlo_engine = (sw.engine == PlanSweepEngineKind::MonteCarlo);
            const bool coarse_fine_engine = (sw.engine == PlanSweepEngineKind::CoarseFine);
            const bool successive_halving_engine =
                (sw.engine == PlanSweepEngineKind::SuccessiveHalving);
            const bool bayesian_posterior_engine =
                (sw.engine == PlanSweepEngineKind::BayesianPosterior);
            if ((monte_carlo_engine || coarse_fine_engine ||
                    successive_halving_engine || bayesian_posterior_engine) &&
                total_combinations_overflow) {
                out << "PlanSpec sweep error: "
                    << (monte_carlo_engine
                        ? "monte_carlo"
                        : successive_halving_engine
                        ? "successive_halving"
                        : bayesian_posterior_engine
                        ? "bayesian_posterior"
                        : "coarse_fine")
                    << " requires an indexable full cartesian grid.\n";
                return false;
            }
            std::size_t engine_candidate_space_size = bounded_candidate_space_size;
            bool candidate_space_truncated =
                total_combinations_overflow || bounded_candidate_space_size < total_combos;
            if (monte_carlo_engine) {
                engine_candidate_space_size = total_combos;
                candidate_space_truncated = false;
            }
            else if (coarse_fine_engine) {
                engine_candidate_space_size = total_combos;
            }
            else if (successive_halving_engine) {
                engine_candidate_space_size = total_combos;
                candidate_space_truncated = false;
            }
            else if (bayesian_posterior_engine) {
                engine_candidate_space_size = total_combos;
                candidate_space_truncated = false;
            }

            PlanSweepReport sweep_report{};
            sweep_report.requested_sweep = sw;
            sweep_report.resolved_objective = sweep_objective;
            if (run_provenance != nullptr) {
                sweep_report.run_provenance = *run_provenance;
            }
            sweep_report.parameter_ranges = build_plan_sweep_parameter_range_summaries(sw, dims);
            sweep_report.parameter_count = dims.names.size();
            sweep_report.total_combinations_overflow = total_combinations_overflow;
            if (!sweep_report.total_combinations_overflow) {
                sweep_report.total_combinations = total_combos;
            }
            sweep_report.candidate_space_size = engine_candidate_space_size;
            sweep_report.candidate_space_truncated = candidate_space_truncated;

            std::size_t compile_failures = 0;
            std::string last_compile_error;
            std::size_t execution_failures = 0;
            std::string last_execution_failure;
            PlanSweepParetoObservationCollector pareto_collector(sw);
            PlanSweepTraceRecorder trace_recorder;
            std::unordered_map<std::size_t, TraceObservationPayload>
                trace_observation_payloads;
            const SweepTraceCallback trace_cb =
                [&](const SweepTraceEvent& event) {
                    SweepTraceEvent enriched = event;
                    if (enriched.kind == SweepTraceEventKind::Observation) {
                        const auto it =
                            trace_observation_payloads.find(enriched.candidate_index);
                        if (it != trace_observation_payloads.end()) {
                            enriched.run_outcome_present = true;
                            enriched.run_outcome = it->second.outcome;
                            enriched.run_rankability_present = true;
                            enriched.run_rankability = it->second.rankability;
                        }
                    }
                    trace_recorder.record_event(enriched);
                };

            using IndexVec = std::vector<std::size_t>;

            auto eval_index = [&](std::size_t idx) -> BacktestResult {
                if (idx >= engine_candidate_space_size) {
                    BacktestResult bad{};
                    bad.pnl = -std::numeric_limits<double>::infinity();
                    bad.max_drawdown = std::numeric_limits<double>::infinity();
                    trace_observation_payloads[idx] = TraceObservationPayload{
                        RunOutcomeState::Failed,
                        RunRankabilityState::NotEvaluated
                    };
                    return bad;
                }

                const IndexVec p = decode_plan_sweep_candidate_point(dims, idx);
                ParamMap overrides{};

                for (std::size_t i = 0; i < dims.names.size(); ++i) {
                    const std::string& name = dims.names[i];
                    const double v = dims.values[i][p[i]];
                    if (dims.is_int[i]) overrides[name] = static_cast<std::int64_t>(std::llround(v));
                    else overrides[name] = v;
                }

                BacktestResult r{};
                std::string err_run;
                if (!run_one(&overrides, r, err_run)) {
                    const bool canceled =
                        (opt.cancel_requested && opt.cancel_requested()) ||
                        err_run.find("canceled") != std::string::npos;
                    if (canceled) {
                        increment_run_outcome_counts(
                            sweep_report.candidate_outcomes,
                            RunOutcomeState::Canceled);
                        trace_observation_payloads[idx] = TraceObservationPayload{
                            RunOutcomeState::Canceled,
                            RunRankabilityState::NotEvaluated
                        };
                    }
                    else {
                        ++compile_failures;
                        ++execution_failures;
                        last_compile_error = err_run;
                        last_execution_failure = err_run;
                        increment_run_outcome_counts(
                            sweep_report.candidate_outcomes,
                            RunOutcomeState::Failed);
                        trace_observation_payloads[idx] = TraceObservationPayload{
                            RunOutcomeState::Failed,
                            RunRankabilityState::NotEvaluated
                        };
                    }
                    increment_run_rankability_counts(
                        sweep_report.candidate_rankability,
                        RunRankabilityState::NotEvaluated);
                    r.pnl = -std::numeric_limits<double>::infinity();
                    r.max_drawdown = std::numeric_limits<double>::infinity();
                    r.trades = 0;
                    r.trade_hash = 0;
                }
                else {
                    if (!err_run.empty()) last_compile_error = err_run;
                    const RunReport report =
                        build_run_report(r, &sweep_objective, run_provenance);
                    increment_run_outcome_counts(
                        sweep_report.candidate_outcomes,
                        report.outcome);
                    increment_run_rankability_counts(
                        sweep_report.candidate_rankability,
                        report.objective.rankability);
                    trace_observation_payloads[idx] = TraceObservationPayload{
                        report.outcome,
                        report.objective.rankability
                    };
                    pareto_collector.observe(
                        idx,
                        evaluate_ranking_objective(sweep_objective, r),
                        r);
                }
                return r;
                };

            auto score_fn = [&](const BacktestResult& r) -> double {
                return evaluate_ranking_objective(sweep_objective, r);
                };

            auto ostream_progress_cb = make_ostream_progress_cb(
                out,
                std::chrono::milliseconds(opt.ostream_progress_throttle_ms));
            auto progress_cb = [&](const SweepProgress& progress) {
                SweepProgress enriched = progress;
                populate_plan_sweep_candidate_parameter_values(dims, enriched.current_best);
                upsert_stage_summary(
                    sweep_report,
                    enriched.stage,
                    enriched.stage_done,
                    enriched.stage_total);
                if (!enriched.current_best.empty()) {
                    sweep_report.top_candidates = enriched.current_best;
                }
                if (ostream_progress_cb) {
                    ostream_progress_cb(enriched);
                }
                if (opt.progress_cb) {
                    opt.progress_cb(enriched);
                }
            };
            std::vector<IndexedCandidate> best;
            std::size_t eval_count = 0;
            const std::size_t retained_top_k =
                plan_sweep_retained_top_k(sw, engine_candidate_space_size);

            switch (sw.engine) {
            case PlanSweepEngineKind::Exhaustive: {
                auto eval_token = [&](const SweepCandidateToken& token) -> BacktestResult {
                    return eval_index(token.id);
                    };

                DenseIndexSweepSpace space(engine_candidate_space_size);
                FunctionTrialEvaluator evaluator(eval_token);
                FunctionObjective objective(score_fn);
                ExhaustiveIndexTopKPolicy policy;
                trace_recorder.set_policy_name(policy.name());
                record_common_trace_config(
                    trace_recorder,
                    engine_candidate_space_size,
                    retained_top_k);
                record_trace_config_u64(trace_recorder, "batch_size", 256);
                SweepSessionConfig session_cfg{};
                session_cfg.top_k = retained_top_k;
                session_cfg.progress_cb = progress_cb;
                session_cfg.cancel_cb = opt.cancel_requested;
                session_cfg.trace_cb = trace_cb;

                auto session = run_sweep_session(
                    space,
                    evaluator,
                    objective,
                    policy,
                    session_cfg);

                best = std::move(session.best_first);
                eval_count = session.eval_count;
                upsert_stage_summary(
                    sweep_report,
                    SweepProgressStage::CandidateEvaluation,
                    session.eval_count,
                    session.total_candidates);
                break;
            }

            case PlanSweepEngineKind::CoarseFine:
            case PlanSweepEngineKind::SuccessiveHalving:
            case PlanSweepEngineKind::MonteCarlo:
            case PlanSweepEngineKind::BayesianPosterior: {
                SweepIndexConfig cfg{};
                SweepIndexProblem problem{};
                problem.eval = [&](std::size_t idx) -> BacktestResult {
                    return eval_index(idx);
                    };

                auto configure_grid_neighbors = [&]() {
                    const std::size_t nd = dims.values.size();
                    std::vector<std::size_t> radices;
                    radices.reserve(nd);
                    for (const auto& v : dims.values) radices.push_back(v.size());

                    auto unpack = [radices, nd](std::size_t idx, std::vector<std::size_t>& coords) {
                        coords.assign(nd, 0);
                        for (std::size_t d = 0; d < nd; ++d) {
                            const auto r = radices[d];
                            if (r == 0) continue;
                            coords[d] = idx % r;
                            idx /= r;
                        }
                        };

                    auto pack = [radices, nd](const std::vector<std::size_t>& coords) -> std::size_t {
                        std::size_t mult = 1;
                        std::size_t out_idx = 0;
                        for (std::size_t d = 0; d < nd; ++d) {
                            out_idx += coords[d] * mult;
                            mult *= radices[d];
                        }
                        return out_idx;
                        };

                    problem.neighbors = [unpack, pack, radices, nd](std::size_t idx) -> std::vector<std::size_t> {
                        std::vector<std::size_t> coords;
                        unpack(idx, coords);

                        std::vector<std::size_t> out;
                        out.reserve(1 + 2 * nd);
                        out.push_back(idx);

                        for (std::size_t d = 0; d < nd; ++d) {
                            if (coords[d] > 0) {
                                auto c = coords;
                                --c[d];
                                out.push_back(pack(c));
                            }
                            if (coords[d] + 1 < radices[d]) {
                                auto c = coords;
                                ++c[d];
                                out.push_back(pack(c));
                            }
                        }
                        return out;
                        };
                    };

                auto configure_grid_features = [&]() {
                    const std::vector<std::vector<double>> values = dims.values;
                    const std::vector<double> mins = [&]() {
                        std::vector<double> out;
                        out.reserve(values.size());
                        for (const auto& dim_values : values) {
                            if (dim_values.empty()) {
                                out.push_back(0.0);
                            }
                            else {
                                out.push_back(*std::min_element(
                                    dim_values.begin(),
                                    dim_values.end()));
                            }
                        }
                        return out;
                    }();
                    const std::vector<double> maxs = [&]() {
                        std::vector<double> out;
                        out.reserve(values.size());
                        for (const auto& dim_values : values) {
                            if (dim_values.empty()) {
                                out.push_back(0.0);
                            }
                            else {
                                out.push_back(*std::max_element(
                                    dim_values.begin(),
                                    dim_values.end()));
                            }
                        }
                        return out;
                    }();
                    const bool normalize =
                        sw.bayesian_posterior.normalize_inputs;
                    problem.features =
                        [values, mins, maxs, normalize](std::size_t idx) {
                        std::vector<double> features;
                        features.reserve(values.size());
                        for (std::size_t dim = 0; dim < values.size(); ++dim) {
                            const auto& dim_values = values[dim];
                            if (dim_values.empty()) {
                                features.push_back(0.0);
                                continue;
                            }
                            const std::size_t value_index =
                                idx % dim_values.size();
                            idx /= dim_values.size();
                            double value = dim_values[value_index];
                            if (normalize) {
                                const double range = maxs[dim] - mins[dim];
                                value = range > 1e-12
                                    ? ((value - mins[dim]) / range)
                                    : 0.0;
                            }
                            features.push_back(value);
                        }
                        return features;
                        };
                    };

                if (sw.engine == PlanSweepEngineKind::CoarseFine) {
                    if (engine_candidate_space_size != total_combos) {
                        out << "PlanSpec sweep error: coarse_fine requires full cartesian grid "
                            << "(set max_evals to total combinations).\n";
                        return false;
                    }
                    configure_grid_neighbors();

                    cfg.kind = SweepIndexKind::CoarseFine;
                    cfg.coarse_fine.k_stage1 = sw.coarse_fine.k_stage1;
                    cfg.coarse_fine.k_stage2 =
                        sw.result_diversity.enabled
                        ? std::max(sw.coarse_fine.k_stage2, retained_top_k)
                        : sw.coarse_fine.k_stage2;
                    cfg.coarse_fine.opt.depollute_stage1 = sw.coarse_fine.depollute_stage1;
                    cfg.coarse_fine.opt.dedupe_stage2 = sw.coarse_fine.dedupe_stage2;
                    cfg.coarse_fine.opt.suppression_hops = sw.coarse_fine.suppression_hops;
                    cfg.coarse_fine.opt.stage2_hops = sw.coarse_fine.stage2_hops;
                    cfg.coarse_fine.opt.max_evaluations = sw.max_evals;

                    bool derive_refinement_from_budget = false;
                    if (sw.coarse_fine.coarse_step_multiplier > 1) {
                        cfg.coarse_fine.stage1_indices = make_plan_sweep_per_dimension_coarse_indices(
                            dims,
                            sw.coarse_fine.coarse_step_multiplier,
                            sw.max_evals);
                        derive_refinement_from_budget = true;
                    }
                    else if (sw.coarse_fine.stage1_stride > 1) {
                        cfg.coarse_fine.stage1_indices = make_plan_sweep_flat_stride_indices(
                            engine_candidate_space_size,
                            sw.coarse_fine.stage1_stride,
                            sw.max_evals);
                    }
                    else if (sw.max_evals > 0 && engine_candidate_space_size > sw.max_evals) {
                        const std::size_t refinement_reserve =
                            estimate_plan_sweep_coarse_fine_refinement_reserve(sw, dims);
                        const std::size_t stage1_budget =
                            sw.max_evals > refinement_reserve
                            ? (sw.max_evals - refinement_reserve)
                            : sw.max_evals;
                        const std::size_t auto_stride =
                            std::max<std::size_t>(
                                2,
                                plan_sweep_ceil_div(engine_candidate_space_size, std::max<std::size_t>(1, stage1_budget)));
                        sw.coarse_fine.stage1_stride = auto_stride;
                        sweep_report.requested_sweep = sw;
                        cfg.coarse_fine.stage1_indices = make_plan_sweep_flat_stride_indices(
                            engine_candidate_space_size,
                            auto_stride,
                            sw.max_evals);
                        derive_refinement_from_budget = true;
                    }

                    if (derive_refinement_from_budget && cfg.coarse_fine.stage1_indices) {
                        const std::size_t derived_stage2_hops =
                            derive_plan_sweep_stage2_hops_for_remaining_budget(
                                sw,
                                dims,
                                cfg.coarse_fine.stage1_indices->size());
                        if (derived_stage2_hops > sw.coarse_fine.stage2_hops) {
                            sw.coarse_fine.stage2_hops = derived_stage2_hops;
                            sweep_report.requested_sweep = sw;
                            cfg.coarse_fine.opt.stage2_hops = derived_stage2_hops;
                        }
                    }
                }
                else if (sw.engine == PlanSweepEngineKind::SuccessiveHalving) {
                    configure_grid_neighbors();
                    cfg.kind = SweepIndexKind::SuccessiveHalving;
                    cfg.successive_halving.rounds = sw.successive_halving.rounds;
                    cfg.successive_halving.reduction_factor =
                        sw.successive_halving.reduction_factor;
                    cfg.successive_halving.initial_candidates =
                        sw.successive_halving.initial_candidates;
                    cfg.successive_halving.initial_stride =
                        sw.successive_halving.initial_stride;
                    cfg.successive_halving.neighborhood_hops =
                        sw.successive_halving.neighborhood_hops;
                    cfg.successive_halving.max_evaluations = sw.max_evals;
                    cfg.successive_halving.dedupe_candidates =
                        sw.successive_halving.dedupe_candidates;
                    cfg.successive_halving.top_k =
                        (sw.successive_halving.top_k > 0)
                        ? sw.successive_halving.top_k
                        : sw.top_k;
                    if (sw.result_diversity.enabled) {
                        cfg.successive_halving.top_k =
                            std::max(cfg.successive_halving.top_k, retained_top_k);
                    }
                }
                else if (sw.engine == PlanSweepEngineKind::BayesianPosterior) {
                    configure_grid_neighbors();
                    configure_grid_features();
                    cfg.kind = SweepIndexKind::BayesianPosterior;
                    cfg.bayesian_posterior.posterior = sw.bayesian_posterior;
                    cfg.bayesian_posterior.max_evaluations = sw.max_evals;
                    cfg.bayesian_posterior.posterior.top_k =
                        sw.bayesian_posterior.top_k > 0
                        ? sw.bayesian_posterior.top_k
                        : sw.top_k;
                    if (sw.result_diversity.enabled) {
                        cfg.bayesian_posterior.posterior.top_k =
                            std::max(
                                cfg.bayesian_posterior.posterior.top_k,
                                retained_top_k);
                    }
                }
                else {
                    cfg.kind = SweepIndexKind::MonteCarlo;
                    cfg.monte_carlo.draws = std::min(sw.monte_carlo.draws, sw.max_evals);
                    cfg.monte_carlo.seed = sw.monte_carlo.seed;
                    cfg.monte_carlo.dedupe = sw.monte_carlo.dedupe;
                    cfg.monte_carlo.top_k =
                        (sw.monte_carlo.top_k > 0) ? sw.monte_carlo.top_k : sw.top_k;
                    if (sw.result_diversity.enabled) {
                        cfg.monte_carlo.top_k = std::max(cfg.monte_carlo.top_k, retained_top_k);
                    }
                }

                problem.param_count = engine_candidate_space_size;
                auto sweep = make_sweep_index(cfg);
                trace_recorder.set_policy_name(sweep ? sweep->name() : "");
                record_common_trace_config(
                    trace_recorder,
                    engine_candidate_space_size,
                    retained_top_k);
                switch (cfg.kind) {
                case SweepIndexKind::CoarseFine:
                    record_trace_config_u64(
                        trace_recorder,
                        "k_stage1",
                        cfg.coarse_fine.k_stage1);
                    record_trace_config_u64(
                        trace_recorder,
                        "k_stage2",
                        cfg.coarse_fine.k_stage2);
                    record_trace_config_u64(
                        trace_recorder,
                        "stage1_candidate_count",
                        cfg.coarse_fine.stage1_indices
                        ? cfg.coarse_fine.stage1_indices->size()
                        : problem.param_count);
                    record_trace_config_string(
                        trace_recorder,
                        "stage1_source",
                        cfg.coarse_fine.stage1_indices
                        ? "coarse_subset"
                        : "full_grid");
                    record_trace_config_bool(
                        trace_recorder,
                        "depollute_stage1",
                        cfg.coarse_fine.opt.depollute_stage1);
                    record_trace_config_bool(
                        trace_recorder,
                        "dedupe_stage2",
                        cfg.coarse_fine.opt.dedupe_stage2);
                    record_trace_config_u64(
                        trace_recorder,
                        "suppression_hops",
                        cfg.coarse_fine.opt.suppression_hops);
                    record_trace_config_u64(
                        trace_recorder,
                        "stage2_hops",
                        cfg.coarse_fine.opt.stage2_hops);
                    record_trace_config_u64(
                        trace_recorder,
                        "max_evaluations",
                        cfg.coarse_fine.opt.max_evaluations);
                    break;
                case SweepIndexKind::MonteCarlo:
                    record_trace_config_u64(
                        trace_recorder,
                        "draws",
                        cfg.monte_carlo.draws);
                    record_trace_config_u64(
                        trace_recorder,
                        "top_k",
                        cfg.monte_carlo.top_k);
                    record_trace_config_bool(
                        trace_recorder,
                        "dedupe",
                        cfg.monte_carlo.dedupe);
                    record_trace_config_u64(
                        trace_recorder,
                        "requested_seed",
                        cfg.monte_carlo.seed);
                    break;
                case SweepIndexKind::SuccessiveHalving:
                    record_trace_config_u64(
                        trace_recorder,
                        "rounds",
                        cfg.successive_halving.rounds);
                    record_trace_config_u64(
                        trace_recorder,
                        "reduction_factor",
                        cfg.successive_halving.reduction_factor);
                    record_trace_config_u64(
                        trace_recorder,
                        "initial_candidates",
                        cfg.successive_halving.initial_candidates);
                    record_trace_config_u64(
                        trace_recorder,
                        "initial_stride",
                        cfg.successive_halving.initial_stride);
                    record_trace_config_u64(
                        trace_recorder,
                        "neighborhood_hops",
                        cfg.successive_halving.neighborhood_hops);
                    record_trace_config_u64(
                        trace_recorder,
                        "max_evaluations",
                        cfg.successive_halving.max_evaluations);
                    record_trace_config_u64(
                        trace_recorder,
                        "top_k",
                        cfg.successive_halving.top_k);
                    record_trace_config_bool(
                        trace_recorder,
                        "dedupe_candidates",
                        cfg.successive_halving.dedupe_candidates);
                    break;
                case SweepIndexKind::BayesianPosterior:
                    record_posterior_trace_config(
                        trace_recorder,
                        cfg.bayesian_posterior.posterior);
                    record_trace_config_u64(
                        trace_recorder,
                        "max_evaluations",
                        cfg.bayesian_posterior.max_evaluations);
                    break;
                }
                const auto run = sweep->run(
                    problem,
                    score_fn,
                    progress_cb,
                    opt.cancel_requested,
                    trace_cb);
                if (run.effective_seed_available) {
                    trace_recorder.record_seed(
                        sweep ? sweep->name() : "",
                        run.requested_seed_explicit,
                        run.requested_seed,
                        run.effective_seed,
                        true,
                        "candidate_proposal");
                }
                if (run.has_posterior) {
                    sweep_report.posterior = run.posterior;
                }
                best = run.has_two_stage ? run.two_stage.stage2_best_first : run.best_first;
                eval_count = run.eval_count;
                if (run.has_two_stage) {
                    upsert_stage_summary(
                        sweep_report,
                        SweepProgressStage::CoarseSearch,
                        run.two_stage.stage1_evaluated,
                        run.two_stage.stage1_evaluated);
                    upsert_stage_summary(
                        sweep_report,
                        SweepProgressStage::LocalRefinement,
                        run.two_stage.stage2_evaluated,
                        run.two_stage.stage2_evaluated);
                }
                else if (sw.engine == PlanSweepEngineKind::MonteCarlo) {
                    const std::size_t monte_carlo_stage_total =
                        cfg.monte_carlo.dedupe
                        ? std::min(cfg.monte_carlo.draws, problem.param_count)
                        : cfg.monte_carlo.draws;
                    upsert_stage_summary(
                        sweep_report,
                        SweepProgressStage::MonteCarloSampling,
                        run.eval_count,
                        monte_carlo_stage_total);
                }
                else if (sw.engine == PlanSweepEngineKind::SuccessiveHalving) {
                    const std::size_t adaptive_stage_total =
                        cfg.successive_halving.dedupe_candidates
                        ? std::min(cfg.successive_halving.max_evaluations, problem.param_count)
                        : cfg.successive_halving.max_evaluations;
                    const bool adaptive_canceled =
                        opt.cancel_requested && opt.cancel_requested();
                    upsert_stage_summary(
                        sweep_report,
                        SweepProgressStage::AdaptiveSelection,
                        run.eval_count,
                        adaptive_canceled ? adaptive_stage_total : run.eval_count);
                }
                else if (sw.engine == PlanSweepEngineKind::BayesianPosterior) {
                    const std::size_t posterior_stage_total =
                        run.has_posterior
                        ? (run.posterior.initial_design_size +
                            run.posterior.iterations)
                        : run.eval_count;
                    const bool posterior_canceled =
                        opt.cancel_requested && opt.cancel_requested();
                    upsert_stage_summary(
                        sweep_report,
                        SweepProgressStage::AdaptiveSelection,
                        run.eval_count,
                        posterior_canceled ? posterior_stage_total : run.eval_count);
                }
                break;
            }
            }

            sweep_report.compile_failures = compile_failures;
            sweep_report.execution_failures = execution_failures;
            sweep_report.last_execution_failure = last_execution_failure;
            sweep_report.last_compile_error = last_compile_error;
            sweep_report.evaluations_run = eval_count;
            sweep_report.proposal_trace = trace_recorder.summary();

            if (opt.cancel_requested && opt.cancel_requested()) {
                sweep_report.canceled = true;
                sweep_report.pareto = pareto_collector.build_summary(dims, 0);
                if (sweep_report_out) {
                    *sweep_report_out = sweep_report;
                }
                out << "PlanSpec execution canceled.\n";
                return false;
            }

            std::sort(best.begin(), best.end(),
                [&](const IndexedCandidate& a, const IndexedCandidate& b) {
                    if (a.score != b.score) return a.score > b.score;
                    if (ranking_objective_prefers_left(sweep_objective, a.result, b.result)) return true;
                    if (ranking_objective_prefers_left(sweep_objective, b.result, a.result)) return false;
                    return a.index < b.index;
                });

            const std::vector<IndexedCandidate> retained_candidate_pool = best;
            sweep_report.pareto = pareto_collector.build_summary(
                dims,
                retained_candidate_pool.size());
            best = apply_plan_sweep_result_diversity(
                best,
                dims,
                sw,
                sweep_report.result_diversity);
            sweep_report.search_geometry = build_plan_sweep_search_geometry(
                retained_candidate_pool,
                best,
                dims,
                sw);

            const auto after_engine = std::chrono::steady_clock::now();
            const double ranking_elapsed =
                std::chrono::duration<double>(after_engine - sweep_started).count();
            const auto ranked_candidates = make_plan_sweep_ranked_candidates(best, dims);
            progress_cb(make_sweep_progress(
                eval_count,
                engine_candidate_space_size,
                ranking_elapsed,
                0.0,
                SweepProgressStage::Ranking,
                1,
                1,
                ranked_candidates));

            sweep_report.top_candidates = ranked_candidates;
            sweep_report.candidate_reports = build_sweep_candidate_reports(
                sweep_report.top_candidates,
                &sweep_report.resolved_objective,
                &sweep_report.run_provenance);
            sweep_report.warnings = derive_plan_sweep_report_warnings(sweep_report);
            progress_cb(make_sweep_progress(
                eval_count,
                engine_candidate_space_size,
                ranking_elapsed,
                0.0,
                SweepProgressStage::ReportAssembly,
                1,
                1,
                sweep_report.top_candidates));

            if (!sweep_report.top_candidates.empty() && result_out) {
                *result_out = sweep_report.top_candidates.front().result;
            }
            if (sweep_report_out) {
                *sweep_report_out = sweep_report;
            }
            print_plan_sweep_report(sweep_report, out);
            return true;
        }

        BacktestResult r;
        std::string e;
        if (!run_one(nullptr, r, e)) {
            if (opt.cancel_requested && opt.cancel_requested()) {
                out << "PlanSpec execution canceled.\n";
            }
            else {
                out << "PlanSpec compile failed: " << e << "\n";
            }
            return false;
        }
        if (result_out) {
            *result_out = r;
        }
        print_run_report_summary(
            build_run_report(r, nullptr, run_provenance),
            out);
        return true;
    }

} // namespace sugar
