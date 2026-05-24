#include "plan_sweep_geometry.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "plan_sweep_candidate_index.h"

namespace sugar {

    const char* sweep_search_geometry_source_name(
        SweepSearchGeometrySource source) {
        switch (source) {
        case SweepSearchGeometrySource::None:
            return "none";
        case SweepSearchGeometrySource::RetainedCandidatePool:
            return "retained_candidate_pool";
        case SweepSearchGeometrySource::ExplicitNeighborhoodProbe:
            return "explicit_neighborhood_probe";
        case SweepSearchGeometrySource::CollectorTrace:
            return "collector_trace";
        }
        return "unknown";
    }

    namespace {
        struct BasinAccumulator {
            SweepBasinSummary summary{};
            std::size_t first_seen_order{ 0 };
            double score_sum{ 0.0 };
        };

        struct TradePathAccumulator {
            SweepDuplicateTradePathGroup group{};
            std::size_t first_seen_order{ 0 };
        };

        std::size_t effective_basin_bucket_size(const PlanSweepSpec& sweep) {
            if (sweep.result_diversity.basin_aware &&
                sweep.result_diversity.param_bucket_size > 0) {
                return sweep.result_diversity.param_bucket_size;
            }
            return 1;
        }

        std::vector<std::size_t> bucket_coords_for_point(
            const std::vector<std::size_t>& point,
            std::size_t bucket_size) {
            const std::size_t bucket = std::max<std::size_t>(1, bucket_size);
            std::vector<std::size_t> coords;
            coords.reserve(point.size());
            for (const auto value : point) {
                coords.push_back(value / bucket);
            }
            return coords;
        }

        std::string basin_key_for_bucket_coords(
            const std::vector<std::size_t>& coords) {
            std::string key;
            for (std::size_t i = 0; i < coords.size(); ++i) {
                if (i != 0) {
                    key.push_back('|');
                }
                key += std::to_string(coords[i]);
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

        bool candidate_order_less(
            double left_score,
            std::size_t left_index,
            double right_score,
            std::size_t right_index) {
            if (left_score != right_score) {
                return left_score > right_score;
            }
            return left_index < right_index;
        }

        std::unordered_map<std::size_t, std::size_t> build_returned_ranks(
            const std::vector<IndexedCandidate>& returned_candidates) {
            std::unordered_map<std::size_t, std::size_t> ranks;
            ranks.reserve(returned_candidates.size());
            for (std::size_t i = 0; i < returned_candidates.size(); ++i) {
                ranks.emplace(returned_candidates[i].index, i + 1);
            }
            return ranks;
        }

        void build_basin_summaries(
            SweepSearchGeometrySummary& geometry,
            const std::vector<IndexedCandidate>& retained_pool,
            const std::vector<IndexedCandidate>& returned_candidates,
            const std::unordered_map<std::size_t, std::size_t>& returned_ranks,
            const PlanSpecSweepDims& dims) {
            std::unordered_map<std::string, BasinAccumulator> basins;
            basins.reserve(retained_pool.size());

            for (std::size_t i = 0; i < retained_pool.size(); ++i) {
                const auto& candidate = retained_pool[i];
                const auto point =
                    decode_plan_sweep_candidate_point(dims, candidate.index);
                const auto coords =
                    bucket_coords_for_point(point, geometry.basin_bucket_size);
                const std::string key = basin_key_for_bucket_coords(coords);
                auto [it, inserted] = basins.emplace(key, BasinAccumulator{});
                if (inserted) {
                    it->second.first_seen_order = i;
                    it->second.summary.key = key;
                    it->second.summary.bucket_coords = coords;
                    it->second.summary.best_candidate_index = candidate.index;
                    it->second.summary.best_score = candidate.score;
                    it->second.summary.worst_score = candidate.score;
                    it->second.summary.best_trade_hash = candidate.result.trade_hash;
                    if (const auto rank_it = returned_ranks.find(candidate.index);
                        rank_it != returned_ranks.end()) {
                        it->second.summary.best_rank = rank_it->second;
                    }
                }

                auto& accumulator = it->second;
                auto& summary = accumulator.summary;
                ++summary.candidate_count;
                accumulator.score_sum += candidate.score;
                summary.worst_score =
                    std::min(summary.worst_score, candidate.score);
                if (candidate_order_less(
                        candidate.score,
                        candidate.index,
                        summary.best_score,
                        summary.best_candidate_index)) {
                    summary.best_candidate_index = candidate.index;
                    summary.best_score = candidate.score;
                    summary.best_trade_hash = candidate.result.trade_hash;
                    summary.best_rank = 0;
                    if (const auto rank_it = returned_ranks.find(candidate.index);
                        rank_it != returned_ranks.end()) {
                        summary.best_rank = rank_it->second;
                    }
                }
            }

            for (const auto& candidate : returned_candidates) {
                const auto point =
                    decode_plan_sweep_candidate_point(dims, candidate.index);
                const auto coords =
                    bucket_coords_for_point(point, geometry.basin_bucket_size);
                const std::string key = basin_key_for_bucket_coords(coords);
                if (auto it = basins.find(key); it != basins.end()) {
                    ++it->second.summary.returned_candidate_count;
                }
            }

            geometry.basins.reserve(basins.size());
            for (auto& [_, basin] : basins) {
                if (basin.summary.candidate_count > 0) {
                    basin.summary.mean_score =
                        basin.score_sum /
                        static_cast<double>(basin.summary.candidate_count);
                    basin.summary.score_range =
                        basin.summary.best_score - basin.summary.worst_score;
                    basin.summary.best_parameter_values =
                        make_plan_sweep_candidate_parameter_values(
                            dims,
                            basin.summary.best_candidate_index);
                }
                basin.summary.retained_pool_density =
                    retained_pool.empty()
                    ? 0.0
                    : (static_cast<double>(basin.summary.candidate_count) /
                       static_cast<double>(retained_pool.size()));
                geometry.basins.push_back(std::move(basin.summary));
            }
            std::sort(
                geometry.basins.begin(),
                geometry.basins.end(),
                [](const SweepBasinSummary& left, const SweepBasinSummary& right) {
                    if (left.candidate_count != right.candidate_count) {
                        return left.candidate_count > right.candidate_count;
                    }
                    if (left.best_score != right.best_score) {
                        return left.best_score > right.best_score;
                    }
                    if (left.best_rank != right.best_rank) {
                        if (left.best_rank == 0) return false;
                        if (right.best_rank == 0) return true;
                        return left.best_rank < right.best_rank;
                    }
                    if (left.best_candidate_index != right.best_candidate_index) {
                        return left.best_candidate_index < right.best_candidate_index;
                    }
                    return left.key < right.key;
                });
        }

        void build_duplicate_trade_path_groups(
            SweepSearchGeometrySummary& geometry,
            const std::vector<IndexedCandidate>& retained_pool,
            const std::vector<IndexedCandidate>& returned_candidates,
            const std::unordered_map<std::size_t, std::size_t>& returned_ranks) {
            std::unordered_map<std::uint64_t, TradePathAccumulator> groups;
            groups.reserve(retained_pool.size());

            for (std::size_t i = 0; i < retained_pool.size(); ++i) {
                const auto& candidate = retained_pool[i];
                auto [it, inserted] =
                    groups.emplace(candidate.result.trade_hash, TradePathAccumulator{});
                if (inserted) {
                    it->second.first_seen_order = i;
                    it->second.group.trade_hash = candidate.result.trade_hash;
                    it->second.group.best_candidate_index = candidate.index;
                    it->second.group.best_score = candidate.score;
                    if (const auto rank_it = returned_ranks.find(candidate.index);
                        rank_it != returned_ranks.end()) {
                        it->second.group.best_rank = rank_it->second;
                    }
                }

                auto& group = it->second.group;
                ++group.candidate_count;
                group.candidate_indices.push_back(candidate.index);
                if (candidate_order_less(
                        candidate.score,
                        candidate.index,
                        group.best_score,
                        group.best_candidate_index)) {
                    group.best_candidate_index = candidate.index;
                    group.best_score = candidate.score;
                    group.best_rank = 0;
                    if (const auto rank_it = returned_ranks.find(candidate.index);
                        rank_it != returned_ranks.end()) {
                        group.best_rank = rank_it->second;
                    }
                }
            }

            for (const auto& candidate : returned_candidates) {
                if (auto it = groups.find(candidate.result.trade_hash); it != groups.end()) {
                    ++it->second.group.returned_candidate_count;
                    if (const auto rank_it = returned_ranks.find(candidate.index);
                        rank_it != returned_ranks.end()) {
                        it->second.group.ranks.push_back(rank_it->second);
                    }
                }
            }

            for (auto& [_, accumulator] : groups) {
                if (accumulator.group.candidate_count < 2) {
                    continue;
                }
                std::sort(
                    accumulator.group.candidate_indices.begin(),
                    accumulator.group.candidate_indices.end());
                std::sort(
                    accumulator.group.ranks.begin(),
                    accumulator.group.ranks.end());
                geometry.duplicate_trade_paths.push_back(std::move(accumulator.group));
            }

            std::sort(
                geometry.duplicate_trade_paths.begin(),
                geometry.duplicate_trade_paths.end(),
                [](const SweepDuplicateTradePathGroup& left,
                   const SweepDuplicateTradePathGroup& right) {
                    if (left.candidate_count != right.candidate_count) {
                        return left.candidate_count > right.candidate_count;
                    }
                    if (left.returned_candidate_count != right.returned_candidate_count) {
                        return left.returned_candidate_count > right.returned_candidate_count;
                    }
                    if (left.best_score != right.best_score) {
                        return left.best_score > right.best_score;
                    }
                    return left.trade_hash < right.trade_hash;
                });
        }

        void build_local_robustness(
            SweepSearchGeometrySummary& geometry,
            const std::vector<IndexedCandidate>& retained_pool,
            const std::vector<IndexedCandidate>& returned_candidates,
            const PlanSpecSweepDims& dims) {
            for (std::size_t i = 0; i < returned_candidates.size(); ++i) {
                const auto& center = returned_candidates[i];
                const auto center_point =
                    decode_plan_sweep_candidate_point(dims, center.index);

                SweepLocalRobustnessSummary summary{};
                summary.rank = i + 1;
                summary.candidate_index = center.index;
                summary.center_score = center.score;
                summary.best_neighbor_score =
                    -std::numeric_limits<double>::infinity();
                summary.worst_neighbor_score =
                    std::numeric_limits<double>::infinity();

                double score_sum = 0.0;
                for (const auto& neighbor : retained_pool) {
                    if (neighbor.index == center.index) {
                        continue;
                    }
                    const auto neighbor_point =
                        decode_plan_sweep_candidate_point(dims, neighbor.index);
                    if (manhattan_distance(center_point, neighbor_point) >
                        geometry.neighborhood_radius) {
                        continue;
                    }

                    ++summary.retained_neighbor_count;
                    score_sum += neighbor.score;
                    summary.best_neighbor_score =
                        std::max(summary.best_neighbor_score, neighbor.score);
                    summary.worst_neighbor_score =
                        std::min(summary.worst_neighbor_score, neighbor.score);
                }

                if (summary.retained_neighbor_count == 0) {
                    summary.best_neighbor_score = center.score;
                    summary.mean_neighbor_score = center.score;
                    summary.worst_neighbor_score = center.score;
                }
                else {
                    summary.mean_neighbor_score =
                        score_sum /
                        static_cast<double>(summary.retained_neighbor_count);
                }
                summary.score_decay_to_best_neighbor =
                    center.score - summary.best_neighbor_score;
                summary.score_decay_to_mean_neighbor =
                    center.score - summary.mean_neighbor_score;
                summary.center_is_retained_local_best =
                    summary.retained_neighbor_count == 0 ||
                    center.score >= summary.best_neighbor_score;
                geometry.local_robustness.push_back(summary);
            }
        }
    } // namespace

    SweepSearchGeometrySummary build_plan_sweep_search_geometry(
        const std::vector<IndexedCandidate>& retained_pool,
        const std::vector<IndexedCandidate>& returned_candidates,
        const PlanSpecSweepDims& dims,
        const PlanSweepSpec& sweep) {
        SweepSearchGeometrySummary geometry{};
        geometry.source = SweepSearchGeometrySource::RetainedCandidatePool;
        geometry.retained_candidate_pool = retained_pool.size();
        geometry.basin_bucket_size = effective_basin_bucket_size(sweep);
        geometry.neighborhood_radius = 1;
        if (retained_pool.empty() || dims.values.empty()) {
            return geometry;
        }

        const auto returned_ranks = build_returned_ranks(returned_candidates);
        build_basin_summaries(
            geometry,
            retained_pool,
            returned_candidates,
            returned_ranks,
            dims);
        build_duplicate_trade_path_groups(
            geometry,
            retained_pool,
            returned_candidates,
            returned_ranks);
        build_local_robustness(
            geometry,
            retained_pool,
            returned_candidates,
            dims);
        return geometry;
    }

} // namespace sugar
