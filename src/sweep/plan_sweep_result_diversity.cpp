#include "plan_sweep_result_diversity.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "plan_sweep_candidate_index.h"

namespace sugar {

    namespace {
        std::size_t multiply_clamped(std::size_t a, std::size_t b) {
            if (a == 0 || b == 0) {
                return 0;
            }
            if (a > (std::numeric_limits<std::size_t>::max() / b)) {
                return std::numeric_limits<std::size_t>::max();
            }
            return a * b;
        }

        std::string basin_key_for_point(
            const std::vector<std::size_t>& point,
            std::size_t bucket_size) {
            const std::size_t bucket = std::max<std::size_t>(1, bucket_size);
            std::string key;
            for (std::size_t i = 0; i < point.size(); ++i) {
                if (i != 0) key.push_back('|');
                key += std::to_string(point[i] / bucket);
            }
            return key;
        }

        std::size_t manhattan_distance(
            const std::vector<std::size_t>& left,
            const std::vector<std::size_t>& right) {
            const std::size_t n = std::min(left.size(), right.size());
            std::size_t distance = 0;
            for (std::size_t i = 0; i < n; ++i) {
                distance += (left[i] > right[i])
                    ? (left[i] - right[i])
                    : (right[i] - left[i]);
            }
            return distance;
        }

        void summarize_emitted_trade_hashes(
            const std::vector<IndexedCandidate>& candidates,
            PlanSweepOutputDepollutionSummary& summary) {
            std::unordered_map<std::uint64_t, std::size_t> counts;
            counts.reserve(candidates.size());
            for (const auto& candidate : candidates) {
                ++counts[candidate.result.trade_hash];
            }

            summary.emitted_unique_trade_hashes = counts.size();
            summary.emitted_duplicate_trade_hash_groups = 0;
            summary.emitted_largest_trade_hash_group = 0;
            for (const auto& kv : counts) {
                summary.emitted_largest_trade_hash_group =
                    std::max(summary.emitted_largest_trade_hash_group, kv.second);
                if (kv.second > 1) {
                    ++summary.emitted_duplicate_trade_hash_groups;
                }
            }
        }
    } // namespace

    PlanSweepOutputDepollutionConfig make_plan_sweep_output_depollution_config(
        const PlanSweepResultDiversityConfig& config,
        bool use_result_diversity,
        std::size_t row_limit) {
        PlanSweepOutputDepollutionConfig out{};
        out.output_depollution_requested =
            use_result_diversity && config.enabled;
        out.dedupe_trade_hash =
            out.output_depollution_requested && config.dedupe_trade_hash;
        out.basin_aware =
            out.output_depollution_requested && config.basin_aware;
        out.row_limit = row_limit;
        out.max_per_basin =
            out.basin_aware
            ? std::max<std::size_t>(1, config.max_per_basin)
            : 0;
        out.param_bucket_size =
            out.basin_aware
            ? std::max<std::size_t>(1, config.param_bucket_size)
            : 0;
        out.min_param_distance =
            out.basin_aware ? config.min_param_distance : 0;
        out.min_trades =
            out.output_depollution_requested ? config.min_trades : 0;
        return out;
    }

    std::vector<IndexedCandidate> apply_plan_sweep_output_depollution(
        const std::vector<IndexedCandidate>& sorted_pool,
        const PlanSpecSweepDims& dims,
        const PlanSweepOutputDepollutionConfig& config,
        PlanSweepOutputDepollutionSummary& summary) {
        summary = PlanSweepOutputDepollutionSummary{};
        summary.output_depollution_requested =
            config.output_depollution_requested;
        summary.dedupe_trade_hash = config.dedupe_trade_hash;
        summary.basin_aware = config.basin_aware;
        summary.row_limit = config.row_limit;
        summary.min_trades = config.min_trades;
        summary.input_candidate_count = sorted_pool.size();

        std::vector<IndexedCandidate> selected;
        selected.reserve(
            config.row_limit == 0
            ? sorted_pool.size()
            : std::min<std::size_t>(config.row_limit, sorted_pool.size()));

        std::unordered_set<std::uint64_t> seen_trade_hashes;
        if (summary.dedupe_trade_hash) {
            const std::size_t expected_rows =
                config.row_limit == 0 ? sorted_pool.size() : config.row_limit;
            const std::size_t reserve_rows =
                multiply_clamped(expected_rows, 4);
            seen_trade_hashes.reserve(
                reserve_rows == std::numeric_limits<std::size_t>::max()
                ? reserve_rows
                : reserve_rows + 1);
        }
        std::unordered_map<std::string, std::size_t> basin_counts;
        std::unordered_map<std::string, std::vector<std::vector<std::size_t>>> basin_points;

        for (std::size_t i = 0; i < sorted_pool.size(); ++i) {
            if (config.row_limit > 0 && selected.size() >= config.row_limit) {
                summary.rows_capped = true;
                summary.suppressed_row_limit = sorted_pool.size() - i;
                break;
            }

            const auto& candidate = sorted_pool[i];
            if (summary.min_trades > 0 && candidate.result.trades < summary.min_trades) {
                ++summary.suppressed_min_trades;
                continue;
            }
            if (summary.dedupe_trade_hash &&
                seen_trade_hashes.find(candidate.result.trade_hash) != seen_trade_hashes.end()) {
                ++summary.suppressed_duplicate_trade_hash;
                continue;
            }

            std::vector<std::size_t> point;
            std::string basin_key;
            if (summary.basin_aware) {
                point = decode_plan_sweep_candidate_point(dims, candidate.index);
                basin_key = basin_key_for_point(point, config.param_bucket_size);
                if (basin_counts[basin_key] >= config.max_per_basin) {
                    ++summary.suppressed_basin;
                    continue;
                }
                if (config.min_param_distance > 0) {
                    const auto it = basin_points.find(basin_key);
                    if (it != basin_points.end()) {
                        const bool too_close = std::any_of(
                            it->second.begin(),
                            it->second.end(),
                            [&](const std::vector<std::size_t>& existing) {
                                return manhattan_distance(existing, point) < config.min_param_distance;
                            });
                        if (too_close) {
                            ++summary.suppressed_basin;
                            continue;
                        }
                    }
                }
            }

            selected.push_back(candidate);
            if (summary.dedupe_trade_hash) {
                seen_trade_hashes.insert(candidate.result.trade_hash);
            }
            if (summary.basin_aware) {
                ++basin_counts[basin_key];
                basin_points[basin_key].push_back(std::move(point));
            }
        }

        summary.emitted_candidate_count = selected.size();
        summary.emitted_basin_count = basin_counts.size();
        summarize_emitted_trade_hashes(selected, summary);
        return selected;
    }

    std::size_t plan_sweep_retained_top_k(
        const PlanSweepSpec& sweep,
        std::size_t candidate_space_size) {
        std::size_t retained = sweep.top_k;
        if (sweep.result_diversity.enabled) {
            retained = std::max<std::size_t>(
                retained,
                multiply_clamped(
                    sweep.top_k,
                    std::max<std::size_t>(1, sweep.result_diversity.oversample_multiplier)));
        }
        if (candidate_space_size > 0) {
            retained = std::min(retained, candidate_space_size);
        }
        return std::max<std::size_t>(1, retained);
    }

    std::vector<IndexedCandidate> apply_plan_sweep_result_diversity(
        const std::vector<IndexedCandidate>& sorted_pool,
        const PlanSpecSweepDims& dims,
        const PlanSweepSpec& sweep,
        SweepResultDiversitySummary& summary) {
        summary = SweepResultDiversitySummary{};
        const PlanSweepOutputDepollutionConfig output_config =
            make_plan_sweep_output_depollution_config(
                sweep.result_diversity,
                true,
                sweep.top_k);
        PlanSweepOutputDepollutionSummary output_summary{};
        std::vector<IndexedCandidate> selected =
            apply_plan_sweep_output_depollution(
                sorted_pool,
                dims,
                output_config,
                output_summary);

        summary.requested = sweep.result_diversity.enabled;
        summary.dedupe_trade_hash = output_summary.dedupe_trade_hash;
        summary.basin_aware = output_summary.basin_aware;
        summary.oversample_multiplier =
            sweep.result_diversity.enabled
            ? std::max<std::size_t>(1, sweep.result_diversity.oversample_multiplier)
            : 1;
        summary.max_per_basin =
            output_summary.basin_aware ? output_config.max_per_basin : 0;
        summary.param_bucket_size =
            output_summary.basin_aware ? output_config.param_bucket_size : 0;
        summary.min_param_distance =
            output_summary.basin_aware ? output_config.min_param_distance : 0;
        summary.min_trades = output_config.min_trades;
        summary.retained_candidate_pool = output_summary.input_candidate_count;
        summary.returned_unique_trade_hashes =
            output_summary.emitted_unique_trade_hashes;
        summary.returned_duplicate_trade_hash_groups =
            output_summary.emitted_duplicate_trade_hash_groups;
        summary.returned_largest_trade_hash_group =
            output_summary.emitted_largest_trade_hash_group;
        summary.returned_basin_count = output_summary.emitted_basin_count;
        summary.suppressed_duplicate_trade_hash =
            output_summary.suppressed_duplicate_trade_hash;
        summary.suppressed_basin = output_summary.suppressed_basin;
        summary.suppressed_min_trades = output_summary.suppressed_min_trades;
        return selected;
    }

} // namespace sugar
