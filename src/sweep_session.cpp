#include "sweep.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace sugar {

    namespace {
        using Clock = std::chrono::steady_clock;
    }

    FunctionTrialEvaluator::FunctionTrialEvaluator(
        std::function<BacktestResult(const CandleSeries&, const SweepCandidateToken&)> fn)
        : fn_(std::move(fn)) {
    }

    BacktestResult FunctionTrialEvaluator::evaluate(
        const CandleSeries& data,
        const SweepCandidateToken& token) const
    {
        if (!fn_) return BacktestResult{};
        return fn_(data, token);
    }

    FunctionObjective::FunctionObjective(std::function<double(const BacktestResult&)> fn)
        : fn_(std::move(fn)) {
    }

    double FunctionObjective::score(const BacktestResult& result) const
    {
        if (!fn_) return -std::numeric_limits<double>::infinity();
        return fn_(result);
    }

    ExhaustiveIndexTopKPolicy::ExhaustiveIndexTopKPolicy(std::size_t batch_size)
        : batch_size_(std::max<std::size_t>(std::size_t{ 1 }, batch_size)) {
    }

    void ExhaustiveIndexTopKPolicy::begin(const ISweepSpace& space, std::size_t top_k)
    {
        total_ = space.size();
        next_ = 0;
        top_ = TopK<std::size_t>(top_k);
    }

    SweepProposalBatch ExhaustiveIndexTopKPolicy::propose()
    {
        SweepProposalBatch batch{};
        if (next_ >= total_) {
            batch.done = true;
            return batch;
        }

        const std::size_t end = std::min<std::size_t>(total_, next_ + batch_size_);
        batch.tokens.reserve(end - next_);
        for (std::size_t i = next_; i < end; ++i) {
            batch.tokens.push_back(SweepCandidateToken{ i });
        }
        next_ = end;
        batch.done = (next_ >= total_);
        return batch;
    }

    void ExhaustiveIndexTopKPolicy::observe(const SweepObservation& obs)
    {
        if (!std::isfinite(obs.score)) return;
        top_.consider(obs.score, obs.result, obs.token.id);
    }

    std::vector<IndexedCandidate> ExhaustiveIndexTopKPolicy::best_first() const
    {
        std::vector<IndexedCandidate> out;
        for (const auto& c : top_.sorted_best_first()) {
            out.push_back(IndexedCandidate{ c.params, c.score, c.result });
        }
        return out;
    }

    SweepSessionResult run_sweep_session(
        const CandleSeries& data,
        const ISweepSpace& space,
        const ITrialEvaluator& evaluator,
        const IObjective& objective,
        ISweepPolicy& policy,
        const SweepSessionConfig& cfg)
    {
        const auto t0 = Clock::now();
        const std::size_t total = space.size();
        std::size_t done = 0;

        policy.begin(space, cfg.top_k);

        auto report = [&](bool force = false) {
            if (!cfg.progress_cb) return;
            const std::size_t every = std::max<std::size_t>(std::size_t{ 1 }, cfg.report_every);
            if (!force && (done % every != 0)) return;

            const auto now = Clock::now();
            const double elapsed = std::chrono::duration<double>(now - t0).count();
            const double rate = (elapsed > 1e-6) ? (static_cast<double>(done) / elapsed) : 0.0;
            const double remaining = (total > done) ? static_cast<double>(total - done) : 0.0;
            const double eta = (rate > 1e-9) ? (remaining / rate) : 0.0;
            cfg.progress_cb(SweepProgress{ done, total, elapsed, eta });
            };

        for (;;) {
            SweepProposalBatch batch = policy.propose();
            for (const auto& token : batch.tokens) {
                const auto res = evaluator.evaluate(data, token);
                const double sc = objective.score(res);
                policy.observe(SweepObservation{ token, res, sc });
                ++done;
                report();
            }

            if (batch.done) break;
            if (batch.tokens.empty()) break; // defensive guard
        }

        report(true);

        SweepSessionResult out{};
        out.best_first = policy.best_first();
        out.eval_count = done;
        out.total_candidates = total;
        out.policy_name = policy.name();
        return out;
    }

} // namespace sugar
