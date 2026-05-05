#include "sweep/sweep.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <numeric>
#include <limits>
#include <queue>
#include <unordered_set>

#include "ingest/utils.h"

namespace sugar {

    using Clock = std::chrono::steady_clock;

    std::string_view sweep_progress_stage_name(SweepProgressStage stage)
    {
        switch (stage) {
        case SweepProgressStage::CandidateEvaluation: return "candidate evaluation";
        case SweepProgressStage::CoarseSearch: return "coarse search";
        case SweepProgressStage::LocalRefinement: return "local refinement";
        case SweepProgressStage::MonteCarloSampling: return "monte carlo sampling";
        case SweepProgressStage::Ranking: return "ranking";
        case SweepProgressStage::ReportAssembly: return "report assembly";
        }
        return "unknown";
    }

    namespace {
        std::vector<SweepRankedCandidate> make_ranked_progress_candidates(
            const std::vector<IndexedCandidate>& best_first)
        {
            std::vector<SweepRankedCandidate> out;
            out.reserve(best_first.size());
            for (const auto& candidate : best_first) {
                SweepRankedCandidate ranked{};
                ranked.candidate_index = candidate.index;
                ranked.score = candidate.score;
                ranked.result = candidate.result;
                out.push_back(std::move(ranked));
            }
            return out;
        }

        std::vector<SweepRankedCandidate> make_ranked_progress_candidates(
            const TopK<std::size_t>& top)
        {
            std::vector<SweepRankedCandidate> out;
            const auto best_first = top.sorted_best_first();
            out.reserve(best_first.size());
            for (const auto& candidate : best_first) {
                SweepRankedCandidate ranked{};
                ranked.candidate_index = candidate.params;
                ranked.score = candidate.score;
                ranked.result = candidate.result;
                out.push_back(std::move(ranked));
            }
            return out;
        }
    } // namespace

    // ============================================================================
    // Reporting helpers
    // ============================================================================
    SweepProgressCallback make_ostream_progress_cb(
        std::ostream& out,
        std::chrono::milliseconds throttle,
        const char* prefix)
    {
        // Shared state so the returned lambda is copyable.
        struct State {
            Clock::time_point last = Clock::now();
        };
        auto st = std::make_shared<State>();

        return [&out, throttle, prefix, st](const SweepProgress& p) {
            const auto now = Clock::now();
            if (throttle.count() > 0 && (now - st->last) < throttle) return;
            st->last = now;

            out << "\r" << prefix
                << sweep_progress_stage_name(p.stage) << " "
                << p.done << "/" << p.total
                << " stage=" << p.stage_done << "/" << p.stage_total
                << " elapsed=" << static_cast<int>(p.elapsed_sec) << "s"
                << " eta~" << static_cast<int>(p.eta_sec) << "s";
            if (!p.current_best.empty()) {
                out << " best=" << p.current_best.front().score;
            }
            out
                << std::flush;
            };
    }

    // ============================================================================
    // PlanSpec sweep adapters
    // ============================================================================
    bool build_planspec_sweep_dims(
        const PlanSweepSpec& sw,
        PlanSpecSweepDims& out,
        std::string& err)
    {
        out = PlanSpecSweepDims{};
        err.clear();

        if (sw.params.empty()) {
            err = "PlanSpec sweep has no params.";
            return false;
        }

        struct DimTmp {
            std::string name;
            std::vector<double> values;
            bool is_int{ true };
        };
        std::vector<DimTmp> dims;
        dims.reserve(sw.params.size());

        for (const auto& kv : sw.params) {
            DimTmp d;
            d.name = kv.first;
            d.is_int = kv.second.is_int;

            if (!kv.second.values.empty()) {
                d.values = kv.second.values;
            } else {
                const double a = kv.second.min;
                const double b = kv.second.max;
                const double step = kv.second.step;

                if (!(std::isfinite(a) && std::isfinite(b) && std::isfinite(step))) {
                    err = "PlanSpec sweep param '" + d.name + "' has non-finite min/max/step.";
                    return false;
                }
                if (step <= 0.0) {
                    err = "PlanSpec sweep param '" + d.name + "' has non-positive step.";
                    return false;
                }
                if (b < a) {
                    err = "PlanSpec sweep param '" + d.name + "' has max < min.";
                    return false;
                }

                // Expand the declared range exactly once here. Evaluation-budget
                // limits belong in the engine layer, not in the author-facing
                // range definition.
                d.values = make_drange(a, b, step);
            }

            if (d.values.empty()) {
                err = "PlanSpec sweep param '" + d.name + "' has no values.";
                return false;
            }
            dims.push_back(std::move(d));
        }

        // Deterministic order.
        std::sort(dims.begin(), dims.end(), [](const DimTmp& a, const DimTmp& b) {
            return a.name < b.name;
            });

        out.names.reserve(dims.size());
        out.values.reserve(dims.size());
        out.is_int.reserve(dims.size());

        for (auto& d : dims) {
            out.names.push_back(std::move(d.name));
            out.values.push_back(std::move(d.values));
            out.is_int.push_back(d.is_int);
        }

        return true;
    }

    std::vector<std::vector<std::size_t>> build_planspec_sweep_points(
        const PlanSpecSweepDims& dims,
        std::size_t max_evals)
    {
        std::vector<std::vector<std::size_t>> points;
        if (dims.values.empty() || max_evals == 0) return points;

        const std::size_t nd = dims.values.size();
        for (const auto& v : dims.values) {
            if (v.empty()) return points;
        }

        const std::size_t total = dims.total_combinations();
        const std::size_t want = std::min<std::size_t>(max_evals, total);

        points.reserve(want);

        std::vector<std::size_t> idx(nd, 0);
        for (std::size_t i = 0; i < want; ++i) {
            points.push_back(idx);

            // increment mixed radix counter
            std::size_t k = 0;
            for (; k < nd; ++k) {
                idx[k]++;
                if (idx[k] < dims.values[k].size()) break;
                idx[k] = 0;
            }
            if (k == nd) break; // exhausted early (total < want due to overflow clamp)
        }

        return points;
    }

    namespace {

        // Mark idx and (optionally) its neighborhood as suppressed.
        // suppression_hops=1 suppresses idx and its direct neighbors.
        void suppress_region(
            std::vector<unsigned char>& suppressed,
            std::size_t start_idx,
            std::size_t suppression_hops,
            const std::function<std::vector<std::size_t>(std::size_t)>& neighbors)
        {
            if (start_idx >= suppressed.size()) return;

            std::queue<std::pair<std::size_t, std::size_t>> q;
            q.push({ start_idx, 0 });

            while (!q.empty()) {
                auto [idx, depth] = q.front();
                q.pop();

                if (idx >= suppressed.size()) continue;
                if (suppressed[idx]) continue;

                suppressed[idx] = 1;
                if (depth >= suppression_hops) continue;

                for (auto nb : neighbors(idx)) {
                    if (nb < suppressed.size() && !suppressed[nb]) {
                        q.push({ nb, depth + 1 });
                    }
                }
            }
        }

        // Collect indices within `hops` neighbor-expansions from start_idx (inclusive).
        // `seen` dedupes evaluations across seeds, while local `visited` keeps
        // traversal correct when seed neighborhoods overlap.
        template <typename SeenT>
        void collect_region(
            std::size_t param_count,
            std::size_t start_idx,
            std::size_t hops,
            const std::function<std::vector<std::size_t>(std::size_t)>& neighbors,
            SeenT* seen,
            const std::function<void(std::size_t)>& on_index,
            const std::function<bool()>& stop_requested = {})
        {
            if (start_idx >= param_count) return;

            std::queue<std::pair<std::size_t, std::size_t>> q;
            std::unordered_set<std::size_t> visited;
            q.push({ start_idx, 0 });

            while (!q.empty()) {
                auto [idx, depth] = q.front();
                q.pop();

                if (stop_requested && stop_requested()) return;
                if (idx >= param_count) continue;
                if (!visited.insert(idx).second) continue;

                bool should_evaluate = true;
                if (seen) {
                    should_evaluate = seen->insert(idx).second;
                }
                if (should_evaluate) {
                    on_index(idx);
                }

                if (depth >= hops) continue;
                for (auto nb : neighbors(idx)) {
                    if (nb < param_count) {
                        q.push({ nb, depth + 1 });
                    }
                }
            }
        }

    } // namespace

    // ============================================================================
    // Index-based coarse→fine sweep (existing algorithm)
    // ============================================================================
    TwoStageSweepResult two_stage_sweep_topk(
        std::size_t param_count,
        const std::vector<std::size_t>* stage1_indices,
        const std::function<BacktestResult(std::size_t)>& eval,
        const std::function<double(const BacktestResult&)>& score,
        const std::function<std::vector<std::size_t>(std::size_t)>& neighbors,
        std::size_t k_stage1,
        std::size_t k_stage2,
        TwoStageSweepOptions opt,
        SweepProgressCallback progress_cb,
        SweepCancellationCallback cancel_cb)
    {
        const auto t0 = Clock::now();
        auto last_report = t0;

        // Stage 1 total is exact; stage 2 is estimated (neighbor BFS).
        const std::size_t stage1_total = stage1_indices ? stage1_indices->size() : param_count;
        const std::size_t stage2_est = k_stage1 * 250; // ~243 for radius=1 hypercube-ish
        const std::size_t uncapped_total_est = stage1_total + stage2_est;
        const std::size_t total_est =
            opt.max_evaluations > 0
            ? opt.max_evaluations
            : uncapped_total_est;

        std::size_t done = 0;
        std::size_t stage1_done = 0;
        auto budget_reached = [&]() {
            return opt.max_evaluations > 0 && done >= opt.max_evaluations;
            };

        auto report = [&](SweepProgressStage stage,
                          std::size_t stage_done,
                          std::size_t stage_total,
                          std::vector<SweepRankedCandidate> current_best,
                          bool force = false) {
            if (!progress_cb) return;
            const auto now = Clock::now();
            if (!force && (now - last_report) < std::chrono::milliseconds(250)) return;
            last_report = now;

            const double elapsed = std::chrono::duration<double>(now - t0).count();
            const double rate = (elapsed > 1e-6) ? (static_cast<double>(done) / elapsed) : 0.0;
            const double remaining = (total_est > done) ? static_cast<double>(total_est - done) : 0.0;
            const double eta = (rate > 1e-9) ? (remaining / rate) : 0.0;
            progress_cb(make_sweep_progress(
                done,
                total_est,
                elapsed,
                eta,
                stage,
                stage_done,
                stage_total,
                std::move(current_best)));
            };

        // -------------------------
        // Stage 1
        // -------------------------
        std::vector<std::size_t> all;
        if (!stage1_indices) {
            all.reserve(param_count);
            for (std::size_t i = 0; i < param_count; ++i) all.push_back(i);
        }
        const std::vector<std::size_t>& s1 = stage1_indices ? *stage1_indices : all;

        std::vector<IndexedCandidate> stage1;
        stage1.reserve(k_stage1);
        TopK<std::size_t> progress_top1(k_stage1);

        if (!opt.depollute_stage1) {
            TopK<std::size_t> top1(k_stage1);
            for (std::size_t i : s1) {
                if ((cancel_cb && cancel_cb()) || budget_reached()) {
                    break;
                }
                if (i >= param_count) continue;
                const auto res = eval(i);
                ++done;
                ++stage1_done;
                const double sc = score(res);
                if (std::isfinite(sc)) {
                    progress_top1.consider(sc, res, i);
                    top1.consider(sc, res, i);
                }
                report(
                    SweepProgressStage::CoarseSearch,
                    stage1_done,
                    stage1_total,
                    make_ranked_progress_candidates(progress_top1));
            }
            for (const auto& c : top1.sorted_best_first()) {
                stage1.push_back(IndexedCandidate{ c.params, c.score, c.result });
            }
        }
        else {
            // Diversity selection: evaluate stage-1 indices, sort by score,
            // pick top seeds while suppressing local basins.
            std::vector<double> scores(param_count, -std::numeric_limits<double>::infinity());
            std::vector<BacktestResult> results(param_count);
            std::vector<std::size_t> order;
            order.reserve(s1.size());

            for (std::size_t i : s1) {
                if ((cancel_cb && cancel_cb()) || budget_reached()) {
                    break;
                }
                if (i >= param_count) continue;
                const auto res = eval(i);
                ++done;
                ++stage1_done;
                double sc = score(res);
                if (!std::isfinite(sc)) sc = -std::numeric_limits<double>::infinity();
                results[i] = res;
                scores[i] = sc;
                order.push_back(i);
                if (std::isfinite(sc)) {
                    progress_top1.consider(sc, res, i);
                }
                report(
                    SweepProgressStage::CoarseSearch,
                    stage1_done,
                    stage1_total,
                    make_ranked_progress_candidates(progress_top1));
            }

            std::sort(order.begin(), order.end(),
                [&](std::size_t a, std::size_t b) { return scores[a] > scores[b]; });

            std::vector<unsigned char> suppressed(param_count, 0);
            for (auto idx : order) {
                if (stage1.size() >= k_stage1) break;
                if (suppressed[idx]) continue;
                const double sc = scores[idx];
                if (!std::isfinite(sc)) continue;

                stage1.push_back(IndexedCandidate{ idx, sc, results[idx] });
                suppress_region(suppressed, idx, opt.suppression_hops, neighbors);
            }
        }

        report(
            SweepProgressStage::CoarseSearch,
            stage1_done,
            stage1_total,
            make_ranked_progress_candidates(stage1),
            true);

        // -------------------------
        // Stage 2
        // -------------------------
        TopK<std::size_t> top2(k_stage2);
        const std::size_t stage2_total =
            (opt.max_evaluations > stage1_done)
            ? (opt.max_evaluations - stage1_done)
            : stage2_est;
        std::unordered_set<std::size_t> seen;
        if (opt.dedupe_stage2) {
            seen.reserve(stage1.size() * 64);
        }

        const auto stop_refinement = [&]() {
            return (cancel_cb && cancel_cb()) || budget_reached();
            };
        auto evaluate_refinement = [&](std::size_t idx) {
            if (stop_refinement()) {
                return;
            }
            const auto res = eval(idx);
            ++done;
            const double sc = score(res);
            if (std::isfinite(sc)) {
                top2.consider(sc, res, idx);
            }
            report(
                SweepProgressStage::LocalRefinement,
                (done > stage1_done) ? (done - stage1_done) : 0,
                stage2_total,
                make_ranked_progress_candidates(top2));
            };

        if (opt.dedupe_stage2) {
            std::size_t hops = std::max<std::size_t>(1, opt.stage2_hops);
            const std::size_t max_budget_fill_hops =
                (opt.max_evaluations > stage1_done)
                ? std::min<std::size_t>(
                    std::max<std::size_t>(hops, opt.max_evaluations - stage1_done),
                    static_cast<std::size_t>(65536))
                : hops;
            while (!stop_refinement()) {
                const std::size_t before = done;
                for (const auto& seed : stage1) {
                    if (stop_refinement()) {
                        break;
                    }
                    collect_region(
                        param_count,
                        seed.index,
                        hops,
                        neighbors,
                        &seen,
                        evaluate_refinement,
                        stop_refinement);
                }
                if (stop_refinement()) {
                    break;
                }
                if (opt.max_evaluations == 0 || done == before || hops >= max_budget_fill_hops) {
                    break;
                }
                ++hops;
            }
        }
        else {
            for (const auto& seed : stage1) {
                if (stop_refinement()) {
                    break;
                }
                collect_region<std::unordered_set<std::size_t>>(
                    param_count, seed.index, opt.stage2_hops, neighbors,
                    (std::unordered_set<std::size_t>*)nullptr,
                    evaluate_refinement,
                    stop_refinement);
            }
        }

        std::vector<IndexedCandidate> stage2;
        for (const auto& c : top2.sorted_best_first()) {
            stage2.push_back(IndexedCandidate{ c.params, c.score, c.result });
        }
        if (stage2.empty()) {
            stage2 = stage1;
        }

        report(
            SweepProgressStage::LocalRefinement,
            (done > stage1_done) ? (done - stage1_done) : 0,
            stage2_total,
            make_ranked_progress_candidates(top2),
            true);

        TwoStageSweepResult out;
        out.stage1_best_first = std::move(stage1);
        out.stage2_best_first = std::move(stage2);
        out.stage1_evaluated = stage1_done;
        out.stage2_evaluated = (done > stage1_done) ? (done - stage1_done) : 0;
        return out;
    }

    // ============================================================================
    // Train / validate evaluation
    // ============================================================================
    std::vector<TrainValidateCandidate> evaluate_train_validate(
        const std::vector<std::size_t>& indices,
        const std::function<BacktestResult(std::size_t)>& eval_train,
        const std::function<BacktestResult(std::size_t)>& eval_validate,
        const std::function<double(const BacktestResult&)>& train_score_fn,
        const std::function<double(const BacktestResult&)>& validate_score_fn,
        double validate_weight,
        SweepProgressCallback progress_cb,
        SweepCancellationCallback cancel_cb)
    {
        const auto t0 = Clock::now();
        const std::size_t total = indices.size() * 2; // train + validate
        std::size_t done = 0;

        auto report = [&](bool force = false) {
            if (!progress_cb) return;
            if (!force && (done % 25 != 0)) return;
            const auto now = Clock::now();
            const double elapsed = std::chrono::duration<double>(now - t0).count();
            const double rate = (elapsed > 1e-6) ? (static_cast<double>(done) / elapsed) : 0.0;
            const double remaining = (total > done) ? static_cast<double>(total - done) : 0.0;
            const double eta = (rate > 1e-9) ? (remaining / rate) : 0.0;
            progress_cb(make_sweep_progress(
                done,
                total,
                elapsed,
                eta,
                SweepProgressStage::CandidateEvaluation,
                done,
                total));
            };

        std::vector<TrainValidateCandidate> out;
        out.reserve(indices.size());

        for (auto idx : indices) {
            if (cancel_cb && cancel_cb()) {
                break;
            }
            auto tr = eval_train(idx);
            ++done;
            report();

            if (cancel_cb && cancel_cb()) {
                break;
            }
            auto vr = eval_validate(idx);
            ++done;
            report();

            const double ts = train_score_fn(tr);
            const double vs = validate_score_fn(vr);
            const double combined = ts + validate_weight * vs;

            out.push_back(TrainValidateCandidate{ idx, tr, vr, ts, vs, combined });
        }

        std::sort(out.begin(), out.end(),
            [](const auto& a, const auto& b) { return a.combined_score > b.combined_score; });

        report(true);
        return out;
    }

    // ============================================================================
    // Modular Sweep Framework (v1) implementations
    // ============================================================================
    CoarseFineIndexSweep::CoarseFineIndexSweep(CoarseFineIndexConfig cfg)
        : cfg_(cfg) {
    }

    SweepIndexRunResult CoarseFineIndexSweep::run(
        const SweepIndexProblem& problem,
        const ScalarObjective& objective,
        SweepProgressCallback progress_cb,
        SweepCancellationCallback cancel_cb) const
    {
        if (!problem.eval) return {};
        if (!problem.neighbors) {
            // This sweep type requires neighborhood topology.
            return {};
        }

        auto two = two_stage_sweep_topk(
            problem.param_count,
            cfg_.stage1_indices ? cfg_.stage1_indices.get() : nullptr,
            problem.eval,
            objective,
            problem.neighbors,
            cfg_.k_stage1,
            cfg_.k_stage2,
            cfg_.opt,
            progress_cb,
            cancel_cb);

        SweepIndexRunResult out;
        out.has_two_stage = true;
        out.two_stage = std::move(two);
        out.best_first = out.two_stage.stage2_best_first;
        out.eval_count = out.two_stage.stage1_evaluated + out.two_stage.stage2_evaluated;
        return out;
    }

    MonteCarloIndexSweep::MonteCarloIndexSweep(MonteCarloIndexConfig cfg)
        : cfg_(cfg) {
    }

    SweepIndexRunResult MonteCarloIndexSweep::run(
        const SweepIndexProblem& problem,
        const ScalarObjective& objective,
        SweepProgressCallback progress_cb,
        SweepCancellationCallback cancel_cb) const
    {
        SweepIndexRunResult out;
        if (!problem.eval || problem.param_count == 0) return out;

        std::mt19937_64 rng;
        if (cfg_.seed == 0) {
            std::random_device rd;
            rng.seed((static_cast<std::uint64_t>(rd()) << 32) ^ static_cast<std::uint64_t>(rd()));
        }
        else {
            rng.seed(cfg_.seed);
        }
        std::uniform_int_distribution<std::size_t> dist(0, problem.param_count - 1);

        TopK<std::size_t> top(cfg_.top_k);
        const auto t0 = Clock::now();
        std::vector<std::size_t> draw_indices;
        if (cfg_.dedupe) {
            const std::size_t target = std::min<std::size_t>(cfg_.draws, problem.param_count);
            draw_indices.reserve(target);

            // Strict dedupe contract: sample without replacement.
            // Use partial shuffle for dense draws and rejection sampling for sparse draws.
            if (target > (problem.param_count / 3)) {
                draw_indices.resize(problem.param_count);
                std::iota(draw_indices.begin(), draw_indices.end(), 0);
                std::shuffle(draw_indices.begin(), draw_indices.end(), rng);
                draw_indices.resize(target);
            } else {
                std::unordered_set<std::size_t> selected;
                selected.reserve(target * 2 + 1);
                while (draw_indices.size() < target) {
                    const std::size_t idx = dist(rng);
                    if (selected.insert(idx).second) {
                        draw_indices.push_back(idx);
                    }
                }
            }
        } else {
            draw_indices.reserve(cfg_.draws);
            for (std::size_t i = 0; i < cfg_.draws; ++i) {
                draw_indices.push_back(dist(rng));
            }
        }

        const std::size_t total_draws = draw_indices.size();
        std::size_t executed_draws = 0;
        for (std::size_t i = 0; i < total_draws; ++i) {
            if (cancel_cb && cancel_cb()) {
                break;
            }
            const std::size_t idx = draw_indices[i];
            const auto res = problem.eval(idx);
            ++executed_draws;
            const double sc = objective(res);
            if (std::isfinite(sc)) {
                top.consider(sc, res, idx);
            }

            if (progress_cb && (i % 200 == 0 || i + 1 == total_draws)) {
                const auto now = Clock::now();
                const double elapsed = std::chrono::duration<double>(now - t0).count();
                const double rate = (elapsed > 1e-6) ? (static_cast<double>(i + 1) / elapsed) : 0.0;
                const double remaining = (total_draws > (i + 1)) ? static_cast<double>(total_draws - (i + 1)) : 0.0;
                const double eta = (rate > 1e-9) ? (remaining / rate) : 0.0;
                progress_cb(make_sweep_progress(
                    i + 1,
                    total_draws,
                    elapsed,
                    eta,
                    SweepProgressStage::MonteCarloSampling,
                    i + 1,
                    total_draws,
                    make_ranked_progress_candidates(top)));
            }
        }

        for (const auto& c : top.sorted_best_first()) {
            out.best_first.push_back(IndexedCandidate{ c.params, c.score, c.result });
        }
        out.eval_count = executed_draws;
        return out;
    }

    std::unique_ptr<ISweepIndex> make_sweep_index(const SweepIndexConfig& cfg)
    {
        switch (cfg.kind) {
        case SweepIndexKind::MonteCarlo:
            return std::make_unique<MonteCarloIndexSweep>(cfg.monte_carlo);
        case SweepIndexKind::CoarseFine:
        default:
            return std::make_unique<CoarseFineIndexSweep>(cfg.coarse_fine);
        }
    }

} // namespace sugar
