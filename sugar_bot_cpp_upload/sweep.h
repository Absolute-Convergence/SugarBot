#pragma once
#include <algorithm>
#include <cstddef>
#include <cmath>
#include <limits>
#include <queue>
#include <tuple>
#include <utility>
#include <vector>
#include <functional>
#include <unordered_set>
#include <unordered_map>
#include <cstdint>
#include <chrono>
#include <iosfwd>
#include <memory>
#include <random>

#include "metrics.h"
#include "series.h"

namespace sugar {

    // ============================================================================
    // Generalized sweep utilities
    // ============================================================================

    // -------------------------
    // Scoring policy
    // -------------------------
    struct ScorePolicy {
        // Default score: pnl - dd_penalty * max_drawdown
        double dd_penalty = 0.25;

        // Minimum trades constraint (0 = disabled)
#ifdef _DEBUG
        std::size_t min_trades = 5;
#else
        std::size_t min_trades = 0;
#endif

        // Require finite pnl/dd
        bool require_finite = true;
    };

    // -------------------------
    // Basin-aware Top-K policy
    // -------------------------
    struct BasinPolicy {
        // Max candidates kept per basin during sweep
        std::size_t max_per_basin = 3;

        // Minimum "distance" (in param step-index units) required to keep two
        // distinct candidates within the same basin.
        std::size_t min_param_distance = 4;

        // Always include the global best candidate (by score) as insurance.
        bool keep_global_best = true;

        // Dedupe *exact* execution behavior by BacktestResult::trade_hash
        // when emitting final Top-K.
        bool dedupe_trade_hash = true;
    };

    inline double default_sweep_score(const BacktestResult& r,
        const ScorePolicy& p)
    {
        if (p.require_finite) {
            if (!std::isfinite(r.pnl) || !std::isfinite(r.max_drawdown)) {
                return -std::numeric_limits<double>::infinity();
            }
        }
        if (r.trades < p.min_trades) {
            return -std::numeric_limits<double>::infinity();
        }
        return r.pnl - p.dd_penalty * r.max_drawdown;
    }

    // -------------------------
    // Generic param sweep helper
    // -------------------------
    struct SweepProgress {
        std::size_t done = 0;
        std::size_t total = 0;
        double elapsed_sec = 0.0;
        double eta_sec = 0.0;
    };
    using SweepProgressCallback = std::function<void(const SweepProgress&)>;

    // -------------------------
    // Generic Top-K container
    // -------------------------
    template <typename ParamsT>
    struct ScoredCandidate {
        double score{};
        BacktestResult result{};
        ParamsT params{};
    };

    template <typename ParamsT>
    class TopK {
    public:
        explicit TopK(std::size_t k) : k_(k) {}

        void consider(double score,
            const BacktestResult& r,
            const ParamsT& p)
        {
            if (k_ == 0) return;
            heap_.push(ScoredCandidate<ParamsT>{ score, r, p });
            if (heap_.size() > k_) heap_.pop();
        }

        std::vector<ScoredCandidate<ParamsT>> sorted_best_first() const {
            std::vector<ScoredCandidate<ParamsT>> out;
            auto copy = heap_;
            while (!copy.empty()) {
                out.push_back(copy.top());
                copy.pop();
            }
            std::sort(out.begin(), out.end(),
                [](const auto& a, const auto& b) {
                    return a.score > b.score;
                });
            return out;
        }

    private:
        struct Cmp {
            bool operator()(const ScoredCandidate<ParamsT>& a,
                const ScoredCandidate<ParamsT>& b) const {
                // min-heap by score
                return a.score > b.score;
            }
        };

        std::size_t k_{ 0 };
        std::priority_queue<
            ScoredCandidate<ParamsT>,
            std::vector<ScoredCandidate<ParamsT>>,
            Cmp
        > heap_;
    };

    // -------------------------
    // Generic param sweep helper
    // -------------------------
    template <typename ParamsT, typename EvalFn, typename ScoreFn>
    TopK<ParamsT> sweep_topk(
        const CandleSeries& data,
        const std::vector<ParamsT>& params,
        EvalFn&& eval,
        ScoreFn&& score,
        std::size_t k,
        SweepProgressCallback progress_cb = nullptr)
    {
        const auto t0 = std::chrono::steady_clock::now();
        std::size_t done = 0;
        const std::size_t total = params.size();

        auto report = [&](bool force = false) {
            if (!progress_cb) return;
            if (!force && (done % 50 != 0)) return; // cheap pacing; ostream cb also throttles
            const auto now = std::chrono::steady_clock::now();
            const double elapsed = std::chrono::duration<double>(now - t0).count();
            const double rate = (elapsed > 1e-6) ? (static_cast<double>(done) / elapsed) : 0.0;
            const double remaining = (total > done) ? static_cast<double>(total - done) : 0.0;
            const double eta = (rate > 1e-9) ? (remaining / rate) : 0.0;
            progress_cb(SweepProgress{ done, total, elapsed, eta });
            };

        TopK<ParamsT> top(k);
        for (const auto& p : params) {
            auto res = eval(data, p);
            ++done;
            report();

            const double sc = score(res);
            if (!std::isfinite(sc)) continue;
            top.consider(sc, res, p);
        }

        report(true);
        return top;
    }

    // -------------------------
    // Generic param sweep helper (unique / depolluted)
    //
    // Uses a key function to suppress near-duplicates (e.g., bucketed params).
    // This is intentionally sweep-layer logic: strategies can choose the key.
    // -------------------------
    template <typename ParamsT, typename KeyT,
        typename EvalFn, typename ScoreFn, typename KeyFn,
        typename HashFn = std::hash<KeyT>, typename EqFn = std::equal_to<KeyT>>
        TopK<ParamsT> sweep_topk_unique(
            const CandleSeries& data,
            const std::vector<ParamsT>& params,
            EvalFn&& eval,
            ScoreFn&& score,
            std::size_t k,
            KeyFn&& key_fn,
            std::size_t oversample_mult = 10,
            SweepProgressCallback progress_cb = nullptr,
            HashFn hash_fn = {},
            EqFn eq_fn = {})
    {
        if (k == 0) return TopK<ParamsT>(0);

        const auto t0 = std::chrono::steady_clock::now();
        std::size_t done = 0;
        const std::size_t total = params.size();

        auto report = [&](bool force = false) {
            if (!progress_cb) return;
            if (!force && (done % 50 != 0)) return;
            const auto now = std::chrono::steady_clock::now();
            const double elapsed = std::chrono::duration<double>(now - t0).count();
            const double rate = (elapsed > 1e-6) ? (static_cast<double>(done) / elapsed) : 0.0;
            const double remaining = (total > done) ? static_cast<double>(total - done) : 0.0;
            const double eta = (rate > 1e-9) ? (remaining / rate) : 0.0;
            progress_cb(SweepProgress{ done, total, elapsed, eta });
            };

        const std::size_t oversample_k = std::max<std::size_t>(k, k * oversample_mult);
        TopK<ParamsT> provisional(oversample_k);

        for (const auto& p : params) {
            auto res = eval(data, p);
            ++done;
            report();

            const double sc = score(res);
            if (!std::isfinite(sc)) continue;
            provisional.consider(sc, res, p);
        }

        report(true);

        auto best = provisional.sorted_best_first();

        TopK<ParamsT> out(k);
        std::unordered_set<KeyT, HashFn, EqFn> seen(0, hash_fn, eq_fn);
        seen.reserve(best.size());

        std::size_t kept = 0;
        for (const auto& c : best) {
            const KeyT key = key_fn(c.params, c.result);
            if (!seen.insert(key).second) continue;
            out.consider(c.score, c.result, c.params);
            if (++kept >= k) break;
        }

        return out;
    }

    // -------------------------
    // Basin-aware Top-K 
    //   - Key defines a "basin" (bucketed params or coarse cell)
    //   - Keeps up to policy.max_per_basin candidates per basin
    //   - Optional min distance inside basin to avoid near-identical knob spam
    //   - Keeps global best as insurance
    //   - Optional output dedupe by trade_hash (exact behavior)
    //   - Deterministic basin tie-break via first-seen index
    // -------------------------
    template <
        typename ParamsT,
        typename KeyT,
        typename EvalFn,
        typename ScoreFn,
        typename KeyFn,
        typename DistFn,
        typename HashFn = std::hash<KeyT>,
        typename EqFn = std::equal_to<KeyT>
    >
    TopK<ParamsT> sweep_topk_basins(
        const CandleSeries& data,
        const std::vector<ParamsT>& params,
        EvalFn&& eval,
        ScoreFn&& score,
        std::size_t k_total,
        KeyFn&& basin_key_fn,
        DistFn&& param_distance_fn,
        const BasinPolicy& policy,
        SweepProgressCallback progress_cb = nullptr,
        HashFn hash_fn = {},
        EqFn eq_fn = {},
        std::size_t* out_basin_count = nullptr)
    {
        if (k_total == 0) return TopK<ParamsT>(0);

        struct Cand {
            double score = -std::numeric_limits<double>::infinity();
            BacktestResult result{};
            ParamsT params{};
        };

        struct Basin {
            std::size_t first_seen = 0; // deterministic tiebreak
            std::vector<Cand> best;     // sorted best-first
        };

        const auto t0 = std::chrono::steady_clock::now();
        const std::size_t total = params.size();

        auto report = [&](std::size_t done, bool force = false) {
            if (!progress_cb) return;
            if (!force && (done % 50 != 0)) return;
            const auto now = std::chrono::steady_clock::now();
            const double elapsed = std::chrono::duration<double>(now - t0).count();
            const double rate = (elapsed > 1e-6) ? (static_cast<double>(done) / elapsed) : 0.0;
            const double remaining = (total > done) ? static_cast<double>(total - done) : 0.0;
            const double eta = (rate > 1e-9) ? (remaining / rate) : 0.0;
            progress_cb(SweepProgress{ done, total, elapsed, eta });
            };

        const std::size_t max_per_basin = std::max<std::size_t>(1, policy.max_per_basin);
        const std::size_t min_dist = policy.min_param_distance;

        std::unordered_map<KeyT, Basin, HashFn, EqFn> basins(0, hash_fn, eq_fn);
        basins.reserve(std::min<std::size_t>(params.size(), 200000));

        bool have_global = false;
        Cand global_best{};

        auto consider_in_basin = [&](Basin& b, const Cand& c) {
            // Distance suppression inside basin:
            // if "too close", keep only the better one (elite replacement).
            if (min_dist > 0) {
                for (auto& existing : b.best) {
                    const auto d = static_cast<std::size_t>(param_distance_fn(existing.params, c.params));
                    if (d < min_dist) {
                        if (c.score > existing.score) existing = c;
                        std::sort(b.best.begin(), b.best.end(),
                            [](auto const& x, auto const& y) { return x.score > y.score; });
                        return;
                    }
                }
            }

            b.best.push_back(c);
            std::sort(b.best.begin(), b.best.end(),
                [](auto const& x, auto const& y) { return x.score > y.score; });

            if (b.best.size() > max_per_basin) b.best.resize(max_per_basin);
            };

        for (std::size_t i = 0; i < params.size(); ++i) {
            const auto& p = params[i];

            BacktestResult res = eval(data, p);
            const double sc = score(res);

            report(i + 1);

            if (!std::isfinite(sc)) continue;

            Cand c{ sc, std::move(res), p };

            if (policy.keep_global_best) {
                if (!have_global || c.score > global_best.score) {
                    global_best = c;
                    have_global = true;
                }
            }

            const KeyT key = basin_key_fn(p, c.result);
            auto it = basins.find(key);
            if (it == basins.end()) {
                Basin b;
                b.first_seen = i;
                b.best.reserve(max_per_basin);
                b.best.push_back(c);
                basins.emplace(key, std::move(b));
            }
            else {
                consider_in_basin(it->second, c);
            }
        }

        report(total, true);

        if (out_basin_count) *out_basin_count = basins.size();

        // Rank basins by their best score; deterministic tiebreak via first_seen.
        struct BasinRef {
            const Basin* basin{};
            double best_score{};
        };

        std::vector<BasinRef> ranked;
        ranked.reserve(basins.size());
        for (auto const& kv : basins) {
            if (kv.second.best.empty()) continue;
            ranked.push_back(BasinRef{ &kv.second, kv.second.best.front().score });
        }

        std::sort(ranked.begin(), ranked.end(),
            [](auto const& a, auto const& b) {
                if (a.best_score != b.best_score) return a.best_score > b.best_score;
                return a.basin->first_seen < b.basin->first_seen;
            });

        // Emit: interleave basins, optionally dedupe exact behavior by trade_hash.
        std::unordered_set<std::uint64_t> seen_hash;
        if (policy.dedupe_trade_hash) seen_hash.reserve(k_total * 4);

        std::vector<Cand> selected;
        selected.reserve(k_total);

        if (policy.keep_global_best && have_global) {
            if (!policy.dedupe_trade_hash || seen_hash.insert(global_best.result.trade_hash).second) {
                selected.push_back(global_best);
            }
        }

        for (auto const& br : ranked) {
            for (auto const& c : br.basin->best) {
                if (selected.size() >= k_total) break;
                if (policy.dedupe_trade_hash) {
                    if (!seen_hash.insert(c.result.trade_hash).second) continue;
                }
                selected.push_back(c);
            }
            if (selected.size() >= k_total) break;
        }

        TopK<ParamsT> out(k_total);
        for (auto const& c : selected) {
            out.consider(c.score, c.result, c.params);
        }
        return out;
    }

    // Back-compat overload:
    // (Older signature: key_fn + max_per_key + optional distance suppression + keep_global_best)
    template <typename ParamsT, typename KeyT,
        typename EvalFn, typename ScoreFn, typename KeyFn,
        typename DistFn = std::nullptr_t,
        typename HashFn = std::hash<KeyT>, typename EqFn = std::equal_to<KeyT>>
        TopK<ParamsT> sweep_topk_basins(
            const CandleSeries& data,
            const std::vector<ParamsT>& params,
            EvalFn&& eval,
            ScoreFn&& score,
            std::size_t k_total,
            KeyFn&& key_fn,
            std::size_t max_per_key = 2,
            std::size_t min_param_distance = 0,
            DistFn dist_fn = nullptr,
            bool keep_global_best = true,
            SweepProgressCallback progress_cb = nullptr,
            HashFn hash_fn = {},
            EqFn eq_fn = {})
    {
        BasinPolicy pol{};
        pol.max_per_basin = std::max<std::size_t>(1, max_per_key);
        pol.min_param_distance = min_param_distance;
        pol.keep_global_best = keep_global_best;
        pol.dedupe_trade_hash = true;

        auto dist = [&](const ParamsT& a, const ParamsT& b) -> std::size_t {
            if constexpr (std::is_same_v<DistFn, std::nullptr_t>) {
                (void)a; (void)b;
                return 0;
            }
            else {
                if (!dist_fn) return 0;
                return static_cast<std::size_t>(dist_fn(a, b));
            }
            };

        return sweep_topk_basins<ParamsT, KeyT>(
            data,
            params,
            std::forward<EvalFn>(eval),
            std::forward<ScoreFn>(score),
            k_total,
            std::forward<KeyFn>(key_fn),
            dist,
            pol,
            progress_cb,
            hash_fn,
            eq_fn,
            nullptr);
    }

    // -------------------------
    // Reporting helpers
    // -------------------------
    // Creates a throttled progress callback that prints to the given stream.
    // This keeps reporting out of strategies and prevents output from
    // accidentally contaminating sweep logic.
    SweepProgressCallback make_ostream_progress_cb(
        std::ostream& out,
        std::chrono::milliseconds throttle = std::chrono::milliseconds(250),
        const char* prefix = "  [sweep] ");

    // ============================================================================
    // Index-based helpers (for coarse→fine + gated strategies)
    // ============================================================================

    // Candidate identified by index (params stored elsewhere)
    struct IndexedCandidate {
        std::size_t index{};
        double score{};
        BacktestResult result{};
    };

    struct TwoStageSweepResult {
        std::vector<IndexedCandidate> stage1_best_first;
        std::vector<IndexedCandidate> stage2_best_first;

        // Diagnostics
        std::size_t stage1_evaluated = 0;
        std::size_t stage2_evaluated = 0;
    };

    struct TwoStageSweepOptions {
        // If true, stage-1 seeds are selected with basin suppression (diversity),
        // using the provided neighbors() definition as the "local region".
        bool depollute_stage1 = true;

        // If true, stage-2 neighbor indices are deduplicated with a visited set.
        bool dedupe_stage2 = true;

        // How many neighbor-hops to suppress around a chosen seed in stage1.
        // 1 means suppress idx and its direct neighbors.
        std::size_t suppression_hops = 1;

        // How many neighbor-expansion hops to explore around each stage-1 seed in stage2.
        // 1 means evaluate the seed and its direct neighbors; 2 expands one more ring.
        std::size_t stage2_hops = 1;
    };

    // Coarse → fine sweep using index indirection
    TwoStageSweepResult two_stage_sweep_topk(
        const CandleSeries& data,
        std::size_t param_count,
        // Optional: restrict stage-1 evaluation to this subset of indices (coarse mesh).
        // If null, stage-1 evaluates [0, param_count).
        const std::vector<std::size_t>* stage1_indices,
        const std::function<BacktestResult(const CandleSeries&, std::size_t)>& eval,
        const std::function<double(const BacktestResult&)>& score,
        const std::function<std::vector<std::size_t>(std::size_t)>& neighbors,
        std::size_t k_stage1,
        std::size_t k_stage2,
        TwoStageSweepOptions opt = {},
        SweepProgressCallback progress_cb = nullptr
    );

    // -------------------------
    // Train / Validate evaluation
    // -------------------------
    struct TrainValidateCandidate {
        std::size_t index{};
        BacktestResult train{};
        BacktestResult validate{};
        double train_score{};
        double validate_score{};
        double combined_score{};
    };

    std::vector<TrainValidateCandidate> evaluate_train_validate(
        const CandleSeries& train,
        const CandleSeries& validate,
        const std::vector<std::size_t>& indices,
        const std::function<BacktestResult(const CandleSeries&, std::size_t)>& eval,
        const std::function<double(const BacktestResult&)>& train_score_fn,
        const std::function<double(const BacktestResult&)>& validate_score_fn,
        double validate_weight = 1.0,
        SweepProgressCallback progress_cb = nullptr
    );

    // ============================================================================
    // Modular Sweep Framework (v1)
    //
    // Goal:
    //   - Keep strategies as adapters (param space + eval), not sweep engines.
    //   - Allow executor/menu to choose sweep types via config.
    //   - Keep primitives (objective, collector, reporting) reusable.
    //
    // This first version is intentionally index-based because your existing
    // coarse→fine machinery already works on mixed-radix packed indices.
    // Param-struct sweeps can be added later as a parallel path.
    // ============================================================================

    // Strategy-side adapter: defines an index-addressable parameter space.
    struct SweepIndexProblem {
        std::size_t param_count = 0;

        // Required: evaluate an index candidate.
        std::function<BacktestResult(const CandleSeries&, std::size_t)> eval;

        // Optional: neighborhood topology for local refinement.
        // If empty, sweep types that require neighbors should fail fast.
        std::function<std::vector<std::size_t>(std::size_t)> neighbors;
    };

    // Scalar objective (v1). Future versions can extend this to vector objectives
    // (Pareto), likelihood/posterior, expected utility, etc.
    using ScalarObjective = std::function<double(const BacktestResult&)>;

    // Unified sweep output for index-based sweep types.
    struct SweepIndexRunResult {
        // Always populated: best candidates, sorted best-first.
        std::vector<IndexedCandidate> best_first;

        // Optional diagnostics (used by coarse→fine).
        TwoStageSweepResult two_stage;
        bool has_two_stage = false;
    };

    // Base interface for index-based sweep types.
    class ISweepIndex {
    public:
        virtual ~ISweepIndex() = default;
        virtual const char* name() const = 0;

        virtual SweepIndexRunResult run(
            const CandleSeries& data,
            const SweepIndexProblem& problem,
            const ScalarObjective& objective,
            SweepProgressCallback progress_cb = nullptr) const = 0;
    };

    // -------------------------
    // Sweep type: Coarse→Fine (existing algorithm wrapped as an object)
    // -------------------------
    struct CoarseFineIndexConfig {
        // Optional: stage-1 index subset (coarse mesh). If null, stage-1 scans all.
        const std::vector<std::size_t>* stage1_indices = nullptr;

        std::size_t k_stage1 = 25;
        std::size_t k_stage2 = 50;
        TwoStageSweepOptions opt{};
    };

    class CoarseFineIndexSweep final : public ISweepIndex {
    public:
        explicit CoarseFineIndexSweep(CoarseFineIndexConfig cfg);
        const char* name() const override { return "coarse_fine"; }

        SweepIndexRunResult run(
            const CandleSeries& data,
            const SweepIndexProblem& problem,
            const ScalarObjective& objective,
            SweepProgressCallback progress_cb = nullptr) const override;

    private:
        CoarseFineIndexConfig cfg_{};
    };

    // -------------------------
    // Sweep type: Monte Carlo (index sampler)
    // -------------------------
    struct MonteCarloIndexConfig {
        std::size_t draws = 10'000;
        std::size_t top_k = 25;
        std::uint64_t seed = 0;      // 0 => random_device
        bool dedupe = true;
    };

    class MonteCarloIndexSweep final : public ISweepIndex {
    public:
        explicit MonteCarloIndexSweep(MonteCarloIndexConfig cfg);
        const char* name() const override { return "monte_carlo"; }

        SweepIndexRunResult run(
            const CandleSeries& data,
            const SweepIndexProblem& problem,
            const ScalarObjective& objective,
            SweepProgressCallback progress_cb = nullptr) const override;

    private:
        MonteCarloIndexConfig cfg_{};
    };

    // Convenience factory for executor/menu.
    enum class SweepIndexKind {
        CoarseFine = 1,
        MonteCarlo = 2,
    };

    struct SweepIndexConfig {
        SweepIndexKind kind = SweepIndexKind::CoarseFine;
        CoarseFineIndexConfig coarse_fine{};
        MonteCarloIndexConfig monte_carlo{};
    };

    std::unique_ptr<ISweepIndex> make_sweep_index(const SweepIndexConfig& cfg);

} // namespace sugar
