#include "sweep/sweep.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <queue>
#include <unordered_set>
#include <utility>

namespace sugar {

    namespace {
        using Clock = std::chrono::steady_clock;

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
        SweepCancellationCallback cancel_cb,
        SweepTraceCallback trace_cb)
    {
        const auto t0 = Clock::now();
        auto last_report = t0;

        const std::size_t stage1_total = stage1_indices ? stage1_indices->size() : param_count;
        const std::size_t stage2_est = k_stage1 * 250;
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
        std::size_t proposal_order = 0;
        const auto trace_proposal =
            [&](SweepProgressStage stage,
                std::size_t idx,
                const char* reason,
                bool has_parent,
                std::size_t parent_idx) {
                ++proposal_order;
                if (trace_cb) {
                    SweepTraceEvent event{};
                    event.kind = SweepTraceEventKind::Proposal;
                    event.stage = stage;
                    event.candidate_index = idx;
                    event.proposal_order = proposal_order;
                    event.batch_index = proposal_order - 1;
                    event.has_parent_candidate = has_parent;
                    event.parent_candidate_index = parent_idx;
                    event.reason = reason;
                    trace_cb(event);
                }
                return proposal_order;
            };
        const auto trace_observation =
            [&](SweepProgressStage stage,
                std::size_t idx,
                std::size_t order,
                double sc,
                const char* reason,
                bool has_parent,
                std::size_t parent_idx) {
                if (!trace_cb) return;
                SweepTraceEvent event{};
                event.kind = SweepTraceEventKind::Observation;
                event.stage = stage;
                event.candidate_index = idx;
                event.proposal_order = order;
                event.batch_index = order > 0 ? order - 1 : 0;
                event.has_parent_candidate = has_parent;
                event.parent_candidate_index = parent_idx;
                event.score_present = true;
                event.score_finite = std::isfinite(sc);
                event.score = sc;
                event.reason = reason;
                trace_cb(event);
            };
        const auto trace_promotion =
            [&](SweepProgressStage stage,
                std::size_t idx,
                double sc,
                const char* reason) {
                if (!trace_cb) return;
                SweepTraceEvent event{};
                event.kind = SweepTraceEventKind::Promotion;
                event.stage = stage;
                event.candidate_index = idx;
                event.proposal_order = proposal_order;
                event.batch_index = proposal_order > 0 ? proposal_order - 1 : 0;
                event.score_present = true;
                event.score_finite = std::isfinite(sc);
                event.score = sc;
                event.reason = reason;
                trace_cb(event);
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
                const std::size_t proposal = trace_proposal(
                    SweepProgressStage::CoarseSearch,
                    i,
                    "stage1_candidate",
                    false,
                    0);
                const auto res = eval(i);
                ++done;
                ++stage1_done;
                const double sc = score(res);
                if (std::isfinite(sc)) {
                    progress_top1.consider(sc, res, i);
                    top1.consider(sc, res, i);
                }
                trace_observation(
                    SweepProgressStage::CoarseSearch,
                    i,
                    proposal,
                    sc,
                    "stage1_observation",
                    false,
                    0);
                report(
                    SweepProgressStage::CoarseSearch,
                    stage1_done,
                    stage1_total,
                    make_ranked_progress_candidates(progress_top1));
            }
            for (const auto& c : top1.sorted_best_first()) {
                stage1.push_back(IndexedCandidate{ c.params, c.score, c.result });
                trace_promotion(
                    SweepProgressStage::CoarseSearch,
                    c.params,
                    c.score,
                    "stage1_seed");
            }
        }
        else {
            std::vector<double> scores(param_count, -std::numeric_limits<double>::infinity());
            std::vector<BacktestResult> results(param_count);
            std::vector<std::size_t> order;
            order.reserve(s1.size());

            for (std::size_t i : s1) {
                if ((cancel_cb && cancel_cb()) || budget_reached()) {
                    break;
                }
                if (i >= param_count) continue;
                const std::size_t proposal = trace_proposal(
                    SweepProgressStage::CoarseSearch,
                    i,
                    "stage1_candidate",
                    false,
                    0);
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
                trace_observation(
                    SweepProgressStage::CoarseSearch,
                    i,
                    proposal,
                    sc,
                    "stage1_observation",
                    false,
                    0);
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
                trace_promotion(
                    SweepProgressStage::CoarseSearch,
                    idx,
                    sc,
                    "stage1_seed");
                suppress_region(suppressed, idx, opt.suppression_hops, neighbors);
            }
        }

        report(
            SweepProgressStage::CoarseSearch,
            stage1_done,
            stage1_total,
            make_ranked_progress_candidates(stage1),
            true);

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
        bool has_current_refinement_seed = false;
        std::size_t current_refinement_seed = 0;
        auto evaluate_refinement = [&](std::size_t idx) {
            if (stop_refinement()) {
                return;
            }
            const std::size_t proposal = trace_proposal(
                SweepProgressStage::LocalRefinement,
                idx,
                "stage2_neighbor",
                has_current_refinement_seed,
                current_refinement_seed);
            const auto res = eval(idx);
            ++done;
            const double sc = score(res);
            if (std::isfinite(sc)) {
                top2.consider(sc, res, idx);
            }
            trace_observation(
                SweepProgressStage::LocalRefinement,
                idx,
                proposal,
                sc,
                "stage2_observation",
                has_current_refinement_seed,
                current_refinement_seed);
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
                    has_current_refinement_seed = true;
                    current_refinement_seed = seed.index;
                    collect_region(
                        param_count,
                        seed.index,
                        hops,
                        neighbors,
                        &seen,
                        evaluate_refinement,
                        stop_refinement);
                }
                has_current_refinement_seed = false;
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
                has_current_refinement_seed = true;
                current_refinement_seed = seed.index;
                collect_region<std::unordered_set<std::size_t>>(
                    param_count, seed.index, opt.stage2_hops, neighbors,
                    static_cast<std::unordered_set<std::size_t>*>(nullptr),
                    evaluate_refinement,
                    stop_refinement);
            }
            has_current_refinement_seed = false;
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

} // namespace sugar
