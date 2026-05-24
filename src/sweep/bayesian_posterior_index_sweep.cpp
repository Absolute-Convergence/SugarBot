#include "sweep/sweep.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <random>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace sugar {

    namespace {
        constexpr std::uint64_t kDeterministicPosteriorSeed =
            0x9e3779b97f4a7c15ull;
        constexpr std::size_t kMaxTrainingPoints = 96;
        constexpr std::size_t kMaxAcquisitionCandidates = 4096;
        constexpr std::size_t kMaxLocalAcquisitionCandidates = 512;

        struct PosteriorObservation {
            std::size_t index{ 0 };
            std::size_t proposal_order{ 0 };
            std::vector<double> features{};
            double score{ 0.0 };
            BacktestResult result{};
        };

        struct PosteriorPrediction {
            bool available{ false };
            double mean{ 0.0 };
            double variance{ 0.0 };
        };

        struct GaussianProcessModel {
            std::vector<PosteriorObservation> training{};
            std::vector<double> lower{};
            std::vector<double> alpha{};
            std::vector<double> feature_scale{};
            double y_mean{ 0.0 };
            std::size_t feature_count{ 0 };
            bool trained{ false };
        };

        struct TrainingObservationSelection {
            std::vector<PosteriorObservation> valid{};
            std::size_t invalid_feature_count{ 0 };
        };

        std::vector<double> fallback_features(
            std::size_t idx,
            std::size_t param_count) {
            if (param_count <= 1) {
                return { 0.0 };
            }
            return {
                static_cast<double>(idx) /
                static_cast<double>(param_count - 1)
            };
        }

        std::vector<double> candidate_features(
            const SweepIndexProblem& problem,
            std::size_t idx) {
            if (problem.features) {
                auto features = problem.features(idx);
                if (!features.empty()) {
                    return features;
                }
            }
            return fallback_features(idx, problem.param_count);
        }

        std::size_t effective_top_k(
            const BayesianPosteriorIndexConfig& cfg) {
            return std::max<std::size_t>(1, cfg.posterior.top_k);
        }

        std::size_t effective_budget(
            const BayesianPosteriorIndexConfig& cfg,
            std::size_t param_count) {
            const std::size_t configured =
                cfg.max_evaluations > 0 ? cfg.max_evaluations : param_count;
            return std::min(configured, param_count);
        }

        std::size_t effective_initial_design_size(
            const BayesianPosteriorIndexConfig& cfg,
            std::size_t param_count,
            std::size_t budget,
            std::size_t feature_count) {
            if (budget == 0 || param_count == 0) {
                return 0;
            }
            if (cfg.posterior.initial_design_size > 0) {
                return std::min({ cfg.posterior.initial_design_size, budget, param_count });
            }
            const std::size_t derived = std::max<std::size_t>(
                3,
                std::max(
                    effective_top_k(cfg) * 2,
                    (feature_count * 2) + 1));
            return std::min({ derived, budget, param_count });
        }

        std::size_t effective_iteration_count(
            const BayesianPosteriorIndexConfig& cfg,
            std::size_t budget,
            std::size_t initial_design) {
            if (budget <= initial_design) {
                return 0;
            }
            const std::size_t remaining = budget - initial_design;
            if (cfg.posterior.iterations > 0) {
                return std::min(cfg.posterior.iterations, remaining);
            }
            return remaining;
        }

        void append_unique_candidate(
            std::vector<std::size_t>& out,
            std::unordered_set<std::size_t>& seen,
            std::size_t idx,
            std::size_t param_count) {
            if (idx < param_count && seen.insert(idx).second) {
                out.push_back(idx);
            }
        }

        std::vector<std::size_t> make_initial_design(
            std::size_t param_count,
            std::size_t target,
            std::uint64_t seed) {
            std::vector<std::size_t> out;
            if (param_count == 0 || target == 0) {
                return out;
            }
            target = std::min(target, param_count);
            out.reserve(target);
            std::unordered_set<std::size_t> seen;
            seen.reserve(target * 2 + 1);

            std::mt19937_64 rng(seed);
            const std::size_t offset =
                param_count > 0
                ? static_cast<std::size_t>(rng() % param_count)
                : 0;
            for (std::size_t i = 0; i < target; ++i) {
                const std::size_t idx =
                    (offset + ((i * param_count) / target)) % param_count;
                append_unique_candidate(out, seen, idx, param_count);
            }
            for (std::size_t i = 0; out.size() < target && i < param_count; ++i) {
                append_unique_candidate(
                    out,
                    seen,
                    (offset + i) % param_count,
                    param_count);
            }
            std::sort(out.begin(), out.end());
            return out;
        }

        std::vector<PosteriorObservation> select_training_observations(
            const std::vector<PosteriorObservation>& observations) {
            if (observations.size() <= kMaxTrainingPoints) {
                return observations;
            }

            std::vector<PosteriorObservation> by_score = observations;
            std::sort(
                by_score.begin(),
                by_score.end(),
                [](const PosteriorObservation& lhs,
                    const PosteriorObservation& rhs) {
                    if (lhs.score != rhs.score) {
                        return lhs.score > rhs.score;
                    }
                    return lhs.index < rhs.index;
                });

            std::vector<PosteriorObservation> out;
            out.reserve(kMaxTrainingPoints);
            std::unordered_set<std::size_t> selected;
            selected.reserve(kMaxTrainingPoints * 2 + 1);

            const std::size_t best_count = kMaxTrainingPoints / 2;
            for (const auto& observation : by_score) {
                if (out.size() >= best_count) {
                    break;
                }
                if (selected.insert(observation.index).second) {
                    out.push_back(observation);
                }
            }

            for (auto it = observations.rbegin();
                 it != observations.rend() && out.size() < kMaxTrainingPoints;
                 ++it) {
                if (selected.insert(it->index).second) {
                    out.push_back(*it);
                }
            }
            return out;
        }

        bool feature_vector_valid(const std::vector<double>& features) {
            return !features.empty() &&
                std::all_of(
                    features.begin(),
                    features.end(),
                    [](double value) {
                        return std::isfinite(value);
                    });
        }

        TrainingObservationSelection filter_training_observations(
            const std::vector<PosteriorObservation>& observations) {
            TrainingObservationSelection selection{};
            selection.valid.reserve(observations.size());

            std::size_t feature_count = 0;
            bool have_feature_count = false;
            for (const auto& observation : observations) {
                if (!feature_vector_valid(observation.features)) {
                    ++selection.invalid_feature_count;
                    continue;
                }
                if (!have_feature_count) {
                    feature_count = observation.features.size();
                    have_feature_count = true;
                }
                if (observation.features.size() != feature_count) {
                    ++selection.invalid_feature_count;
                    continue;
                }
                selection.valid.push_back(observation);
            }
            return selection;
        }

        double squared_distance(
            const std::vector<double>& lhs,
            const std::vector<double>& rhs,
            const std::vector<double>& scale) {
            const std::size_t n = std::min(lhs.size(), rhs.size());
            double sum = 0.0;
            for (std::size_t i = 0; i < n; ++i) {
                const double s =
                    i < scale.size() && scale[i] > 1e-12 ? scale[i] : 1.0;
                const double d = (lhs[i] - rhs[i]) / s;
                sum += d * d;
            }
            return sum;
        }

        double rbf_kernel(
            const std::vector<double>& lhs,
            const std::vector<double>& rhs,
            const std::vector<double>& scale) {
            return std::exp(-0.5 * squared_distance(lhs, rhs, scale));
        }

        bool cholesky_decompose(
            const std::vector<double>& matrix,
            std::size_t n,
            std::vector<double>& lower) {
            lower.assign(n * n, 0.0);
            for (std::size_t i = 0; i < n; ++i) {
                for (std::size_t j = 0; j <= i; ++j) {
                    double sum = matrix[i * n + j];
                    for (std::size_t k = 0; k < j; ++k) {
                        sum -= lower[i * n + k] * lower[j * n + k];
                    }
                    if (i == j) {
                        if (sum <= 1e-14 || !std::isfinite(sum)) {
                            return false;
                        }
                        lower[i * n + j] = std::sqrt(sum);
                    }
                    else {
                        lower[i * n + j] = sum / lower[j * n + j];
                    }
                }
            }
            return true;
        }

        std::vector<double> solve_lower(
            const std::vector<double>& lower,
            const std::vector<double>& rhs,
            std::size_t n) {
            std::vector<double> out(n, 0.0);
            for (std::size_t i = 0; i < n; ++i) {
                double sum = rhs[i];
                for (std::size_t k = 0; k < i; ++k) {
                    sum -= lower[i * n + k] * out[k];
                }
                out[i] = sum / lower[i * n + i];
            }
            return out;
        }

        std::vector<double> solve_upper_from_lower_transpose(
            const std::vector<double>& lower,
            const std::vector<double>& rhs,
            std::size_t n) {
            std::vector<double> out(n, 0.0);
            for (std::size_t rev = 0; rev < n; ++rev) {
                const std::size_t i = n - 1 - rev;
                double sum = rhs[i];
                for (std::size_t k = i + 1; k < n; ++k) {
                    sum -= lower[k * n + i] * out[k];
                }
                out[i] = sum / lower[i * n + i];
            }
            return out;
        }

        GaussianProcessModel train_gaussian_process(
            const std::vector<PosteriorObservation>& observations,
            std::size_t& invalid_feature_count) {
            GaussianProcessModel model{};
            const auto selection = filter_training_observations(observations);
            invalid_feature_count = selection.invalid_feature_count;
            model.training = select_training_observations(
                selection.valid);
            const std::size_t n = model.training.size();
            if (n == 0) {
                return model;
            }

            model.feature_count = model.training.front().features.size();
            std::vector<double> min_feature(model.feature_count, std::numeric_limits<double>::infinity());
            std::vector<double> max_feature(model.feature_count, -std::numeric_limits<double>::infinity());
            for (const auto& observation : model.training) {
                for (std::size_t i = 0;
                     i < model.feature_count && i < observation.features.size();
                     ++i) {
                    min_feature[i] = std::min(min_feature[i], observation.features[i]);
                    max_feature[i] = std::max(max_feature[i], observation.features[i]);
                }
                model.y_mean += observation.score;
            }
            model.y_mean /= static_cast<double>(n);

            model.feature_scale.assign(model.feature_count, 1.0);
            for (std::size_t i = 0; i < model.feature_count; ++i) {
                const double range = max_feature[i] - min_feature[i];
                if (std::isfinite(range) && range > 1e-12) {
                    model.feature_scale[i] = range;
                }
            }

            std::vector<double> matrix(n * n, 0.0);
            for (std::size_t i = 0; i < n; ++i) {
                for (std::size_t j = 0; j <= i; ++j) {
                    const double k = rbf_kernel(
                        model.training[i].features,
                        model.training[j].features,
                        model.feature_scale);
                    matrix[i * n + j] = k;
                    matrix[j * n + i] = k;
                }
                matrix[i * n + i] += 1e-8;
            }

            bool ok = cholesky_decompose(matrix, n, model.lower);
            if (!ok) {
                for (std::size_t i = 0; i < n; ++i) {
                    matrix[i * n + i] += 1e-5;
                }
                ok = cholesky_decompose(matrix, n, model.lower);
            }
            if (!ok) {
                return model;
            }

            std::vector<double> centered(n, 0.0);
            for (std::size_t i = 0; i < n; ++i) {
                centered[i] = model.training[i].score - model.y_mean;
            }
            const auto z = solve_lower(model.lower, centered, n);
            model.alpha = solve_upper_from_lower_transpose(model.lower, z, n);
            model.trained = true;
            return model;
        }

        PosteriorPrediction predict(
            const GaussianProcessModel& model,
            const std::vector<double>& features) {
            PosteriorPrediction out{};
            if (!model.trained || model.training.empty()) {
                return out;
            }

            const std::size_t n = model.training.size();
            std::vector<double> kvec(n, 0.0);
            for (std::size_t i = 0; i < n; ++i) {
                kvec[i] = rbf_kernel(
                    features,
                    model.training[i].features,
                    model.feature_scale);
            }

            out.mean = model.y_mean;
            for (std::size_t i = 0; i < n; ++i) {
                out.mean += kvec[i] * model.alpha[i];
            }
            const auto v = solve_lower(model.lower, kvec, n);
            double explained = 0.0;
            for (double x : v) {
                explained += x * x;
            }
            out.variance = std::max(0.0, 1.0 - explained);
            out.available = std::isfinite(out.mean) && std::isfinite(out.variance);
            return out;
        }

        double normal_pdf(double z) {
            constexpr double inv_sqrt_2pi = 0.39894228040143267794;
            return inv_sqrt_2pi * std::exp(-0.5 * z * z);
        }

        double normal_cdf(double z) {
            return 0.5 * std::erfc(-z / std::sqrt(2.0));
        }

        double acquisition_value(
            PlanSweepPosteriorAcquisitionKind acquisition,
            const PosteriorPrediction& prediction,
            double best_score,
            double exploration_weight) {
            if (!prediction.available) {
                return -std::numeric_limits<double>::infinity();
            }
            const double sigma = std::sqrt(std::max(0.0, prediction.variance));
            switch (acquisition) {
            case PlanSweepPosteriorAcquisitionKind::ProbabilityOfImprovement:
                if (sigma <= 1e-12) {
                    return prediction.mean > best_score ? 1.0 : 0.0;
                }
                return normal_cdf((prediction.mean - best_score) / sigma);
            case PlanSweepPosteriorAcquisitionKind::UpperConfidenceBound:
                return prediction.mean + (exploration_weight * sigma);
            case PlanSweepPosteriorAcquisitionKind::ExpectedImprovement:
            default:
                if (sigma <= 1e-12) {
                    return std::max(0.0, prediction.mean - best_score);
                }
                {
                    const double improvement = prediction.mean - best_score;
                    const double z = improvement / sigma;
                    return (improvement * normal_cdf(z)) + (sigma * normal_pdf(z));
                }
            }
        }

        std::vector<std::size_t> make_acquisition_pool(
            const SweepIndexProblem& problem,
            const std::vector<PosteriorObservation>& observations,
            const std::unordered_set<std::size_t>& evaluated,
            std::uint64_t seed,
            bool& used_local_neighbors) {
            std::vector<std::size_t> out;
            const std::size_t param_count = problem.param_count;
            if (evaluated.size() >= param_count) {
                return out;
            }
            const std::size_t remaining = param_count - evaluated.size();
            const std::size_t target =
                std::min<std::size_t>(remaining, kMaxAcquisitionCandidates);
            out.reserve(target);

            if (param_count <= kMaxAcquisitionCandidates) {
                for (std::size_t idx = 0; idx < param_count; ++idx) {
                    if (evaluated.find(idx) == evaluated.end()) {
                        out.push_back(idx);
                    }
                }
                return out;
            }

            std::unordered_set<std::size_t> queued;
            queued.reserve(target * 2 + 1);

            const auto append_pool_candidate = [&](std::size_t idx) {
                if (idx < param_count &&
                    evaluated.find(idx) == evaluated.end() &&
                    queued.insert(idx).second) {
                    out.push_back(idx);
                    return true;
                }
                return false;
            };

            if (problem.neighbors && !observations.empty()) {
                std::vector<PosteriorObservation> ranked = observations;
                std::sort(
                    ranked.begin(),
                    ranked.end(),
                    [](const PosteriorObservation& lhs,
                        const PosteriorObservation& rhs) {
                        if (lhs.score != rhs.score) {
                            return lhs.score > rhs.score;
                        }
                        return lhs.index < rhs.index;
                    });

                const std::size_t local_target = std::min<std::size_t>(
                    target,
                    kMaxLocalAcquisitionCandidates);
                for (const auto& observation : ranked) {
                    if (out.size() >= local_target) {
                        break;
                    }
                    for (const auto idx : problem.neighbors(observation.index)) {
                        if (out.size() >= local_target) {
                            break;
                        }
                        used_local_neighbors =
                            append_pool_candidate(idx) || used_local_neighbors;
                    }
                }
            }

            std::mt19937_64 rng(seed);
            const std::size_t offset =
                static_cast<std::size_t>(rng() % param_count);
            for (std::size_t i = 0; out.size() < target && i < target * 4; ++i) {
                const std::size_t idx =
                    (offset + ((i * param_count) / target)) % param_count;
                append_pool_candidate(idx);
            }
            for (std::size_t i = 0; out.size() < target && i < param_count; ++i) {
                const std::size_t idx = (offset + i) % param_count;
                append_pool_candidate(idx);
            }
            return out;
        }

        std::size_t deterministic_unevaluated_candidate(
            std::size_t param_count,
            const std::unordered_set<std::size_t>& evaluated,
            std::uint64_t seed) {
            if (param_count == 0 || evaluated.size() >= param_count) {
                return param_count;
            }

            std::mt19937_64 rng(seed);
            const std::size_t offset =
                static_cast<std::size_t>(rng() % param_count);
            for (std::size_t i = 0; i < param_count; ++i) {
                const std::size_t idx = (offset + i) % param_count;
                if (evaluated.find(idx) == evaluated.end()) {
                    return idx;
                }
            }
            return param_count;
        }

        std::vector<SweepRankedCandidate> make_ranked_progress_candidates(
            std::vector<IndexedCandidate> best_first) {
            std::sort(
                best_first.begin(),
                best_first.end(),
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
    } // namespace

    BayesianPosteriorIndexSweep::BayesianPosteriorIndexSweep(
        BayesianPosteriorIndexConfig cfg)
        : cfg_(std::move(cfg)) {}

    SweepIndexRunResult BayesianPosteriorIndexSweep::run(
        const SweepIndexProblem& problem,
        const ScalarObjective& objective,
        SweepProgressCallback progress_cb,
        SweepCancellationCallback cancel_cb,
        SweepTraceCallback trace_cb) const {
        SweepIndexRunResult out;
        out.has_posterior = true;
        out.effective_seed_available = true;
        out.requested_seed_explicit = cfg_.posterior.seed != 0;
        out.requested_seed = cfg_.posterior.seed;
        out.effective_seed =
            cfg_.posterior.seed != 0
            ? cfg_.posterior.seed
            : kDeterministicPosteriorSeed;

        auto& posterior = out.posterior;
        posterior.requested = true;
        posterior.computed = false;
        posterior.status = SweepPosteriorDiagnosticsStatus::Ready;
        posterior.surrogate = cfg_.posterior.surrogate;
        posterior.acquisition = cfg_.posterior.acquisition;
        posterior.seed = out.effective_seed;
        posterior.exploration_weight = cfg_.posterior.exploration_weight;
        posterior.normalize_inputs = cfg_.posterior.normalize_inputs;
        posterior.top_k = effective_top_k(cfg_);
        posterior.diagnostics.push_back("gaussian_process_rbf_surrogate");
        posterior.diagnostics.push_back("deterministic_seeded_candidate_proposals");

        if (!problem.eval || problem.param_count == 0) {
            posterior.status = SweepPosteriorDiagnosticsStatus::Unsupported;
            posterior.unsupported_reason =
                "bayesian_posterior requires a non-empty indexable candidate space";
            return out;
        }

        const std::size_t budget = effective_budget(cfg_, problem.param_count);
        const auto probe_features = candidate_features(problem, 0);
        const std::size_t feature_count =
            std::max<std::size_t>(1, probe_features.size());
        const std::size_t initial_design =
            effective_initial_design_size(
                cfg_,
                problem.param_count,
                budget,
                feature_count);
        const std::size_t iterations =
            effective_iteration_count(cfg_, budget, initial_design);

        posterior.initial_design_size = initial_design;
        posterior.iterations = iterations;
        posterior.diagnostics.push_back(
            std::string("acquisition_candidate_pool_limit=") +
            std::to_string(kMaxAcquisitionCandidates));
        posterior.diagnostics.push_back(
            std::string("acquisition_local_neighbor_pool_limit=") +
            std::to_string(kMaxLocalAcquisitionCandidates));
        posterior.diagnostics.push_back(
            std::string("surrogate_training_point_limit=") +
            std::to_string(kMaxTrainingPoints));

        const auto t0 = std::chrono::steady_clock::now();
        std::unordered_set<std::size_t> evaluated;
        evaluated.reserve(std::min(budget, problem.param_count) * 2 + 1);
        std::vector<PosteriorObservation> finite_observations;
        finite_observations.reserve(std::min(budget, problem.param_count));
        std::vector<IndexedCandidate> all_best;
        all_best.reserve(std::min(budget, problem.param_count));

        std::size_t done = 0;
        std::size_t proposal_order = 0;
        bool used_untrained_fallback = false;
        bool used_local_neighbor_pool = false;
        bool used_acquisition_unavailable_fallback = false;
        std::size_t max_invalid_training_features = 0;

        auto current_best_score = [&]() {
            if (finite_observations.empty()) {
                return 0.0;
            }
            return std::max_element(
                finite_observations.begin(),
                finite_observations.end(),
                [](const PosteriorObservation& lhs,
                    const PosteriorObservation& rhs) {
                    return lhs.score < rhs.score;
                })->score;
        };

        auto report_progress = [&](bool force = false) {
            if (!progress_cb) {
                return;
            }
            if (!force && done % 10 != 0) {
                return;
            }
            const auto now = std::chrono::steady_clock::now();
            const double elapsed =
                std::chrono::duration<double>(now - t0).count();
            const double rate =
                elapsed > 1e-6 ? static_cast<double>(done) / elapsed : 0.0;
            const double remaining =
                budget > done ? static_cast<double>(budget - done) : 0.0;
            const double eta = rate > 1e-9 ? remaining / rate : 0.0;
            progress_cb(make_sweep_progress(
                done,
                budget,
                elapsed,
                eta,
                SweepProgressStage::AdaptiveSelection,
                done,
                budget,
                make_ranked_progress_candidates(all_best)));
        };

        auto trace_event = [&](SweepTraceEventKind kind,
            std::size_t batch,
            std::size_t idx,
            std::size_t order,
            const std::string& reason,
            bool score_present,
            double score) {
            if (!trace_cb) {
                return;
            }
            SweepTraceEvent event{};
            event.kind = kind;
            event.stage = SweepProgressStage::AdaptiveSelection;
            event.candidate_index = idx;
            event.proposal_order = order;
            event.batch_index = batch;
            event.reason = reason;
            event.score_present = score_present;
            event.score_finite = score_present && std::isfinite(score);
            event.score = score;
            trace_cb(event);
        };

        auto evaluate_candidate = [&](
            std::size_t idx,
            std::size_t batch,
            const std::string& reason,
            bool acquisition_score_present,
            double acquisition_score) {
            if (idx >= problem.param_count || done >= budget ||
                !evaluated.insert(idx).second) {
                return;
            }

            ++proposal_order;
            SweepPosteriorProposalReason proposal_reason{};
            proposal_reason.proposal_order = proposal_order;
            proposal_reason.candidate_index = idx;
            proposal_reason.acquisition_score_present =
                acquisition_score_present;
            proposal_reason.acquisition_score_finite =
                acquisition_score_present && std::isfinite(acquisition_score);
            proposal_reason.acquisition_score = acquisition_score;
            proposal_reason.reason = reason;
            posterior.proposal_reasons.push_back(proposal_reason);

            trace_event(
                SweepTraceEventKind::Proposal,
                batch,
                idx,
                proposal_order,
                reason,
                false,
                0.0);

            const BacktestResult result = problem.eval(idx);
            ++done;
            const double score = objective(result);

            trace_event(
                SweepTraceEventKind::Observation,
                batch,
                idx,
                proposal_order,
                "bayesian_posterior_observation",
                true,
                score);

            if (std::isfinite(score)) {
                const auto features = candidate_features(problem, idx);
                finite_observations.push_back(
                    PosteriorObservation{
                        idx,
                        proposal_order,
                        features,
                        score,
                        result
                    });
                all_best.push_back(IndexedCandidate{ idx, score, result });
            }
            report_progress();
        };

        for (const auto idx : make_initial_design(
                 problem.param_count,
                 initial_design,
                 out.effective_seed)) {
            if (cancel_cb && cancel_cb()) {
                break;
            }
            evaluate_candidate(
                idx,
                0,
                "bayesian_posterior_initial_design",
                false,
                0.0);
        }

        for (std::size_t iter = 0;
             iter < iterations && done < budget &&
             evaluated.size() < problem.param_count;
             ++iter) {
            if (cancel_cb && cancel_cb()) {
                break;
            }

            std::size_t invalid_features_this_iter = 0;
            const GaussianProcessModel model =
                train_gaussian_process(
                    finite_observations,
                    invalid_features_this_iter);
            max_invalid_training_features = std::max(
                max_invalid_training_features,
                invalid_features_this_iter);
            posterior.surrogate_trained =
                posterior.surrogate_trained || model.trained;
            if (!model.trained) {
                const std::size_t fallback =
                    deterministic_unevaluated_candidate(
                        problem.param_count,
                        evaluated,
                        out.effective_seed + iter + 1);
                if (fallback >= problem.param_count) {
                    break;
                }
                used_untrained_fallback = true;
                evaluate_candidate(
                    fallback,
                    iter + 1,
                    "bayesian_posterior_untrained_fallback",
                    false,
                    0.0);
                continue;
            }

            posterior.acquisition_ready = true;
            bool pool_used_local_neighbors = false;
            const auto pool = make_acquisition_pool(
                problem,
                finite_observations,
                evaluated,
                out.effective_seed + iter + 1,
                pool_used_local_neighbors);
            used_local_neighbor_pool =
                used_local_neighbor_pool || pool_used_local_neighbors;
            if (pool.empty()) {
                break;
            }

            const double best_score = current_best_score();
            bool have_choice = false;
            std::size_t best_index = 0;
            double best_acquisition =
                -std::numeric_limits<double>::infinity();
            for (const auto idx : pool) {
                const auto features = candidate_features(problem, idx);
                const auto prediction = predict(model, features);
                const double acquisition = acquisition_value(
                    cfg_.posterior.acquisition,
                    prediction,
                    best_score,
                    cfg_.posterior.exploration_weight);
                if (!std::isfinite(acquisition)) {
                    continue;
                }
                if (!have_choice ||
                    acquisition > best_acquisition ||
                    (acquisition == best_acquisition && idx < best_index)) {
                    have_choice = true;
                    best_index = idx;
                    best_acquisition = acquisition;
                }
            }
            if (!have_choice) {
                const std::size_t fallback =
                    deterministic_unevaluated_candidate(
                        problem.param_count,
                        evaluated,
                        out.effective_seed + iter + 1);
                if (fallback >= problem.param_count) {
                    break;
                }
                used_acquisition_unavailable_fallback = true;
                evaluate_candidate(
                    fallback,
                    iter + 1,
                    "bayesian_posterior_acquisition_unavailable_fallback",
                    false,
                    0.0);
                continue;
            }

            std::string reason = "bayesian_posterior_expected_improvement";
            if (cfg_.posterior.acquisition ==
                PlanSweepPosteriorAcquisitionKind::ProbabilityOfImprovement) {
                reason = "bayesian_posterior_probability_of_improvement";
            }
            else if (cfg_.posterior.acquisition ==
                PlanSweepPosteriorAcquisitionKind::UpperConfidenceBound) {
                reason = "bayesian_posterior_upper_confidence_bound";
            }
            evaluate_candidate(
                best_index,
                iter + 1,
                reason,
                true,
                best_acquisition);
        }

        if (used_untrained_fallback) {
            posterior.diagnostics.push_back(
                "untrained_surrogate_deterministic_exploration");
        }
        if (used_local_neighbor_pool) {
            posterior.diagnostics.push_back(
                "acquisition_pool_local_neighbor_enrichment");
        }
        if (used_acquisition_unavailable_fallback) {
            posterior.diagnostics.push_back(
                "acquisition_unavailable_deterministic_fallback");
        }
        const auto final_training_selection =
            filter_training_observations(finite_observations);
        max_invalid_training_features = std::max(
            max_invalid_training_features,
            final_training_selection.invalid_feature_count);
        const auto final_selected_training =
            select_training_observations(final_training_selection.valid);

        if (max_invalid_training_features > 0) {
            posterior.diagnostics.push_back(
                "filtered_invalid_training_features");
        }

        std::sort(
            all_best.begin(),
            all_best.end(),
            [](const IndexedCandidate& lhs, const IndexedCandidate& rhs) {
                if (lhs.score != rhs.score) {
                    return lhs.score > rhs.score;
                }
                return lhs.index < rhs.index;
            });
        const std::size_t top_k = effective_top_k(cfg_);
        if (all_best.size() > top_k) {
            all_best.resize(top_k);
        }

        posterior.computed = true;
        posterior.status = SweepPosteriorDiagnosticsStatus::Computed;
        posterior.training_observation_count = finite_observations.size();
        posterior.finite_observation_count = finite_observations.size();
        posterior.surrogate_training_observation_count =
            final_selected_training.size();
        posterior.invalid_training_observation_count =
            max_invalid_training_features;
        out.best_first = std::move(all_best);
        out.eval_count = done;
        report_progress(true);
        return out;
    }

} // namespace sugar
