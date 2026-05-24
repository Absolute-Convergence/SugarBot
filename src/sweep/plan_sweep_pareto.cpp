#include "plan_sweep_pareto.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "plan_sweep_candidate_index.h"
#include "plan_sweep_result_diversity.h"
#include "sweep/metrics.h"

namespace sugar {

    namespace {
        struct CandidateVector {
            const IndexedCandidate* candidate{ nullptr };
            std::vector<SweepParetoObjectiveValue> values{};
            std::vector<double> normalized_values{};
            bool finite{ false };
            bool dominated{ false };
            std::size_t dominates_count{ 0 };
        };

        struct ParetoOutputDepollutionSelection {
            PlanSweepOutputDepollutionConfig config{};
            SweepParetoOutputDepollutionSource source{
                SweepParetoOutputDepollutionSource::None
            };
            bool used_result_diversity{ false };
        };

        bool objective_needs_numeric_output(const ObjectiveVectorComponentSpec& objective) {
            return objective.value.source == ObjectiveValueSource::NumericNodeOutput;
        }

        double normalized_objective_value(
            double value,
            ObjectiveDirection direction) {
            return direction == ObjectiveDirection::Minimize ? -value : value;
        }

        bool dominates(
            const CandidateVector& left,
            const CandidateVector& right) {
            bool strictly_better = false;
            const std::size_t n = std::min(
                left.normalized_values.size(),
                right.normalized_values.size());
            for (std::size_t i = 0; i < n; ++i) {
                if (left.normalized_values[i] < right.normalized_values[i]) {
                    return false;
                }
                if (left.normalized_values[i] > right.normalized_values[i]) {
                    strictly_better = true;
                }
            }
            return strictly_better;
        }

        CandidateVector build_candidate_vector(
            const IndexedCandidate& candidate,
            const PlanSweepParetoConfig& config) {
            CandidateVector out{};
            out.candidate = &candidate;
            out.values.reserve(config.objectives.size());
            out.normalized_values.reserve(config.objectives.size());
            out.finite = true;

            for (const auto& objective : config.objectives) {
                SweepParetoObjectiveValue value{};
                value.present = try_lookup_reported_value(
                    candidate.result,
                    objective.value,
                    value.value);
                value.finite = value.present && std::isfinite(value.value);
                if (!value.finite) {
                    out.finite = false;
                }
                out.normalized_values.push_back(
                    value.finite
                    ? normalized_objective_value(value.value, objective.direction)
                    : -std::numeric_limits<double>::infinity());
                out.values.push_back(value);
            }

            return out;
        }

        ParetoOutputDepollutionSelection select_pareto_output_depollution(
            const PlanSweepSpec& sweep) {
            ParetoOutputDepollutionSelection out{};
            const PlanSweepResultDiversityConfig* config =
                &sweep.result_diversity;
            bool use_config = sweep.pareto.use_result_diversity;
            SweepParetoOutputDepollutionSource source =
                SweepParetoOutputDepollutionSource::SweepResultDiversity;

            if (sweep.pareto.output_depollution) {
                config = &*sweep.pareto.output_depollution;
                use_config = true;
                source = SweepParetoOutputDepollutionSource::ParetoOutputDepollution;
            }

            out.config = make_plan_sweep_output_depollution_config(
                *config,
                use_config,
                sweep.pareto.front_row_limit);
            if (out.config.output_depollution_requested) {
                out.source = source;
                out.used_result_diversity =
                    source == SweepParetoOutputDepollutionSource::SweepResultDiversity;
            }
            return out;
        }

        void copy_pareto_output_depollution_config(
            SweepParetoSummary& summary,
            const ParetoOutputDepollutionSelection& selection) {
            summary.output_depollution_requested =
                selection.config.output_depollution_requested;
            summary.output_depollution_used_result_diversity =
                selection.used_result_diversity;
            summary.output_depollution_source = selection.source;
            summary.output_depollution_dedupe_trade_hash =
                selection.config.dedupe_trade_hash;
            summary.output_depollution_basin_aware =
                selection.config.basin_aware;
            summary.output_depollution_max_per_basin =
                selection.config.max_per_basin;
            summary.output_depollution_param_bucket_size =
                selection.config.param_bucket_size;
            summary.output_depollution_min_param_distance =
                selection.config.min_param_distance;
            summary.output_depollution_min_trades =
                selection.config.min_trades;
        }

        void initialize_pareto_objective_ranges(SweepParetoSummary& summary) {
            summary.objective_ranges.clear();
            summary.objective_ranges.reserve(summary.objectives.size());
            for (const auto& objective : summary.objectives) {
                SweepParetoObjectiveRange range{};
                range.label = objective_vector_component_label(objective);
                summary.objective_ranges.push_back(std::move(range));
            }
        }

        void observe_pareto_objective_range_value(
            SweepParetoObjectiveRange& range,
            double value,
            bool emitted_front) {
            bool& has_value = emitted_front
                ? range.emitted_front_has_value
                : range.raw_non_dominated_has_value;
            double& min_value = emitted_front
                ? range.emitted_front_min
                : range.raw_non_dominated_min;
            double& max_value = emitted_front
                ? range.emitted_front_max
                : range.raw_non_dominated_max;
            double& span = emitted_front
                ? range.emitted_front_range
                : range.raw_non_dominated_range;

            if (!has_value) {
                has_value = true;
                min_value = value;
                max_value = value;
                span = 0.0;
                return;
            }

            min_value = std::min(min_value, value);
            max_value = std::max(max_value, value);
            span = max_value - min_value;
        }

        void observe_pareto_objective_ranges(
            std::vector<SweepParetoObjectiveRange>& ranges,
            const CandidateVector& vector,
            bool emitted_front) {
            const std::size_t n = std::min(ranges.size(), vector.values.size());
            for (std::size_t i = 0; i < n; ++i) {
                const auto& value = vector.values[i];
                if (!value.finite) {
                    continue;
                }
                observe_pareto_objective_range_value(
                    ranges[i],
                    value.value,
                    emitted_front);
            }
        }
    } // namespace

    bool plan_sweep_pareto_needs_numeric_outputs(const PlanSweepSpec& sweep) {
        if (!sweep.pareto.enabled) {
            return false;
        }
        return std::any_of(
            sweep.pareto.objectives.begin(),
            sweep.pareto.objectives.end(),
            objective_needs_numeric_output);
    }

    const char* sweep_pareto_summary_source_name(SweepParetoSummarySource source) {
        switch (source) {
        case SweepParetoSummarySource::None:
            return "none";
        case SweepParetoSummarySource::EvaluatedCandidateObservations:
            return "evaluated_candidate_observations";
        case SweepParetoSummarySource::RetainedCandidatePool:
            return "retained_candidate_pool";
        }
        return "unknown";
    }

    const char* sweep_pareto_output_depollution_source_name(
        SweepParetoOutputDepollutionSource source) {
        switch (source) {
        case SweepParetoOutputDepollutionSource::None:
            return "none";
        case SweepParetoOutputDepollutionSource::SweepResultDiversity:
            return "sweep_result_diversity";
        case SweepParetoOutputDepollutionSource::ParetoOutputDepollution:
            return "pareto_output_depollution";
        }
        return "unknown";
    }

    PlanSweepParetoObservationCollector::PlanSweepParetoObservationCollector(
        const PlanSweepSpec& sweep) {
        reset(sweep);
    }

    void PlanSweepParetoObservationCollector::reset(const PlanSweepSpec& sweep) {
        sweep_ = sweep;
        requested_ = sweep.pareto.enabled;
        observed_candidate_count_ = 0;
        duplicate_observation_count_ = 0;
        observed_indices_.clear();
        candidates_.clear();
    }

    void PlanSweepParetoObservationCollector::observe(
        std::size_t candidate_index,
        double scalar_score,
        const BacktestResult& result) {
        if (!requested_) {
            return;
        }

        ++observed_candidate_count_;
        const auto [_, inserted] = observed_indices_.insert(candidate_index);
        if (!inserted) {
            ++duplicate_observation_count_;
            return;
        }

        IndexedCandidate candidate{};
        candidate.index = candidate_index;
        candidate.score = scalar_score;
        candidate.result = result;
        candidates_.push_back(std::move(candidate));
    }

    SweepParetoSummary PlanSweepParetoObservationCollector::build_summary(
        const PlanSpecSweepDims& dims,
        std::size_t retained_candidate_pool) const {
        SweepParetoSummary summary{};
        summary.requested = requested_;
        summary.source = requested_
            ? SweepParetoSummarySource::EvaluatedCandidateObservations
            : SweepParetoSummarySource::None;
        summary.observed_candidate_count = observed_candidate_count_;
        summary.unique_candidate_count = candidates_.size();
        summary.duplicate_observation_count = duplicate_observation_count_;
        summary.retained_candidate_pool = retained_candidate_pool;

        if (!requested_ || sweep_.pareto.objectives.empty()) {
            summary.computed = false;
            summary.objectives.clear();
            summary.front_row_limit = sweep_.pareto.front_row_limit;
            copy_pareto_output_depollution_config(
                summary,
                select_pareto_output_depollution(sweep_));
            return summary;
        }

        summary = build_plan_sweep_pareto_summary(candidates_, dims, sweep_);
        summary.source = SweepParetoSummarySource::EvaluatedCandidateObservations;
        summary.observed_candidate_count = observed_candidate_count_;
        summary.unique_candidate_count = candidates_.size();
        summary.duplicate_observation_count = duplicate_observation_count_;
        summary.retained_candidate_pool = retained_candidate_pool;
        return summary;
    }

    SweepParetoSummary build_plan_sweep_pareto_summary(
        const std::vector<IndexedCandidate>& sorted_pool,
        const PlanSpecSweepDims& dims,
        const PlanSweepSpec& sweep) {
        SweepParetoSummary summary{};
        summary.requested = sweep.pareto.enabled;
        summary.retained_candidate_pool = sorted_pool.size();
        summary.unique_candidate_count = sorted_pool.size();
        if (!sweep.pareto.enabled || sweep.pareto.objectives.empty()) {
            return summary;
        }

        summary.computed = true;
        summary.source = SweepParetoSummarySource::RetainedCandidatePool;
        summary.objectives = sweep.pareto.objectives;
        initialize_pareto_objective_ranges(summary);
        summary.primary_sort_index = std::min(
            sweep.pareto.primary_sort_index,
            sweep.pareto.objectives.size() - 1);
        summary.front_row_limit = sweep.pareto.front_row_limit;
        const ParetoOutputDepollutionSelection output_selection =
            select_pareto_output_depollution(sweep);
        copy_pareto_output_depollution_config(summary, output_selection);

        std::vector<CandidateVector> vectors;
        vectors.reserve(sorted_pool.size());
        for (const auto& candidate : sorted_pool) {
            CandidateVector vector = build_candidate_vector(candidate, sweep.pareto);
            if (vector.finite) {
                ++summary.finite_candidate_count;
            }
            else {
                ++summary.incomplete_candidate_count;
            }
            vectors.push_back(std::move(vector));
        }

        for (std::size_t i = 0; i < vectors.size(); ++i) {
            if (!vectors[i].finite) {
                continue;
            }
            for (std::size_t j = 0; j < vectors.size(); ++j) {
                if (i == j || !vectors[j].finite) {
                    continue;
                }
                if (dominates(vectors[i], vectors[j])) {
                    ++vectors[i].dominates_count;
                    vectors[j].dominated = true;
                }
            }
        }

        std::vector<const CandidateVector*> front;
        front.reserve(vectors.size());
        for (const auto& vector : vectors) {
            if (!vector.finite) {
                continue;
            }
            if (vector.dominated) {
                ++summary.dominated_candidate_count;
                continue;
            }
            front.push_back(&vector);
        }

        summary.non_dominated_candidate_count = front.size();
        std::sort(
            front.begin(),
            front.end(),
            [&](const CandidateVector* left, const CandidateVector* right) {
                const double left_primary =
                    left->normalized_values[summary.primary_sort_index];
                const double right_primary =
                    right->normalized_values[summary.primary_sort_index];
                if (left_primary != right_primary) {
                    return left_primary > right_primary;
                }
                if (left->candidate->score != right->candidate->score) {
                    return left->candidate->score > right->candidate->score;
                }
                return left->candidate->index < right->candidate->index;
            });

        std::vector<IndexedCandidate> raw_front_pool;
        raw_front_pool.reserve(front.size());
        std::unordered_map<std::size_t, const CandidateVector*> raw_front_by_index;
        raw_front_by_index.reserve(front.size());
        for (const CandidateVector* vector : front) {
            raw_front_pool.push_back(*vector->candidate);
            raw_front_by_index.emplace(vector->candidate->index, vector);
            observe_pareto_objective_ranges(
                summary.objective_ranges,
                *vector,
                false);
        }

        PlanSweepOutputDepollutionSummary output_summary{};
        const std::vector<IndexedCandidate> emitted_front =
            apply_plan_sweep_output_depollution(
                raw_front_pool,
                dims,
                output_selection.config,
                output_summary);

        summary.output_depollution_requested =
            output_summary.output_depollution_requested;
        summary.emitted_front_row_count =
            output_summary.emitted_candidate_count;
        summary.front_rows_capped = output_summary.rows_capped;
        summary.emitted_unique_trade_hashes =
            output_summary.emitted_unique_trade_hashes;
        summary.emitted_duplicate_trade_hash_groups =
            output_summary.emitted_duplicate_trade_hash_groups;
        summary.emitted_largest_trade_hash_group =
            output_summary.emitted_largest_trade_hash_group;
        summary.emitted_basin_count =
            output_summary.emitted_basin_count;
        summary.suppressed_duplicate_trade_hash =
            output_summary.suppressed_duplicate_trade_hash;
        summary.suppressed_basin = output_summary.suppressed_basin;
        summary.suppressed_min_trades =
            output_summary.suppressed_min_trades;
        summary.suppressed_row_limit =
            output_summary.suppressed_row_limit;

        summary.front.reserve(emitted_front.size());
        for (std::size_t i = 0; i < emitted_front.size(); ++i) {
            const auto vector_it = raw_front_by_index.find(emitted_front[i].index);
            if (vector_it == raw_front_by_index.end()) {
                continue;
            }
            const CandidateVector& vector = *vector_it->second;
            observe_pareto_objective_ranges(
                summary.objective_ranges,
                vector,
                true);
            SweepParetoCandidateSummary candidate{};
            candidate.front_rank = i + 1;
            candidate.candidate_index = vector.candidate->index;
            candidate.scalar_score = vector.candidate->score;
            candidate.dominates_count = vector.dominates_count;
            candidate.objective_values = vector.values;
            candidate.parameter_values = make_plan_sweep_candidate_parameter_values(
                dims,
                vector.candidate->index);
            summary.front.push_back(std::move(candidate));
        }

        return summary;
    }

} // namespace sugar
