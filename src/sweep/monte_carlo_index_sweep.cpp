#include "sweep/sweep.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>
#include <random>
#include <unordered_set>
#include <utility>

namespace sugar {

    namespace {
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
    }

    MonteCarloIndexSweep::MonteCarloIndexSweep(MonteCarloIndexConfig cfg)
        : cfg_(cfg) {
    }

    SweepIndexRunResult MonteCarloIndexSweep::run(
        const SweepIndexProblem& problem,
        const ScalarObjective& objective,
        SweepProgressCallback progress_cb,
        SweepCancellationCallback cancel_cb,
        SweepTraceCallback trace_cb) const
    {
        SweepIndexRunResult out;
        if (!problem.eval || problem.param_count == 0) return out;

        std::mt19937_64 rng;
        std::uint64_t effective_seed = cfg_.seed;
        if (cfg_.seed == 0) {
            std::random_device rd;
            effective_seed =
                (static_cast<std::uint64_t>(rd()) << 32) ^
                static_cast<std::uint64_t>(rd());
        }
        rng.seed(effective_seed);
        out.effective_seed_available = true;
        out.requested_seed_explicit = (cfg_.seed != 0);
        out.requested_seed = cfg_.seed;
        out.effective_seed = effective_seed;
        std::uniform_int_distribution<std::size_t> dist(0, problem.param_count - 1);

        TopK<std::size_t> top(cfg_.top_k);
        const auto t0 = std::chrono::steady_clock::now();
        std::vector<std::size_t> draw_indices;
        if (cfg_.dedupe) {
            const std::size_t target = std::min<std::size_t>(cfg_.draws, problem.param_count);
            draw_indices.reserve(target);

            if (target > (problem.param_count / 3)) {
                draw_indices.resize(problem.param_count);
                std::iota(draw_indices.begin(), draw_indices.end(), 0);
                std::shuffle(draw_indices.begin(), draw_indices.end(), rng);
                draw_indices.resize(target);
            }
            else {
                std::unordered_set<std::size_t> selected;
                selected.reserve(target * 2 + 1);
                while (draw_indices.size() < target) {
                    const std::size_t idx = dist(rng);
                    if (selected.insert(idx).second) {
                        draw_indices.push_back(idx);
                    }
                }
            }
        }
        else {
            draw_indices.reserve(cfg_.draws);
            for (std::size_t i = 0; i < cfg_.draws; ++i) {
                draw_indices.push_back(dist(rng));
            }
        }

        const std::size_t total_draws = draw_indices.size();
        std::size_t executed_draws = 0;
        std::size_t proposal_order = 0;
        for (std::size_t i = 0; i < total_draws; ++i) {
            if (cancel_cb && cancel_cb()) {
                break;
            }
            const std::size_t idx = draw_indices[i];
            ++proposal_order;
            if (trace_cb) {
                SweepTraceEvent event{};
                event.kind = SweepTraceEventKind::Proposal;
                event.stage = SweepProgressStage::MonteCarloSampling;
                event.candidate_index = idx;
                event.proposal_order = proposal_order;
                event.batch_index = i;
                event.reason = cfg_.dedupe ? "monte_carlo_deduped_draw" : "monte_carlo_draw";
                trace_cb(event);
            }
            const auto res = problem.eval(idx);
            ++executed_draws;
            const double sc = objective(res);
            if (std::isfinite(sc)) {
                top.consider(sc, res, idx);
            }
            if (trace_cb) {
                SweepTraceEvent event{};
                event.kind = SweepTraceEventKind::Observation;
                event.stage = SweepProgressStage::MonteCarloSampling;
                event.candidate_index = idx;
                event.proposal_order = proposal_order;
                event.batch_index = i;
                event.score_present = true;
                event.score_finite = std::isfinite(sc);
                event.score = sc;
                event.reason = "monte_carlo_observation";
                trace_cb(event);
            }

            if (progress_cb && (i % 200 == 0 || i + 1 == total_draws)) {
                const auto now = std::chrono::steady_clock::now();
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

} // namespace sugar
