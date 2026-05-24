#include "evaluation_report_internal.h"

#include <algorithm>
#include <ostream>
#include <vector>

namespace sugar::evaluation_report_internal {

    namespace {
        void print_size_list(
            const std::vector<std::size_t>& values,
            std::ostream& out) {
            for (std::size_t i = 0; i < values.size(); ++i) {
                if (i != 0) {
                    out << ",";
                }
                out << values[i];
            }
        }

        void print_param_list(
            const std::vector<SweepParameterSetting>& settings,
            std::ostream& out) {
            for (std::size_t i = 0; i < settings.size(); ++i) {
                if (i != 0) {
                    out << ", ";
                }
                out << settings[i].name << "="
                    << param_value_to_text(settings[i].value);
            }
        }
    } // namespace

    void print_sweep_geometry(const SweepReport& report, std::ostream& out) {
        const auto& geometry = report.search_geometry;
        out << "Search geometry:\n"
            << "  source="
            << sweep_search_geometry_source_name(geometry.source) << "\n"
            << "  retained_candidate_pool="
            << geometry.retained_candidate_pool << "\n"
            << "  basin_bucket_size=" << geometry.basin_bucket_size << "\n"
            << "  basin_count=" << geometry.basins.size() << "\n"
            << "  duplicate_trade_path_groups="
            << geometry.duplicate_trade_paths.size() << "\n"
            << "  neighborhood_radius="
            << geometry.neighborhood_radius << "\n";
        if (!geometry.basins.empty()) {
            out << "Top basin summaries";
            if (geometry.basins.size() > 20) {
                out << " (first 20)";
            }
            out << ":\n";
            const std::size_t limit =
                std::min<std::size_t>(20, geometry.basins.size());
            for (std::size_t i = 0; i < limit; ++i) {
                const auto& basin = geometry.basins[i];
                out << "  key=" << basin.key
                    << " count=" << basin.candidate_count
                    << " density=" << basin.retained_pool_density
                    << " returned=" << basin.returned_candidate_count
                    << " best_candidate_index="
                    << basin.best_candidate_index
                    << " best_rank=" << basin.best_rank
                    << " best_score=" << basin.best_score
                    << " mean_score=" << basin.mean_score
                    << " worst_score=" << basin.worst_score
                    << " score_range=" << basin.score_range
                    << " bucket_coords=";
                print_size_list(basin.bucket_coords, out);
                out << " best_params=";
                print_param_list(basin.best_parameter_values, out);
                out << "\n";
            }
        }
        if (!geometry.local_robustness.empty()) {
            out << "Local robustness around returned winners:\n";
            for (const auto& local : geometry.local_robustness) {
                out << "  rank=" << local.rank
                    << " candidate_index=" << local.candidate_index
                    << " retained_neighbors="
                    << local.retained_neighbor_count
                    << " center_score=" << local.center_score
                    << " mean_neighbor_score="
                    << local.mean_neighbor_score
                    << " score_decay_to_mean="
                    << local.score_decay_to_mean_neighbor
                    << " retained_local_best="
                    << (local.center_is_retained_local_best ? "true" : "false")
                    << "\n";
            }
        }
    }

} // namespace sugar::evaluation_report_internal
