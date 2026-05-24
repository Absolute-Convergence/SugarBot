#include "evaluation_report_internal.h"

#include <cstddef>
#include <ostream>

namespace sugar::evaluation_report_internal {

    namespace {
        void append_json_basin_summary(
            std::ostream& out,
            const SweepBasinSummary& basin) {
            out << "{";
            out << "\"key\":";
            append_json_string(out, basin.key);
            out << ",\"bucket_coords\":";
            append_json_size_array(out, basin.bucket_coords);
            out << ",\"candidate_count\":" << basin.candidate_count;
            out << ",\"retained_pool_density\":";
            append_json_number(out, basin.retained_pool_density);
            out << ",\"returned_candidate_count\":"
                << basin.returned_candidate_count;
            out << ",\"best_candidate_index\":"
                << basin.best_candidate_index;
            out << ",\"best_rank\":" << basin.best_rank;
            out << ",\"best_score\":";
            append_json_number(out, basin.best_score);
            out << ",\"mean_score\":";
            append_json_number(out, basin.mean_score);
            out << ",\"worst_score\":";
            append_json_number(out, basin.worst_score);
            out << ",\"score_range\":";
            append_json_number(out, basin.score_range);
            out << ",\"best_trade_hash\":" << basin.best_trade_hash;
            out << ",\"best_params\":";
            append_json_parameter_settings(out, basin.best_parameter_values);
            out << "}";
        }

        void append_json_duplicate_trade_path_group(
            std::ostream& out,
            const SweepDuplicateTradePathGroup& group) {
            out << "{";
            out << "\"trade_hash\":" << group.trade_hash;
            out << ",\"candidate_count\":" << group.candidate_count;
            out << ",\"returned_candidate_count\":"
                << group.returned_candidate_count;
            out << ",\"best_candidate_index\":"
                << group.best_candidate_index;
            out << ",\"best_rank\":" << group.best_rank;
            out << ",\"best_score\":";
            append_json_number(out, group.best_score);
            out << ",\"candidate_indices\":";
            append_json_size_array(out, group.candidate_indices);
            out << ",\"ranks\":";
            append_json_size_array(out, group.ranks);
            out << "}";
        }

        void append_json_local_robustness(
            std::ostream& out,
            const SweepLocalRobustnessSummary& local) {
            out << "{";
            out << "\"rank\":" << local.rank;
            out << ",\"candidate_index\":" << local.candidate_index;
            out << ",\"retained_neighbor_count\":"
                << local.retained_neighbor_count;
            out << ",\"center_score\":";
            append_json_number(out, local.center_score);
            out << ",\"best_neighbor_score\":";
            append_json_number(out, local.best_neighbor_score);
            out << ",\"mean_neighbor_score\":";
            append_json_number(out, local.mean_neighbor_score);
            out << ",\"worst_neighbor_score\":";
            append_json_number(out, local.worst_neighbor_score);
            out << ",\"score_decay_to_best_neighbor\":";
            append_json_number(out, local.score_decay_to_best_neighbor);
            out << ",\"score_decay_to_mean_neighbor\":";
            append_json_number(out, local.score_decay_to_mean_neighbor);
            out << ",\"center_is_retained_local_best\":"
                << (local.center_is_retained_local_best ? "true" : "false");
            out << "}";
        }
    }

    void append_json_sweep_geometry(
        std::ostream& out,
        const SweepSearchGeometrySummary& geometry) {
        out << "{";
        out << "\"source\":";
        append_json_string(out, sweep_search_geometry_source_name(geometry.source));
        out << ",\"retained_candidate_pool\":"
            << geometry.retained_candidate_pool;
        out << ",\"basin_bucket_size\":"
            << geometry.basin_bucket_size;
        out << ",\"neighborhood_radius\":"
            << geometry.neighborhood_radius;
        out << ",\"basins\":[";
        for (std::size_t i = 0; i < geometry.basins.size(); ++i) {
            if (i != 0) {
                out << ",";
            }
            append_json_basin_summary(out, geometry.basins[i]);
        }
        out << "],\"duplicate_trade_paths\":[";
        for (std::size_t i = 0; i < geometry.duplicate_trade_paths.size(); ++i) {
            if (i != 0) {
                out << ",";
            }
            append_json_duplicate_trade_path_group(
                out,
                geometry.duplicate_trade_paths[i]);
        }
        out << "],\"local_robustness\":[";
        for (std::size_t i = 0; i < geometry.local_robustness.size(); ++i) {
            if (i != 0) {
                out << ",";
            }
            append_json_local_robustness(
                out,
                geometry.local_robustness[i]);
        }
        out << "]}";
    }

} // namespace sugar::evaluation_report_internal
