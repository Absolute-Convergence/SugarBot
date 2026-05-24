#include "sweep/sweep.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <queue>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace sugar {

    namespace {
        struct CandidateProposal {
            std::size_t index{ 0 };
            bool has_parent{ false };
            std::size_t parent_index{ 0 };
        };

        struct RoundEvaluation {
            std::size_t index{ 0 };
            std::size_t proposal_order{ 0 };
            double score{ -std::numeric_limits<double>::infinity() };
            BacktestResult result{};
        };

        std::size_t ceil_div(std::size_t numerator, std::size_t denominator) {
            if (denominator == 0) {
                return numerator;
            }
            return (numerator + denominator - 1) / denominator;
        }

        bool score_better(const RoundEvaluation& lhs, const RoundEvaluation& rhs) {
            const bool lhs_finite = std::isfinite(lhs.score);
            const bool rhs_finite = std::isfinite(rhs.score);
            if (lhs_finite != rhs_finite) {
                return lhs_finite;
            }
            if (lhs.score != rhs.score) {
                return lhs.score > rhs.score;
            }
            return lhs.index < rhs.index;
        }

        std::vector<SweepRankedCandidate> make_ranked_progress_candidates(
            std::vector<IndexedCandidate> best_first)
        {
            std::sort(best_first.begin(), best_first.end(),
                [](const IndexedCandidate& lhs, const IndexedCandidate& rhs) {
                    if (lhs.score != rhs.score) {
                        return lhs.score > rhs.score;
                    }
                    return lhs.index < rhs.index;
                });

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

        std::vector<CandidateProposal> make_initial_proposals(
            std::size_t param_count,
            const SuccessiveHalvingIndexConfig& cfg)
        {
            std::vector<CandidateProposal> out;
            if (param_count == 0) {
                return out;
            }

            const auto append_unique = [&out](std::size_t idx) {
                const auto duplicate = std::any_of(
                    out.begin(),
                    out.end(),
                    [idx](const CandidateProposal& candidate) {
                        return candidate.index == idx;
                    });
                if (!duplicate) {
                    out.push_back(CandidateProposal{ idx, false, 0 });
                }
            };

            if (cfg.initial_candidates > 0) {
                const std::size_t target =
                    std::min(cfg.initial_candidates, param_count);
                out.reserve(target);
                for (std::size_t i = 0; i < target; ++i) {
                    append_unique((i * param_count) / target);
                }
                return out;
            }

            if (cfg.initial_stride > 1) {
                for (std::size_t idx = 0; idx < param_count; idx += cfg.initial_stride) {
                    append_unique(idx);
                }
                return out;
            }

            const std::size_t eval_budget =
                cfg.max_evaluations > 0
                ? std::min(cfg.max_evaluations, param_count)
                : param_count;
            std::size_t target = std::max<std::size_t>(1, cfg.top_k);
            const std::size_t factor =
                std::max<std::size_t>(2, cfg.reduction_factor);
            for (std::size_t round = 1; round < cfg.rounds; ++round) {
                if (target > (std::numeric_limits<std::size_t>::max() / factor)) {
                    target = std::numeric_limits<std::size_t>::max();
                    break;
                }
                target *= factor;
            }
            target = std::max<std::size_t>(target, factor);
            target = std::min({ target, eval_budget, param_count });

            out.reserve(target);
            for (std::size_t i = 0; i < target; ++i) {
                append_unique((i * param_count) / target);
            }
            return out;
        }

    } // namespace

    SuccessiveHalvingIndexSweep::SuccessiveHalvingIndexSweep(
        SuccessiveHalvingIndexConfig cfg)
        : cfg_(std::move(cfg)) {
    }

    SweepIndexRunResult SuccessiveHalvingIndexSweep::run(
        const SweepIndexProblem& problem,
        const ScalarObjective& objective,
        SweepProgressCallback progress_cb,
        SweepCancellationCallback cancel_cb,
        SweepTraceCallback trace_cb) const
    {
        SweepIndexRunResult out;
        if (!problem.eval || !problem.neighbors || problem.param_count == 0) {
            return out;
        }

        const std::size_t rounds = std::max<std::size_t>(1, cfg_.rounds);
        const std::size_t reduction_factor =
            std::max<std::size_t>(2, cfg_.reduction_factor);
        const std::size_t top_k = std::max<std::size_t>(1, cfg_.top_k);
        const std::size_t max_evaluations =
            cfg_.max_evaluations > 0 ? cfg_.max_evaluations : problem.param_count;
        const std::size_t progress_total =
            cfg_.dedupe_candidates
            ? std::min(max_evaluations, problem.param_count)
            : max_evaluations;
        const auto t0 = std::chrono::steady_clock::now();

        std::vector<CandidateProposal> current =
            make_initial_proposals(problem.param_count, cfg_);
        std::unordered_set<std::size_t> evaluated;
        if (cfg_.dedupe_candidates) {
            evaluated.reserve(std::min(progress_total, problem.param_count));
        }

        std::vector<IndexedCandidate> all_best;
        all_best.reserve(std::min(progress_total, problem.param_count));

        std::size_t done = 0;
        std::size_t proposal_order = 0;

        const auto report = [&](bool force = false) {
            if (!progress_cb) {
                return;
            }
            if (!force && (done % 25 != 0)) {
                return;
            }
            const auto now = std::chrono::steady_clock::now();
            const double elapsed =
                std::chrono::duration<double>(now - t0).count();
            const double rate =
                elapsed > 1e-6 ? static_cast<double>(done) / elapsed : 0.0;
            const double remaining =
                progress_total > done
                ? static_cast<double>(progress_total - done)
                : 0.0;
            const double eta = rate > 1e-9 ? remaining / rate : 0.0;
            progress_cb(make_sweep_progress(
                done,
                progress_total,
                elapsed,
                eta,
                SweepProgressStage::AdaptiveSelection,
                done,
                progress_total,
                make_ranked_progress_candidates(all_best)));
        };

        const auto trace_candidate_event =
            [&](SweepTraceEventKind kind,
                std::size_t round,
                std::size_t idx,
                std::size_t order,
                const char* reason,
                bool has_parent,
                std::size_t parent_idx,
                bool score_present,
                double score_value) {
                if (!trace_cb) {
                    return;
                }
                SweepTraceEvent event{};
                event.kind = kind;
                event.stage = SweepProgressStage::AdaptiveSelection;
                event.candidate_index = idx;
                event.proposal_order = order;
                event.batch_index = round;
                event.has_parent_candidate = has_parent;
                event.parent_candidate_index = parent_idx;
                event.score_present = score_present;
                event.score_finite = score_present && std::isfinite(score_value);
                event.score = score_value;
                event.reason = reason;
                trace_cb(event);
            };

        for (std::size_t round = 0; round < rounds && !current.empty(); ++round) {
            if (cancel_cb && cancel_cb()) {
                break;
            }

            std::vector<RoundEvaluation> round_results;
            round_results.reserve(current.size());

            for (const auto& candidate : current) {
                if (cancel_cb && cancel_cb()) {
                    break;
                }
                if (done >= max_evaluations) {
                    break;
                }
                if (candidate.index >= problem.param_count) {
                    continue;
                }
                if (cfg_.dedupe_candidates &&
                    !evaluated.insert(candidate.index).second) {
                    continue;
                }

                ++proposal_order;
                const std::string proposal_reason =
                    round == 0
                    ? "successive_halving_initial_candidate"
                    : "successive_halving_neighbor_candidate";
                trace_candidate_event(
                    SweepTraceEventKind::Proposal,
                    round,
                    candidate.index,
                    proposal_order,
                    proposal_reason.c_str(),
                    candidate.has_parent,
                    candidate.parent_index,
                    false,
                    0.0);

                const BacktestResult result = problem.eval(candidate.index);
                ++done;
                const double score = objective(result);

                trace_candidate_event(
                    SweepTraceEventKind::Observation,
                    round,
                    candidate.index,
                    proposal_order,
                    "successive_halving_observation",
                    candidate.has_parent,
                    candidate.parent_index,
                    true,
                    score);

                round_results.push_back(
                    RoundEvaluation{ candidate.index, proposal_order, score, result });
                if (std::isfinite(score)) {
                    all_best.push_back(
                        IndexedCandidate{ candidate.index, score, result });
                }
                report();
            }

            if (round_results.empty()) {
                break;
            }

            std::sort(round_results.begin(), round_results.end(), score_better);
            const std::size_t finite_count = static_cast<std::size_t>(
                std::count_if(
                    round_results.begin(),
                    round_results.end(),
                    [](const RoundEvaluation& candidate) {
                        return std::isfinite(candidate.score);
                    }));
            const std::size_t survivor_count =
                std::min(
                    finite_count,
                    std::max<std::size_t>(
                        1,
                        ceil_div(round_results.size(), reduction_factor)));

            std::vector<RoundEvaluation> survivors;
            survivors.reserve(survivor_count);
            for (std::size_t i = 0; i < round_results.size(); ++i) {
                const auto& candidate = round_results[i];
                const bool promoted = i < survivor_count;
                trace_candidate_event(
                    promoted
                        ? SweepTraceEventKind::Promotion
                        : SweepTraceEventKind::Rejection,
                    round,
                    candidate.index,
                    candidate.proposal_order,
                    promoted
                        ? "successive_halving_promoted"
                        : "successive_halving_rejected",
                    false,
                    0,
                    true,
                    candidate.score);
                if (promoted) {
                    survivors.push_back(candidate);
                }
            }

            if (round + 1 >= rounds || done >= max_evaluations ||
                (cancel_cb && cancel_cb())) {
                break;
            }

            std::vector<CandidateProposal> next;
            std::unordered_set<std::size_t> queued;
            queued.reserve(survivors.size() * 8 + 1);
            const std::size_t hops =
                std::max<std::size_t>(1, cfg_.neighborhood_hops);

            for (const auto& seed : survivors) {
                std::queue<std::pair<std::size_t, std::size_t>> q;
                std::unordered_set<std::size_t> local_seen;
                q.push({ seed.index, 0 });

                while (!q.empty()) {
                    const auto [idx, depth] = q.front();
                    q.pop();
                    if (idx >= problem.param_count) {
                        continue;
                    }
                    if (!local_seen.insert(idx).second) {
                        continue;
                    }
                    if (!(cfg_.dedupe_candidates && evaluated.find(idx) != evaluated.end()) &&
                        queued.insert(idx).second) {
                        next.push_back(CandidateProposal{ idx, true, seed.index });
                    }
                    if (depth >= hops) {
                        continue;
                    }
                    for (const auto nb : problem.neighbors(idx)) {
                        if (nb < problem.param_count) {
                            q.push({ nb, depth + 1 });
                        }
                    }
                }
            }

            current = std::move(next);
        }

        std::sort(all_best.begin(), all_best.end(),
            [](const IndexedCandidate& lhs, const IndexedCandidate& rhs) {
                if (lhs.score != rhs.score) {
                    return lhs.score > rhs.score;
                }
                return lhs.index < rhs.index;
            });
        if (all_best.size() > top_k) {
            all_best.resize(top_k);
        }

        out.best_first = std::move(all_best);
        out.eval_count = done;
        report(true);
        return out;
    }

} // namespace sugar
