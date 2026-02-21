#include "sweep.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <limits>
#include <queue>
#include <unordered_set>


namespace sugar {

    using Clock = std::chrono::steady_clock;

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
                << p.done << "/" << p.total
                << " elapsed=" << static_cast<int>(p.elapsed_sec) << "s"
                << " eta~" << static_cast<int>(p.eta_sec) << "s"
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

                // Expand [min,max] by step (inclusive).
                // Guard against runaway expansions by capping at sw.max_evals (per-dim).
                for (double x = a; x <= b + 1e-12; x += step) {
                    d.values.push_back(x);
                    if (d.values.size() > sw.max_evals) break;
                }
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
        // Uses `seen` to optionally dedupe across different seeds.
        template <typename SeenT>
        void collect_region(
            std::size_t param_count,
            std::size_t start_idx,
            std::size_t hops,
            const std::function<std::vector<std::size_t>(std::size_t)>& neighbors,
            SeenT* seen,
            const std::function<void(std::size_t)>& on_index)
        {
            if (start_idx >= param_count) return;

            std::queue<std::pair<std::size_t, std::size_t>> q;
            q.push({ start_idx, 0 });

            while (!q.empty()) {
                auto [idx, depth] = q.front();
                q.pop();

                if (idx >= param_count) continue;
                if (seen) {
                    if (!seen->insert(idx).second) continue;
                }

                on_index(idx);

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
        const CandleSeries& data,
        std::size_t param_count,
        const std::vector<std::size_t>* stage1_indices,
        const std::function<BacktestResult(const CandleSeries&, std::size_t)>& eval,
        const std::function<double(const BacktestResult&)>& score,
        const std::function<std::vector<std::size_t>(std::size_t)>& neighbors,
        std::size_t k_stage1,
        std::size_t k_stage2,
        TwoStageSweepOptions opt,
        SweepProgressCallback progress_cb)
    {
        const auto t0 = Clock::now();
        auto last_report = t0;

        // Stage 1 total is exact; stage 2 is estimated (neighbor BFS).
        const std::size_t stage1_total = stage1_indices ? stage1_indices->size() : param_count;
        const std::size_t stage2_est = k_stage1 * 250; // ~243 for radius=1 hypercube-ish
        const std::size_t total_est = stage1_total + stage2_est;

        std::size_t done = 0;

        auto report = [&](bool force = false) {
            if (!progress_cb) return;
            const auto now = Clock::now();
            if (!force && (now - last_report) < std::chrono::milliseconds(250)) return;
            last_report = now;

            const double elapsed = std::chrono::duration<double>(now - t0).count();
            const double rate = (elapsed > 1e-6) ? (static_cast<double>(done) / elapsed) : 0.0;
            const double remaining = (total_est > done) ? static_cast<double>(total_est - done) : 0.0;
            const double eta = (rate > 1e-9) ? (remaining / rate) : 0.0;
            progress_cb(SweepProgress{ done, total_est, elapsed, eta });
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

        if (!opt.depollute_stage1) {
            TopK<std::size_t> top1(k_stage1);
            for (std::size_t i : s1) {
                if (i >= param_count) continue;
                const auto res = eval(data, i);
                ++done;
                report();
                const double sc = score(res);
                if (!std::isfinite(sc)) continue;
                top1.consider(sc, res, i);
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
                if (i >= param_count) continue;
                const auto res = eval(data, i);
                ++done;
                report();
                double sc = score(res);
                if (!std::isfinite(sc)) sc = -std::numeric_limits<double>::infinity();
                results[i] = res;
                scores[i] = sc;
                order.push_back(i);
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

        // -------------------------
        // Stage 2
        // -------------------------
        TopK<std::size_t> top2(k_stage2);
        std::unordered_set<std::size_t> seen;
        if (opt.dedupe_stage2) {
            seen.reserve(stage1.size() * 64);
        }

        for (const auto& seed : stage1) {
            if (opt.dedupe_stage2) {
                collect_region(param_count, seed.index, opt.stage2_hops, neighbors, &seen,
                    [&](std::size_t idx) {
                        const auto res = eval(data, idx);
                        ++done;
                        report();
                        const double sc = score(res);
                        if (!std::isfinite(sc)) return;
                        top2.consider(sc, res, idx);
                    });
            }
            else {
                collect_region<std::unordered_set<std::size_t>>(
                    param_count, seed.index, opt.stage2_hops, neighbors,
                    (std::unordered_set<std::size_t>*)nullptr,
                    [&](std::size_t idx) {
                        const auto res = eval(data, idx);
                        ++done;
                        report();
                        const double sc = score(res);
                        if (!std::isfinite(sc)) return;
                        top2.consider(sc, res, idx);
                    });
            }
        }

        std::vector<IndexedCandidate> stage2;
        for (const auto& c : top2.sorted_best_first()) {
            stage2.push_back(IndexedCandidate{ c.params, c.score, c.result });
        }

        report(true);

        TwoStageSweepResult out;
        out.stage1_best_first = std::move(stage1);
        out.stage2_best_first = std::move(stage2);
        out.stage1_evaluated = stage1_total;
        out.stage2_evaluated = (done > stage1_total) ? (done - stage1_total) : 0;
        return out;
    }

    // ============================================================================
    // Train / validate evaluation
    // ============================================================================
    std::vector<TrainValidateCandidate> evaluate_train_validate(
        const CandleSeries& train,
        const CandleSeries& validate,
        const std::vector<std::size_t>& indices,
        const std::function<BacktestResult(const CandleSeries&, std::size_t)>& eval,
        const std::function<double(const BacktestResult&)>& train_score_fn,
        const std::function<double(const BacktestResult&)>& validate_score_fn,
        double validate_weight,
        SweepProgressCallback progress_cb)
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
            progress_cb(SweepProgress{ done, total, elapsed, eta });
            };

        std::vector<TrainValidateCandidate> out;
        out.reserve(indices.size());

        for (auto idx : indices) {
            auto tr = eval(train, idx);
            ++done;
            report();

            auto vr = eval(validate, idx);
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
        const CandleSeries& data,
        const SweepIndexProblem& problem,
        const ScalarObjective& objective,
        SweepProgressCallback progress_cb) const
    {
        if (!problem.eval) return {};
        if (!problem.neighbors) {
            // This sweep type requires neighborhood topology.
            return {};
        }

        auto two = two_stage_sweep_topk(
            data,
            problem.param_count,
            cfg_.stage1_indices ? cfg_.stage1_indices.get() : nullptr,
            problem.eval,
            objective,
            problem.neighbors,
            cfg_.k_stage1,
            cfg_.k_stage2,
            cfg_.opt,
            progress_cb);

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
        const CandleSeries& data,
        const SweepIndexProblem& problem,
        const ScalarObjective& objective,
        SweepProgressCallback progress_cb) const
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
        std::unordered_set<std::size_t> seen;
        if (cfg_.dedupe) {
            seen.reserve(std::min<std::size_t>(cfg_.draws, cfg_.top_k * 64));
        }

        const auto t0 = Clock::now();
        for (std::size_t i = 0; i < cfg_.draws; ++i) {
            std::size_t idx = dist(rng);
            if (cfg_.dedupe) {
                if (!seen.insert(idx).second) {
                    // resample a couple times to avoid wasting draws
                    for (int tries = 0; tries < 3; ++tries) {
                        idx = dist(rng);
                        if (seen.insert(idx).second) break;
                    }
                }
            }

            const auto res = problem.eval(data, idx);
            const double sc = objective(res);
            if (std::isfinite(sc)) {
                top.consider(sc, res, idx);
            }

            if (progress_cb && (i % 200 == 0 || i + 1 == cfg_.draws)) {
                const auto now = Clock::now();
                const double elapsed = std::chrono::duration<double>(now - t0).count();
                const double rate = (elapsed > 1e-6) ? (static_cast<double>(i + 1) / elapsed) : 0.0;
                const double remaining = (cfg_.draws > (i + 1)) ? static_cast<double>(cfg_.draws - (i + 1)) : 0.0;
                const double eta = (rate > 1e-9) ? (remaining / rate) : 0.0;
                progress_cb(SweepProgress{ i + 1, cfg_.draws, elapsed, eta });
            }
        }

        for (const auto& c : top.sorted_best_first()) {
            out.best_first.push_back(IndexedCandidate{ c.params, c.score, c.result });
        }
        out.eval_count = cfg_.draws;
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
