#pragma once

#include <cstddef>
#include <unordered_set>
#include <vector>

#include "sweep/plan_sweep.h"
#include "sweep/sweep.h"

namespace sugar {

    bool plan_sweep_pareto_needs_numeric_outputs(const PlanSweepSpec& sweep);

    class PlanSweepParetoObservationCollector {
    public:
        PlanSweepParetoObservationCollector() = default;
        explicit PlanSweepParetoObservationCollector(const PlanSweepSpec& sweep);

        void reset(const PlanSweepSpec& sweep);

        void observe(
            std::size_t candidate_index,
            double scalar_score,
            const BacktestResult& result);

        SweepParetoSummary build_summary(
            const PlanSpecSweepDims& dims,
            std::size_t retained_candidate_pool) const;

    private:
        PlanSweepSpec sweep_{};
        bool requested_{ false };
        std::size_t observed_candidate_count_{ 0 };
        std::size_t duplicate_observation_count_{ 0 };
        std::unordered_set<std::size_t> observed_indices_{};
        std::vector<IndexedCandidate> candidates_{};
    };

    SweepParetoSummary build_plan_sweep_pareto_summary(
        const std::vector<IndexedCandidate>& sorted_pool,
        const PlanSpecSweepDims& dims,
        const PlanSweepSpec& sweep);

} // namespace sugar
