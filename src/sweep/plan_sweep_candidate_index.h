#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "sweep/plan_sweep.h"
#include "sweep/sweep.h"

namespace sugar {

    ParamValue make_plan_sweep_param_value(double value, bool is_int);

    std::string plan_sweep_param_value_to_display_string(const ParamValue& value);

    std::vector<std::size_t> decode_plan_sweep_candidate_point(
        const PlanSpecSweepDims& dims,
        std::size_t candidate_index);

    std::size_t pack_plan_sweep_candidate_point(
        const PlanSpecSweepDims& dims,
        const std::vector<std::size_t>& point);

    std::size_t plan_sweep_ceil_div(std::size_t numerator, std::size_t denominator);

    std::shared_ptr<std::vector<std::size_t>> make_plan_sweep_flat_stride_indices(
        std::size_t candidate_count,
        std::size_t stride,
        std::size_t max_indices = 0);

    std::shared_ptr<std::vector<std::size_t>> make_plan_sweep_per_dimension_coarse_indices(
        const PlanSpecSweepDims& dims,
        std::size_t multiplier,
        std::size_t max_indices = 0);

    std::size_t estimate_plan_sweep_coarse_fine_refinement_reserve(
        const PlanSweepSpec& sweep,
        const PlanSpecSweepDims& dims);

    std::size_t derive_plan_sweep_stage2_hops_for_remaining_budget(
        const PlanSweepSpec& sweep,
        const PlanSpecSweepDims& dims,
        std::size_t stage1_count);

    std::vector<SweepParameterSetting> make_plan_sweep_candidate_parameter_values(
        const PlanSpecSweepDims& dims,
        std::size_t candidate_index);

    void populate_plan_sweep_candidate_parameter_values(
        const PlanSpecSweepDims& dims,
        std::vector<SweepRankedCandidate>& candidates);

    std::vector<SweepRankedCandidate> make_plan_sweep_ranked_candidates(
        const std::vector<IndexedCandidate>& best,
        const PlanSpecSweepDims& dims);

    std::vector<SweepParameterRangeSummary> build_plan_sweep_parameter_range_summaries(
        const PlanSweepSpec& sweep,
        const PlanSpecSweepDims& dims);

} // namespace sugar
