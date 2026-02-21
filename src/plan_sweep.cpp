#include "plan_sweep.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "metrics.h"
#include "op_registry.h"
#include "plan_compiler.h"
#include "strategy_executor.h"
#include "sweep.h"

namespace sugar {

    namespace {

        const char* engine_name(PlanSweepEngineKind k) {
            switch (k) {
            case PlanSweepEngineKind::CoarseFine: return "coarse_fine";
            case PlanSweepEngineKind::MonteCarlo: return "monte_carlo";
            case PlanSweepEngineKind::Exhaustive:
            default: return "exhaustive";
            }
        }

        double compute_score(const BacktestResult& r, const PlanSweepSpec& sw) {
            if (sw.objective == "pnl") return r.pnl;
            // default: pnl_dd
            return r.pnl - sw.dd_weight * r.max_drawdown;
        }

    } // namespace

    bool run_custom_plan_spec(
        const CandleSeries& series,
        const PlanSpec& spec,
        const PlanSweepRuntimeOptions& opt,
        std::ostream& out)
    {
        const auto reg = make_default_registry();
        CompileEnv env;

        auto run_one = [&](PlanSpec& s, BacktestResult& result_out, std::string& err_out) -> bool {
            CompileResult cr;
            if (!compile_plan(s, reg, env, cr, err_out)) return false;

            if (opt.override_mode) {
                cr.plan.mode = opt.custom_mode;
            }

            StrategyExecutor exec;
            ExecSettings settings = cr.exec;
            if (opt.override_eval_mode) {
                settings.eval_mode = opt.eval_mode;
            }
            result_out = exec.run(series, cr.plan, settings);
            return true;
            };

        if (spec.sweep && (spec.sweep->enabled || !spec.sweep->params.empty())) {
            const PlanSweepSpec& sw = *spec.sweep;

            PlanSpecSweepDims dims;
            std::string sweep_err;
            if (!build_planspec_sweep_dims(sw, dims, sweep_err)) {
                out << "PlanSpec sweep error: " << sweep_err << "\n";
                return false;
            }

            const std::size_t total_combos = dims.total_combinations();
            auto points = build_planspec_sweep_points(dims, sw.max_evals);

            std::size_t compile_failures = 0;
            std::string last_compile_error;

            using IndexVec = std::vector<std::size_t>;

            auto eval_index = [&](const CandleSeries& /*data*/, std::size_t idx) -> BacktestResult {
                if (idx >= points.size()) {
                    BacktestResult bad{};
                    bad.pnl = -std::numeric_limits<double>::infinity();
                    bad.max_drawdown = std::numeric_limits<double>::infinity();
                    return bad;
                }

                const IndexVec& p = points[idx];
                PlanSpec trial = spec;

                for (std::size_t i = 0; i < dims.names.size(); ++i) {
                    const std::string& name = dims.names[i];
                    const double v = dims.values[i][p[i]];
                    if (dims.is_int[i]) trial.params[name] = static_cast<std::int64_t>(std::llround(v));
                    else trial.params[name] = v;
                }

                BacktestResult r;
                std::string err_run;
                if (!run_one(trial, r, err_run)) {
                    ++compile_failures;
                    last_compile_error = err_run;
                    r.pnl = -std::numeric_limits<double>::infinity();
                    r.max_drawdown = std::numeric_limits<double>::infinity();
                    r.trades = 0;
                    r.trade_hash = 0;
                }
                return r;
                };

            auto score_fn = [&](const BacktestResult& r) -> double {
                return compute_score(r, sw);
                };

            auto progress_cb = make_ostream_progress_cb(out);
            std::vector<IndexedCandidate> best;
            std::size_t eval_count = 0;

            switch (sw.engine) {
            case PlanSweepEngineKind::Exhaustive: {
                auto eval_token = [&](const CandleSeries& data, const SweepCandidateToken& token) -> BacktestResult {
                    return eval_index(data, token.id);
                    };

                DenseIndexSweepSpace space(points.size());
                FunctionTrialEvaluator evaluator(eval_token);
                FunctionObjective objective(score_fn);
                ExhaustiveIndexTopKPolicy policy;
                SweepSessionConfig session_cfg{};
                session_cfg.top_k = sw.top_k;
                session_cfg.progress_cb = progress_cb;

                auto session = run_sweep_session(
                    series,
                    space,
                    evaluator,
                    objective,
                    policy,
                    session_cfg);

                best = std::move(session.best_first);
                eval_count = session.eval_count;
                break;
            }

            case PlanSweepEngineKind::CoarseFine:
            case PlanSweepEngineKind::MonteCarlo: {
                SweepIndexConfig cfg{};
                SweepIndexProblem problem{};
                problem.param_count = points.size();
                problem.eval = [&](const CandleSeries& data, std::size_t idx) -> BacktestResult {
                    return eval_index(data, idx);
                    };

                if (sw.engine == PlanSweepEngineKind::CoarseFine) {
                    if (total_combos == std::numeric_limits<std::size_t>::max() || points.size() != total_combos) {
                        out << "PlanSpec sweep error: coarse_fine requires full cartesian grid "
                            << "(set max_evals to total combinations).\n";
                        return false;
                    }

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

                    cfg.kind = SweepIndexKind::CoarseFine;
                    cfg.coarse_fine.k_stage1 = sw.coarse_fine.k_stage1;
                    cfg.coarse_fine.k_stage2 = sw.coarse_fine.k_stage2;
                    cfg.coarse_fine.opt.depollute_stage1 = sw.coarse_fine.depollute_stage1;
                    cfg.coarse_fine.opt.dedupe_stage2 = sw.coarse_fine.dedupe_stage2;
                    cfg.coarse_fine.opt.suppression_hops = sw.coarse_fine.suppression_hops;
                    cfg.coarse_fine.opt.stage2_hops = sw.coarse_fine.stage2_hops;

                    if (sw.coarse_fine.stage1_stride > 1) {
                        auto stage1_indices = std::make_shared<std::vector<std::size_t>>();
                        stage1_indices->reserve(points.size() / sw.coarse_fine.stage1_stride + 1);
                        for (std::size_t i = 0; i < points.size(); i += sw.coarse_fine.stage1_stride) {
                            stage1_indices->push_back(i);
                        }
                        cfg.coarse_fine.stage1_indices = stage1_indices;
                    }
                }
                else {
                    cfg.kind = SweepIndexKind::MonteCarlo;
                    cfg.monte_carlo.draws = sw.monte_carlo.draws;
                    cfg.monte_carlo.seed = sw.monte_carlo.seed;
                    cfg.monte_carlo.dedupe = sw.monte_carlo.dedupe;
                    cfg.monte_carlo.top_k = (sw.monte_carlo.top_k > 0) ? sw.monte_carlo.top_k : sw.top_k;
                }

                auto sweep = make_sweep_index(cfg);
                const auto run = sweep->run(series, problem, score_fn, progress_cb);
                best = run.has_two_stage ? run.two_stage.stage2_best_first : run.best_first;
                eval_count = run.eval_count;
                break;
            }
            }

            out << "\n";

            out << "\n=== PlanSpec Sweep Results ===\n";
            out << "Engine: " << engine_name(sw.engine) << "\n";
            out << "Objective: " << sw.objective;
            if (sw.objective != "pnl") out << " (dd_weight=" << sw.dd_weight << ")";
            out << "\n";
            out << "Param dims: " << dims.names.size() << "\n";
            out << "Total combos: ";
            if (total_combos == std::numeric_limits<std::size_t>::max()) out << "(overflow)\n";
            else out << total_combos << "\n";
            out << "Evals run: " << eval_count << " (max " << sw.max_evals << ")\n";
            out << "Compile failures: " << compile_failures << "\n";
            if (!last_compile_error.empty()) {
                out << "Last compile error: " << last_compile_error << "\n";
            }
            out << "TopK: " << best.size() << "\n\n";

            for (std::size_t i = 0; i < best.size(); ++i) {
                out << "-- Rank " << (i + 1) << " | score=" << best[i].score << " --\n";
                out << "Params:\n";

                if (best[i].index >= points.size()) continue;
                const auto& p = points[best[i].index];

                for (std::size_t d = 0; d < dims.names.size(); ++d) {
                    const auto& name = dims.names[d];
                    const double v = dims.values[d][p[d]];
                    out << "  " << name << ": ";
                    if (dims.is_int[d]) out << static_cast<std::int64_t>(std::llround(v));
                    else out << v;
                    out << "\n";
                }

                out << "Result:\n";
                print_backtest_summary(best[i].result, out, "  ");
                out << "\n";
            }

            if (!best.empty()) {
                out << "=== Best Result (Rank 1) ===\n";
                print_backtest_summary(best.front().result, out);
            }
            return true;
        }

        BacktestResult r;
        PlanSpec trial = spec;
        std::string e;
        if (!run_one(trial, r, e)) {
            out << "PlanSpec compile failed: " << e << "\n";
            return false;
        }
        print_backtest_summary(r, out);
        return true;
    }

} // namespace sugar
