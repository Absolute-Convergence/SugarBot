#pragma once

#include <cstddef>
#include <vector>

#include "sweep/plan_spec.h"
#include "sweep/plan_sweep.h"
#include "sweep/sweep.h"

namespace sugar {

    struct PlanSweepOutputDepollutionConfig {
        bool output_depollution_requested{ false };
        bool dedupe_trade_hash{ false };
        bool basin_aware{ false };
        std::size_t row_limit{ 0 };
        std::size_t max_per_basin{ 0 };
        std::size_t param_bucket_size{ 0 };
        std::size_t min_param_distance{ 0 };
        std::size_t min_trades{ 0 };
    };

    struct PlanSweepOutputDepollutionSummary {
        bool output_depollution_requested{ false };
        bool dedupe_trade_hash{ false };
        bool basin_aware{ false };
        std::size_t row_limit{ 0 };
        bool rows_capped{ false };
        std::size_t input_candidate_count{ 0 };
        std::size_t emitted_candidate_count{ 0 };
        std::size_t emitted_unique_trade_hashes{ 0 };
        std::size_t emitted_duplicate_trade_hash_groups{ 0 };
        std::size_t emitted_largest_trade_hash_group{ 0 };
        std::size_t emitted_basin_count{ 0 };
        std::size_t min_trades{ 0 };
        std::size_t suppressed_duplicate_trade_hash{ 0 };
        std::size_t suppressed_basin{ 0 };
        std::size_t suppressed_min_trades{ 0 };
        std::size_t suppressed_row_limit{ 0 };
    };

    PlanSweepOutputDepollutionConfig make_plan_sweep_output_depollution_config(
        const PlanSweepResultDiversityConfig& config,
        bool use_result_diversity,
        std::size_t row_limit);

    std::vector<IndexedCandidate> apply_plan_sweep_output_depollution(
        const std::vector<IndexedCandidate>& sorted_pool,
        const PlanSpecSweepDims& dims,
        const PlanSweepOutputDepollutionConfig& config,
        PlanSweepOutputDepollutionSummary& summary);

    std::size_t plan_sweep_retained_top_k(
        const PlanSweepSpec& sweep,
        std::size_t candidate_space_size);

    std::vector<IndexedCandidate> apply_plan_sweep_result_diversity(
        const std::vector<IndexedCandidate>& sorted_pool,
        const PlanSpecSweepDims& dims,
        const PlanSweepSpec& sweep,
        SweepResultDiversitySummary& summary);

} // namespace sugar
