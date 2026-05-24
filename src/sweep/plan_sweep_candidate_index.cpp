#include "plan_sweep_candidate_index.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <sstream>
#include <type_traits>
#include <utility>

namespace sugar {

    ParamValue make_plan_sweep_param_value(double value, bool is_int) {
        if (is_int) {
            return static_cast<std::int64_t>(std::llround(value));
        }
        return value;
    }

    std::string plan_sweep_param_value_to_display_string(const ParamValue& value) {
        return std::visit([](const auto& x) -> std::string {
            using X = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<X, std::nullptr_t>) {
                return "null";
            }
            else if constexpr (std::is_same_v<X, bool>) {
                return x ? "true" : "false";
            }
            else if constexpr (std::is_same_v<X, std::int64_t>) {
                return std::to_string(x);
            }
            else if constexpr (std::is_same_v<X, double>) {
                std::ostringstream oss;
                oss << x;
                return oss.str();
            }
            else if constexpr (std::is_same_v<X, std::string>) {
                return x;
            }
            else {
                return std::string("ref:") + x.name;
            }
        }, value);
    }

    std::vector<std::size_t> decode_plan_sweep_candidate_point(
        const PlanSpecSweepDims& dims,
        std::size_t candidate_index) {
        std::vector<std::size_t> point;
        point.reserve(dims.values.size());
        for (const auto& values : dims.values) {
            if (values.empty()) {
                return {};
            }
            point.push_back(candidate_index % values.size());
            candidate_index /= values.size();
        }
        return point;
    }

    std::size_t pack_plan_sweep_candidate_point(
        const PlanSpecSweepDims& dims,
        const std::vector<std::size_t>& point) {
        std::size_t multiplier = 1;
        std::size_t index = 0;
        for (std::size_t i = 0; i < point.size() && i < dims.values.size(); ++i) {
            index += point[i] * multiplier;
            multiplier *= dims.values[i].size();
        }
        return index;
    }

    std::size_t plan_sweep_ceil_div(std::size_t numerator, std::size_t denominator) {
        if (denominator == 0) {
            return 0;
        }
        return (numerator + denominator - 1) / denominator;
    }

    std::shared_ptr<std::vector<std::size_t>> make_plan_sweep_flat_stride_indices(
        std::size_t candidate_count,
        std::size_t stride,
        std::size_t max_indices) {
        if (candidate_count == 0 || stride <= 1) {
            return {};
        }
        auto indices = std::make_shared<std::vector<std::size_t>>();
        const std::size_t theoretical_count = plan_sweep_ceil_div(candidate_count, stride);
        indices->reserve(
            max_indices > 0
            ? std::min(theoretical_count, max_indices)
            : theoretical_count);
        for (std::size_t i = 0; i < candidate_count; i += stride) {
            if (max_indices > 0 && indices->size() >= max_indices) {
                break;
            }
            indices->push_back(i);
        }
        return indices;
    }

    std::shared_ptr<std::vector<std::size_t>> make_plan_sweep_per_dimension_coarse_indices(
        const PlanSpecSweepDims& dims,
        std::size_t multiplier,
        std::size_t max_indices) {
        if (dims.values.empty() || multiplier <= 1) {
            return {};
        }

        auto indices = std::make_shared<std::vector<std::size_t>>();
        std::vector<std::size_t> point(dims.values.size(), 0);

        auto visit = [&](auto&& self, std::size_t dim) -> void {
            if (max_indices > 0 && indices->size() >= max_indices) {
                return;
            }
            if (dim == dims.values.size()) {
                indices->push_back(pack_plan_sweep_candidate_point(dims, point));
                return;
            }
            const std::size_t count = dims.values[dim].size();
            for (std::size_t i = 0; i < count; i += multiplier) {
                if (max_indices > 0 && indices->size() >= max_indices) {
                    break;
                }
                point[dim] = i;
                self(self, dim + 1);
            }
        };
        visit(visit, 0);
        return indices;
    }

    std::size_t estimate_plan_sweep_coarse_fine_refinement_reserve(
        const PlanSweepSpec& sweep,
        const PlanSpecSweepDims& dims) {
        const std::size_t dimensions = dims.values.size();
        if (dimensions == 0 || sweep.coarse_fine.k_stage1 == 0) {
            return 0;
        }
        const std::size_t per_seed =
            1 + (2 * dimensions * std::max<std::size_t>(1, sweep.coarse_fine.stage2_hops));
        return sweep.coarse_fine.k_stage1 * per_seed;
    }

    namespace {
        double binomial_as_double(std::size_t n, std::size_t k) {
            if (k > n) {
                return 0.0;
            }
            k = std::min(k, n - k);
            double out = 1.0;
            for (std::size_t i = 1; i <= k; ++i) {
                out *= static_cast<double>(n - k + i);
                out /= static_cast<double>(i);
            }
            return out;
        }

        std::size_t estimate_l1_ball_count(std::size_t dimensions, std::size_t hops) {
            if (dimensions == 0) {
                return 0;
            }

            double total = 1.0;
            const std::size_t terms = std::min(dimensions, hops);
            double pow2 = 1.0;
            for (std::size_t used_dims = 1; used_dims <= terms; ++used_dims) {
                pow2 *= 2.0;
                total +=
                    binomial_as_double(dimensions, used_dims) *
                    binomial_as_double(hops, used_dims) *
                    pow2;
                if (total >= static_cast<double>(std::numeric_limits<std::size_t>::max())) {
                    return std::numeric_limits<std::size_t>::max();
                }
            }
            return static_cast<std::size_t>(std::ceil(total));
        }
    } // namespace

    std::size_t derive_plan_sweep_stage2_hops_for_remaining_budget(
        const PlanSweepSpec& sweep,
        const PlanSpecSweepDims& dims,
        std::size_t stage1_count) {
        if (sweep.max_evals <= stage1_count ||
            sweep.coarse_fine.k_stage1 == 0 ||
            dims.values.empty()) {
            return sweep.coarse_fine.stage2_hops;
        }

        const std::size_t remaining_budget = sweep.max_evals - stage1_count;
        const std::size_t target_per_seed =
            plan_sweep_ceil_div(remaining_budget, sweep.coarse_fine.k_stage1);
        std::size_t hops = std::max<std::size_t>(1, sweep.coarse_fine.stage2_hops);
        const std::size_t hop_limit =
            std::min<std::size_t>(std::max<std::size_t>(target_per_seed, hops), 65536);
        while (hops < hop_limit &&
               estimate_l1_ball_count(dims.values.size(), hops) < target_per_seed) {
            ++hops;
        }
        return hops;
    }

    std::vector<SweepParameterSetting> make_plan_sweep_candidate_parameter_values(
        const PlanSpecSweepDims& dims,
        std::size_t candidate_index) {
        std::vector<SweepParameterSetting> out;
        const auto point = decode_plan_sweep_candidate_point(dims, candidate_index);
        out.reserve(dims.names.size());
        for (std::size_t i = 0; i < dims.names.size() && i < point.size(); ++i) {
            const auto value_index = point[i];
            if (i >= dims.values.size() || value_index >= dims.values[i].size()) {
                continue;
            }
            SweepParameterSetting setting{};
            setting.name = dims.names[i];
            setting.value = make_plan_sweep_param_value(dims.values[i][value_index], dims.is_int[i]);
            out.push_back(std::move(setting));
        }
        return out;
    }

    void populate_plan_sweep_candidate_parameter_values(
        const PlanSpecSweepDims& dims,
        std::vector<SweepRankedCandidate>& candidates) {
        for (auto& candidate : candidates) {
            candidate.parameter_values =
                make_plan_sweep_candidate_parameter_values(dims, candidate.candidate_index);
        }
    }

    std::vector<SweepRankedCandidate> make_plan_sweep_ranked_candidates(
        const std::vector<IndexedCandidate>& best,
        const PlanSpecSweepDims& dims) {
        std::vector<SweepRankedCandidate> out;
        out.reserve(best.size());
        for (const auto& candidate : best) {
            SweepRankedCandidate ranked{};
            ranked.candidate_index = candidate.index;
            ranked.score = candidate.score;
            ranked.result = candidate.result;
            ranked.parameter_values = make_plan_sweep_candidate_parameter_values(dims, candidate.index);
            out.push_back(std::move(ranked));
        }
        return out;
    }

    std::vector<SweepParameterRangeSummary> build_plan_sweep_parameter_range_summaries(
        const PlanSweepSpec& sweep,
        const PlanSpecSweepDims& dims) {
        std::vector<SweepParameterRangeSummary> out;
        out.reserve(dims.names.size());
        for (std::size_t i = 0; i < dims.names.size(); ++i) {
            SweepParameterRangeSummary summary{};
            summary.name = dims.names[i];
            if (const auto it = sweep.params.find(summary.name); it != sweep.params.end()) {
                summary.requested_range = it->second;
            }
            summary.expanded_count = (i < dims.values.size()) ? dims.values[i].size() : 0;
            out.push_back(std::move(summary));
        }
        return out;
    }

} // namespace sugar
