#include "sweep/plan_sweep.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <span>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ingest/data_view.h"
#include "sweep/metrics.h"
#include "engine/plan_compiler.h"
#include "plan_compiler_internal.h"
#include "ops/op_registry.h"
#include "op_registry_internal.h"
#include "ops/plan_spec_runtime_alias.h"
#include "ingest/run_pipeline.h"
#include "engine/strategy_executor.h"
#include "engine/program_evaluator_internal.h"
#include "sweep/sweep.h"

namespace sugar {

    namespace {
        const char* axis_kind_name(AxisKind k) {
            switch (k) {
            case AxisKind::Time: return "time";
            case AxisKind::Entity: return "entity";
            case AxisKind::Spatial: return "spatial";
            }
            return "unknown";
        }

        bool topology_family_matches(
            const std::vector<TopologyFamily>& required,
            TopologyFamily actual) {
            return required.empty() ||
                std::find(required.begin(), required.end(), actual) != required.end();
        }

        std::string join_topology_families(const std::vector<TopologyFamily>& families) {
            std::string out;
            for (std::size_t i = 0; i < families.size(); ++i) {
                if (i) out += ", ";
                out += std::string(topology_family_name(families[i]));
            }
            return out;
        }

        std::string join_topology_query_kinds(const std::vector<TopologyQueryKind>& queries) {
            std::string out;
            for (std::size_t i = 0; i < queries.size(); ++i) {
                if (i) out += ", ";
                out += std::string(topology_query_kind_name(queries[i]));
            }
            return out;
        }

        void normalize_runtime_aliases_in_place(PlanSpec& spec) {
            auto normalize_node = [](NodeSpec& node) {
                node.op = normalize_plan_spec_runtime_op(node.op);
            };

            for (auto& node : spec.numeric_nodes) normalize_node(node);
            for (auto& node : spec.bool_nodes) normalize_node(node);
        }

        bool validate_planspec_runtime_requirements_for_dataview(
            const PlanSpec& spec,
            const DataView& dv,
            const std::string& default_series,
            const ParamMap* overrides,
            std::string& err) {
            const auto registry = make_default_registry();
            const auto idx = dv.index_schema();

            auto validate_node = [&](const NodeSpec& node, const char* kind) -> bool {
                const auto* op = registry.get(node.op);
                if (!op) {
                    err = std::string("node '") + node.id + "' (" + kind + "): unknown op '" + node.op + "'";
                    return false;
                }

                for (const auto axis : op->spec.required_axes) {
                    if (!idx) {
                        err = std::string("node '") + node.id + "' (" + kind + ", op '" + node.op +
                            "'): required axis '" + axis_kind_name(axis) + "' is missing (DataView has no index_schema)";
                        return false;
                    }
                    if (!idx->has_kind(axis)) {
                        err = std::string("node '") + node.id + "' (" + kind + ", op '" + node.op +
                            "'): required axis '" + axis_kind_name(axis) + "' is missing from DataView index_schema";
                        return false;
                    }
                }

                std::string topology_name;
                if (!resolve_bound_topology_name(*op, node.params, spec.params, overrides, topology_name, err)) {
                    err = std::string("node '") + node.id + "' (" + kind + ", op '" + node.op +
                        "'): " + err;
                    return false;
                }

                const bool requires_topology =
                    !op->spec.required_topology_families.empty() ||
                    !op->spec.required_topology_query_kinds.empty() ||
                    !topology_name.empty();
                if (!requires_topology) {
                    return true;
                }

                if (topology_name.empty()) {
                    err = std::string("node '") + node.id + "' (" + kind + ", op '" + node.op +
                        "'): requires a resolvable topology binding";
                    return false;
                }

                const TopologySpec* topology = dv.topology(topology_name);
                if (!topology) {
                    err = std::string("node '") + node.id + "' (" + kind + ", op '" + node.op +
                        "'): required topology '" + topology_name + "' is not available on DataView";
                    return false;
                }
                if (!topology_family_matches(op->spec.required_topology_families, topology->family)) {
                    err = std::string("node '") + node.id + "' (" + kind + ", op '" + node.op +
                        "'): topology '" + topology_name + "' has family '" +
                        std::string(topology_family_name(topology->family)) +
                        "', expected one of {" + join_topology_families(op->spec.required_topology_families) + "}";
                    return false;
                }
                for (const auto query_kind : op->spec.required_topology_query_kinds) {
                    if (!topology->supports_query_kind(query_kind)) {
                        err = std::string("node '") + node.id + "' (" + kind + ", op '" + node.op +
                            "'): topology '" + topology_name + "' does not support query kind '" +
                            std::string(topology_query_kind_name(query_kind)) +
                            "'; required kinds are {" + join_topology_query_kinds(op->spec.required_topology_query_kinds) + "}";
                        return false;
                    }
                }

                RuntimeLocalitySubject runtime_subject{};
                if (!resolve_runtime_locality_subject(
                        *op,
                        node.params,
                        spec.params,
                        overrides,
                        dv,
                        default_series,
                        runtime_subject,
                        err)) {
                    err = std::string("node '") + node.id + "' (" + kind + ", op '" + node.op +
                        "'): locality subject could not be resolved: " + err;
                    return false;
                }

                try {
                    auto runtime_query = dv.resolve_topology(runtime_subject, topology_name);
                    if (!runtime_query) {
                        err = std::string("node '") + node.id + "' (" + kind + ", op '" + node.op +
                            "'): topology '" + topology_name + "' did not resolve to a runtime query";
                        return false;
                    }
                }
                catch (const std::exception& ex) {
                    err = std::string("node '") + node.id + "' (" + kind + ", op '" + node.op +
                        "'): topology '" + topology_name + "' could not be resolved: " + ex.what();
                    return false;
                }
                return true;
            };

            for (const auto& node : spec.numeric_nodes) {
                if (!validate_node(node, "numeric")) return false;
            }
            for (const auto& node : spec.bool_nodes) {
                if (!validate_node(node, "bool")) return false;
            }

            return true;
        }

        const char* engine_name(PlanSweepEngineKind k) {
            switch (k) {
            case PlanSweepEngineKind::CoarseFine: return "coarse_fine";
            case PlanSweepEngineKind::MonteCarlo: return "monte_carlo";
            case PlanSweepEngineKind::Exhaustive:
            default: return "exhaustive";
            }
        }

        RankingObjectiveSpec resolve_sweep_objective(const PlanSweepSpec& sw) {
            if (sw.objective_spec.has_value()) {
                return *sw.objective_spec;
            }

            RankingObjectiveSpec objective{};
            objective.primary.source = ObjectiveValueSource::ReportedMetric;
            objective.primary.name = "pnl";
            objective.direction = ObjectiveDirection::Maximize;

            if (sw.objective == "pnl") {
                return objective;
            }

            ObjectivePenaltySpec penalty{};
            penalty.value.source = ObjectiveValueSource::ReportedMetric;
            penalty.value.name = "max_drawdown";
            penalty.weight = sw.dd_weight;
            objective.penalties.push_back(std::move(penalty));
            return objective;
        }

        bool ranking_objective_needs_numeric_outputs(const RankingObjectiveSpec& objective) {
            if (objective.primary.source == ObjectiveValueSource::NumericNodeOutput) {
                return true;
            }
            for (const auto& penalty : objective.penalties) {
                if (penalty.value.source == ObjectiveValueSource::NumericNodeOutput) {
                    return true;
                }
            }
            for (const auto& tie_break : objective.tie_breaks) {
                if (tie_break.value.source == ObjectiveValueSource::NumericNodeOutput) {
                    return true;
                }
            }
            return false;
        }

        double summarize_numeric_output(
            std::span<const double> values,
            NumericNodeSummaryMode mode) {
            switch (mode) {
            case NumericNodeSummaryMode::LastFiniteValue:
                for (std::size_t i = values.size(); i > 0; --i) {
                    const double value = values[i - 1];
                    if (std::isfinite(value)) {
                        return value;
                    }
                }
                return std::numeric_limits<double>::quiet_NaN();
            }
            return std::numeric_limits<double>::quiet_NaN();
        }

        void collect_reported_values(
            const std::vector<NamedNumericOutput>& numeric_outputs,
            const ProgramEvaluationBuffers& buffers,
            BacktestResult& result_out) {
            result_out.reported_values.clear();
            result_out.reported_values.reserve(numeric_outputs.size());

            for (const auto& output : numeric_outputs) {
                if (output.output_slot >= buffers.numeric_slots.size()) {
                    continue;
                }
                const auto& slot = buffers.numeric_slots[output.output_slot];
                const double summarized = summarize_numeric_output(slot, NumericNodeSummaryMode::LastFiniteValue);
                result_out.reported_values.push_back(NamedReportedValue{ output.node_id, summarized });
            }
        }

        ParamValue make_sweep_param_value(double value, bool is_int) {
            if (is_int) {
                return static_cast<std::int64_t>(std::llround(value));
            }
            return value;
        }

        std::string param_value_to_display_string(const ParamValue& value) {
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

        std::vector<std::size_t> decode_candidate_point(
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

        std::size_t pack_candidate_point(
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

        std::size_t ceil_div(std::size_t numerator, std::size_t denominator) {
            if (denominator == 0) {
                return 0;
            }
            return (numerator + denominator - 1) / denominator;
        }

        void apply_runtime_sweep_defaults(PlanSweepSpec& sweep) {
            if (sweep.engine == PlanSweepEngineKind::MonteCarlo &&
                !sweep.monte_carlo.draws_explicit) {
                sweep.monte_carlo.draws = sweep.max_evals;
            }
        }

        std::shared_ptr<std::vector<std::size_t>> make_flat_stride_indices(
            std::size_t candidate_count,
            std::size_t stride,
            std::size_t max_indices = 0) {
            if (candidate_count == 0 || stride <= 1) {
                return {};
            }
            auto indices = std::make_shared<std::vector<std::size_t>>();
            const std::size_t theoretical_count = ceil_div(candidate_count, stride);
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

        std::shared_ptr<std::vector<std::size_t>> make_per_dimension_coarse_indices(
            const PlanSpecSweepDims& dims,
            std::size_t multiplier,
            std::size_t max_indices = 0) {
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
                    indices->push_back(pack_candidate_point(dims, point));
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

        std::size_t estimate_coarse_fine_refinement_reserve(
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

        std::size_t derive_stage2_hops_for_remaining_budget(
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
                ceil_div(remaining_budget, sweep.coarse_fine.k_stage1);
            std::size_t hops = std::max<std::size_t>(1, sweep.coarse_fine.stage2_hops);
            const std::size_t hop_limit =
                std::min<std::size_t>(std::max<std::size_t>(target_per_seed, hops), 65536);
            while (hops < hop_limit &&
                   estimate_l1_ball_count(dims.values.size(), hops) < target_per_seed) {
                ++hops;
            }
            return hops;
        }

        std::vector<SweepParameterSetting> make_candidate_parameter_values(
            const PlanSpecSweepDims& dims,
            std::size_t candidate_index) {
            std::vector<SweepParameterSetting> out;
            const auto point = decode_candidate_point(dims, candidate_index);
            out.reserve(dims.names.size());
            for (std::size_t i = 0; i < dims.names.size() && i < point.size(); ++i) {
                const auto value_index = point[i];
                if (i >= dims.values.size() || value_index >= dims.values[i].size()) {
                    continue;
                }
                SweepParameterSetting setting{};
                setting.name = dims.names[i];
                setting.value = make_sweep_param_value(dims.values[i][value_index], dims.is_int[i]);
                out.push_back(std::move(setting));
            }
            return out;
        }

        void populate_candidate_parameter_values(
            const PlanSpecSweepDims& dims,
            std::vector<SweepRankedCandidate>& candidates) {
            for (auto& candidate : candidates) {
                candidate.parameter_values = make_candidate_parameter_values(dims, candidate.candidate_index);
            }
        }

        std::vector<SweepRankedCandidate> make_sweep_ranked_candidates(
            const std::vector<IndexedCandidate>& best,
            const PlanSpecSweepDims& dims) {
            std::vector<SweepRankedCandidate> out;
            out.reserve(best.size());
            for (const auto& candidate : best) {
                SweepRankedCandidate ranked{};
                ranked.candidate_index = candidate.index;
                ranked.score = candidate.score;
                ranked.result = candidate.result;
                ranked.parameter_values = make_candidate_parameter_values(dims, candidate.index);
                out.push_back(std::move(ranked));
            }
            return out;
        }

        std::size_t multiply_clamped(std::size_t a, std::size_t b) {
            if (a == 0 || b == 0) {
                return 0;
            }
            if (a > (std::numeric_limits<std::size_t>::max() / b)) {
                return std::numeric_limits<std::size_t>::max();
            }
            return a * b;
        }

        std::size_t internal_retained_top_k(
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

        void summarize_returned_trade_hashes(
            const std::vector<IndexedCandidate>& candidates,
            SweepResultDiversitySummary& summary) {
            std::unordered_map<std::uint64_t, std::size_t> counts;
            counts.reserve(candidates.size());
            for (const auto& candidate : candidates) {
                ++counts[candidate.result.trade_hash];
            }

            summary.returned_unique_trade_hashes = counts.size();
            summary.returned_duplicate_trade_hash_groups = 0;
            summary.returned_largest_trade_hash_group = 0;
            for (const auto& kv : counts) {
                summary.returned_largest_trade_hash_group =
                    std::max(summary.returned_largest_trade_hash_group, kv.second);
                if (kv.second > 1) {
                    ++summary.returned_duplicate_trade_hash_groups;
                }
            }
        }

        std::vector<IndexedCandidate> apply_result_diversity(
            const std::vector<IndexedCandidate>& sorted_pool,
            const PlanSpecSweepDims& dims,
            const PlanSweepSpec& sweep,
            SweepResultDiversitySummary& summary) {
            summary = SweepResultDiversitySummary{};
            summary.requested = sweep.result_diversity.enabled;
            summary.dedupe_trade_hash =
                sweep.result_diversity.enabled && sweep.result_diversity.dedupe_trade_hash;
            summary.basin_aware =
                sweep.result_diversity.enabled && sweep.result_diversity.basin_aware;
            summary.oversample_multiplier =
                sweep.result_diversity.enabled
                ? std::max<std::size_t>(1, sweep.result_diversity.oversample_multiplier)
                : 1;
            summary.max_per_basin =
                summary.basin_aware
                ? std::max<std::size_t>(1, sweep.result_diversity.max_per_basin)
                : 0;
            summary.param_bucket_size =
                summary.basin_aware
                ? std::max<std::size_t>(1, sweep.result_diversity.param_bucket_size)
                : 0;
            summary.min_param_distance =
                summary.basin_aware
                ? sweep.result_diversity.min_param_distance
                : 0;
            summary.min_trades =
                sweep.result_diversity.enabled
                ? sweep.result_diversity.min_trades
                : 0;
            summary.retained_candidate_pool = sorted_pool.size();

            std::vector<IndexedCandidate> selected;
            selected.reserve(std::min<std::size_t>(sweep.top_k, sorted_pool.size()));

            if (!sweep.result_diversity.enabled) {
                for (const auto& candidate : sorted_pool) {
                    if (selected.size() >= sweep.top_k) break;
                    selected.push_back(candidate);
                }
                summarize_returned_trade_hashes(selected, summary);
                return selected;
            }

            std::unordered_set<std::uint64_t> seen_trade_hashes;
            if (summary.dedupe_trade_hash) {
                seen_trade_hashes.reserve(sweep.top_k * 4 + 1);
            }
            std::unordered_map<std::string, std::size_t> basin_counts;
            std::unordered_map<std::string, std::vector<std::vector<std::size_t>>> basin_points;

            for (const auto& candidate : sorted_pool) {
                if (selected.size() >= sweep.top_k) break;
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
                    point = decode_candidate_point(dims, candidate.index);
                    basin_key = basin_key_for_point(point, summary.param_bucket_size);
                    if (basin_counts[basin_key] >= summary.max_per_basin) {
                        ++summary.suppressed_basin;
                        continue;
                    }
                    if (summary.min_param_distance > 0) {
                        const auto it = basin_points.find(basin_key);
                        if (it != basin_points.end()) {
                            const bool too_close = std::any_of(
                                it->second.begin(),
                                it->second.end(),
                                [&](const std::vector<std::size_t>& existing) {
                                    return manhattan_distance(existing, point) < summary.min_param_distance;
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

            summary.returned_basin_count = basin_counts.size();
            summarize_returned_trade_hashes(selected, summary);
            return selected;
        }

        std::vector<SweepParameterRangeSummary> build_parameter_range_summaries(
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

        std::optional<double> param_value_as_double(const ParamValue& value) {
            return std::visit([](const auto& current) -> std::optional<double> {
                using Current = std::decay_t<decltype(current)>;
                if constexpr (std::is_same_v<Current, std::int64_t>) {
                    return static_cast<double>(current);
                }
                else if constexpr (std::is_same_v<Current, double>) {
                    return current;
                }
                else {
                    return std::nullopt;
                }
            }, value);
        }

        std::optional<double> candidate_param_value(
            const SweepRankedCandidate& candidate,
            const std::string& name) {
            const auto it = std::find_if(
                candidate.parameter_values.begin(),
                candidate.parameter_values.end(),
                [&name](const SweepParameterSetting& setting) {
                    return setting.name == name;
                });
            if (it == candidate.parameter_values.end()) {
                return std::nullopt;
            }
            return param_value_as_double(it->value);
        }

        bool approximately_equal(double lhs, double rhs) {
            const double scale = std::max({ 1.0, std::abs(lhs), std::abs(rhs) });
            return std::abs(lhs - rhs) <= (scale * 1e-9);
        }

        std::string format_sweep_warning_value(double value, bool is_int) {
            std::ostringstream out;
            if (is_int) {
                out << static_cast<std::int64_t>(std::llround(value));
            }
            else {
                out << std::setprecision(12) << value;
            }
            return out.str();
        }

        std::vector<std::string> build_sweep_report_warnings(const PlanSweepReport& report) {
            std::vector<std::string> warnings;
            if (report.top_candidates.empty()) {
                return warnings;
            }

            if (report.top_candidates.size() >= 2) {
                for (const auto& range : report.parameter_ranges) {
                    double lower = range.requested_range.min;
                    double upper = range.requested_range.max;
                    if (!range.requested_range.values.empty()) {
                        const auto [mn, mx] = std::minmax_element(
                            range.requested_range.values.begin(),
                            range.requested_range.values.end());
                        lower = *mn;
                        upper = *mx;
                    }

                    std::size_t lower_count = 0;
                    std::size_t upper_count = 0;
                    for (const auto& candidate : report.top_candidates) {
                        const auto value = candidate_param_value(candidate, range.name);
                        if (!value.has_value()) {
                            continue;
                        }
                        if (approximately_equal(*value, lower)) {
                            ++lower_count;
                        }
                        if (approximately_equal(*value, upper)) {
                            ++upper_count;
                        }
                    }

                    const auto add_boundary_warning = [&](const char* side, double value) {
                        warnings.push_back(
                            "sweep boundary concentration: all " +
                            std::to_string(report.top_candidates.size()) +
                            " returned candidates use " + range.name + "=" +
                            format_sweep_warning_value(value, range.requested_range.is_int) +
                            " at the " + side +
                            " sweep bound; consider extending that range if the objective is still improving at the edge");
                    };

                    if (lower_count == report.top_candidates.size()) {
                        add_boundary_warning("lower", lower);
                    }
                    else if (upper_count == report.top_candidates.size()) {
                        add_boundary_warning("upper", upper);
                    }
                }
            }

            const std::pair<const char*, const char*> fast_slow_pairs[] = {
                { "n_fast", "n_slow" },
                { "fast_n", "slow_n" },
                { "fast", "slow" },
            };
            for (const auto& [fast_name, slow_name] : fast_slow_pairs) {
                std::size_t inverted = 0;
                for (const auto& candidate : report.top_candidates) {
                    const auto fast = candidate_param_value(candidate, fast_name);
                    const auto slow = candidate_param_value(candidate, slow_name);
                    if (fast.has_value() && slow.has_value() && *fast > *slow) {
                        ++inverted;
                    }
                }
                if (inverted > 0) {
                    warnings.push_back(
                        "semantic fast/slow warning: " + std::to_string(inverted) +
                        "/" + std::to_string(report.top_candidates.size()) +
                        " returned candidates have " + fast_name + " > " + slow_name +
                        "; if these params represent fast/slow windows, constrain the sweep ranges or swap the naming");
                }
            }

            return warnings;
        }

        void upsert_stage_summary(
            PlanSweepReport& report,
            SweepProgressStage stage,
            std::size_t done,
            std::size_t total) {
            auto it = std::find_if(
                report.stage_summaries.begin(),
                report.stage_summaries.end(),
                [stage](const SweepStageSummary& summary) {
                    return summary.stage == stage;
                });
            if (it == report.stage_summaries.end()) {
                report.stage_summaries.push_back(SweepStageSummary{ stage, done, total });
                return;
            }
            it->done = done;
            it->total = total;
        }

        std::string trim_copy(std::string s) {
            auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
            while (!s.empty() && is_space(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
            while (!s.empty() && is_space(static_cast<unsigned char>(s.back()))) s.pop_back();
            return s;
        }

        bool series_alias_and_col(const std::string& token,
            const std::string& default_series,
            std::string& out_series,
            std::string& out_col) {
            const auto p = token.find(':');
            if (p == std::string::npos) {
                out_series = default_series;
                out_col = trim_copy(token);
                return !out_series.empty() && !out_col.empty();
            }
            out_series = trim_copy(token.substr(0, p));
            out_col = trim_copy(token.substr(p + 1));
            return !out_series.empty() && !out_col.empty();
        }

        bool resolve_col_ref_token(const std::string& token,
            const std::string& default_series,
            const ColumnRegistry& reg,
            ColumnRef& out,
            std::string& err) {
            std::string s;
            std::string c;
            if (!series_alias_and_col(token, default_series, s, c)) {
                err = "Invalid column ref token: '" + token + "'";
                return false;
            }

            ColumnId id{};
            if (!reg.find(c, id)) {
                err = "Unknown column '" + c + "' in token '" + token + "'";
                return false;
            }

            out.series = std::move(s);
            out.col = id;
            return true;
        }

        std::optional<ParamValue> find_param_value(const ParamMap& plan_params,
            const ParamMap* overrides,
            const std::string& name) {
            if (overrides) {
                auto it = overrides->find(name);
                if (it != overrides->end()) return it->second;
            }
            auto it = plan_params.find(name);
            if (it != plan_params.end()) return it->second;
            return std::nullopt;
        }

        bool resolve_param_to_string(const ParamValue& v,
            const ParamMap& plan_params,
            const ParamMap* overrides,
            std::string& out,
            std::string& err) {
            if (std::holds_alternative<std::string>(v)) {
                out = std::get<std::string>(v);
                return true;
            }
            if (std::holds_alternative<ParamRef>(v)) {
                const std::string& ref = std::get<ParamRef>(v).name;
                auto pv = find_param_value(plan_params, overrides, ref);
                if (!pv.has_value()) {
                    // In multi-series mode, unresolved refs are allowed as direct column tokens.
                    out = ref;
                    return true;
                }
                if (std::holds_alternative<std::string>(*pv)) {
                    out = std::get<std::string>(*pv);
                    return true;
                }
                err = "Param ref '" + ref + "' did not resolve to string";
                return false;
            }
            err = "Expected string or {ref:...}";
            return false;
        }

        bool get_node_param(const NodeSpec& node,
            const std::string& key,
            ParamValue& out) {
            auto it = node.params.find(key);
            if (it == node.params.end()) return false;
            out = it->second;
            return true;
        }

        bool resolve_token_to_leaf_key(
            const std::string& token,
            const std::string& default_series,
            const DataView& dv,
            std::string& leaf_key,
            std::string& err) {
            ColumnRef cr{};
            if (!resolve_col_ref_token(token, default_series, dv.bundle().schema->columns(), cr, err)) {
                return false;
            }
            if (!dv.has_col(cr)) {
                err = "missing DataView column for token '" + token + "'";
                return false;
            }
            leaf_key = cr.series + ":" + dv.bundle().schema->columns().name_by_id(cr.col);
            return true;
        }

        bool build_compiler_compatible_spec(
            const PlanSpec& spec,
            const ParamMap* overrides,
            const DataView& dv,
            const std::string& default_series,
            PlanSpec& out_spec,
            CompileEnv& out_env,
            std::string& err) {
            out_spec = spec;
            out_env = CompileEnv{};
            auto storage = make_compile_env_storage();
            out_env.storage = storage;

            std::unordered_set<std::string> used_ids;
            used_ids.reserve(spec.numeric_nodes.size() + spec.bool_nodes.size() + 8);
            for (const auto& n : spec.numeric_nodes) used_ids.insert(n.id);
            for (const auto& n : spec.bool_nodes) used_ids.insert(n.id);

            std::unordered_map<std::string, std::string> leaf_key_to_node_id;
            std::vector<NodeSpec> leaf_nodes;
            std::size_t leaf_seq = 0;

            auto make_unique_leaf_id = [&]() {
                while (true) {
                    const std::string id = "__leaf_" + std::to_string(leaf_seq++);
                    if (used_ids.insert(id).second) return id;
                }
                };

            auto ensure_leaf_node_for_token = [&](const std::string& token, std::string& out_id) -> bool {
                std::string key;
                std::string e;
                if (!resolve_token_to_leaf_key(token, default_series, dv, key, e)) {
                    err = e;
                    return false;
                }
                if (auto it = leaf_key_to_node_id.find(key); it != leaf_key_to_node_id.end()) {
                    out_id = it->second;
                    return true;
                }

                const std::string id = make_unique_leaf_id();
                NodeSpec leaf{};
                leaf.id = id;
                leaf.op = "core.precomputed_num";
                leaf.params["name"] = key;
                leaf_nodes.push_back(std::move(leaf));
                leaf_key_to_node_id.emplace(key, id);
                out_id = id;
                return true;
                };

            std::vector<NodeSpec> out_numeric;
            out_numeric.reserve(spec.numeric_nodes.size());

            for (const auto& src : spec.numeric_nodes) {
                NodeSpec node = src;
                if (node.op == "core.ref") {
                    ParamValue pv{};
                    if (!get_node_param(src, "ref", pv)) {
                        err = "node '" + src.id + "': missing param 'ref'";
                        return false;
                    }
                    std::string token;
                    if (!resolve_param_to_string(pv, spec.params, overrides, token, err)) {
                        err = "node '" + src.id + "': param 'ref': " + err;
                        return false;
                    }
                    std::string key;
                    if (!resolve_token_to_leaf_key(token, default_series, dv, key, err)) {
                        err = "node '" + src.id + "': " + err;
                        return false;
                    }
                    node.op = "core.precomputed_num";
                    node.args.clear();
                    node.params.clear();
                    node.params["name"] = key;
                }
                else if ((node.op == "ms.diff" || node.op == "ms.ratio") && node.args.empty()) {
                    ParamValue pa{};
                    ParamValue pb{};
                    if (!get_node_param(src, "a", pa) || !get_node_param(src, "b", pb)) {
                        err = "node '" + src.id + "': " + node.op + " requires args or params a,b";
                        return false;
                    }
                    std::string atok;
                    std::string btok;
                    if (!resolve_param_to_string(pa, spec.params, overrides, atok, err)) {
                        err = "node '" + src.id + "': param 'a': " + err;
                        return false;
                    }
                    if (!resolve_param_to_string(pb, spec.params, overrides, btok, err)) {
                        err = "node '" + src.id + "': param 'b': " + err;
                        return false;
                    }
                    std::string aid;
                    std::string bid;
                    if (!ensure_leaf_node_for_token(atok, aid)) {
                        err = "node '" + src.id + "': a: " + err;
                        return false;
                    }
                    if (!ensure_leaf_node_for_token(btok, bid)) {
                        err = "node '" + src.id + "': b: " + err;
                        return false;
                    }
                    node.args = { aid, bid };
                    node.params.erase("a");
                    node.params.erase("b");
                }
                else if (node.op == "ms.zscore" && node.args.empty()) {
                    ParamValue pa{};
                    if (!get_node_param(src, "a", pa)) {
                        err = "node '" + src.id + "': ms.zscore requires arg or param a";
                        return false;
                    }
                    std::string atok;
                    if (!resolve_param_to_string(pa, spec.params, overrides, atok, err)) {
                        err = "node '" + src.id + "': param 'a': " + err;
                        return false;
                    }
                    std::string aid;
                    if (!ensure_leaf_node_for_token(atok, aid)) {
                        err = "node '" + src.id + "': a: " + err;
                        return false;
                    }
                    node.args = { aid };
                    node.params.erase("a");
                }

                out_numeric.push_back(std::move(node));
            }

            out_spec.numeric_nodes.clear();
            out_spec.numeric_nodes.reserve(leaf_nodes.size() + out_numeric.size());
            for (auto& n : leaf_nodes) out_spec.numeric_nodes.push_back(std::move(n));
            for (auto& n : out_numeric) out_spec.numeric_nodes.push_back(std::move(n));

            // Build compile env leaves from all resolved DataView numeric columns.
            for (std::size_t si = 0; si < dv.bundle().series_keys.size() && si < dv.bundle().frames.size(); ++si) {
                const std::string& skey = dv.bundle().series_keys[si];
                const auto& frame = dv.bundle().frames[si];
                const auto& reg = dv.bundle().schema->columns();
                for (std::size_t ci = 0; ci < reg.size(); ++ci) {
                    ColumnId id{ static_cast<std::uint32_t>(ci) };
                    if (!frame.has_col(id)) continue;

                    ColumnRef cr{ skey, id };
                    const auto col = dv.col(cr);
                    std::vector<double> values(col.begin(), col.end());
                    const std::string leaf_key = skey + ":" + reg.name_by_id(id);
                    storage->numeric_leaf_values[leaf_key] = std::move(values);
                }
            }

            return true;
        }

        struct CompiledPlanRuntimeTemplate {
            CompiledPlanTemplate plan_template{};
            std::vector<NamedNumericOutput> numeric_outputs{};
        };

        bool build_compiled_plan_runtime_template(
            const PlanSpec& spec,
            const DataView& dv,
            const std::string& default_series,
            bool collect_numeric_outputs,
            CompiledPlanRuntimeTemplate& out,
            std::string& err_out) {
            PlanSpec compile_spec{};
            CompileEnv env{};
            if (!build_compiler_compatible_spec(spec, nullptr, dv, default_series, compile_spec, env, err_out)) {
                return false;
            }

            const auto registry = make_default_registry();
            out.numeric_outputs.clear();
            if (collect_numeric_outputs) {
                if (!compile_plan_template_with_numeric_outputs(
                        compile_spec,
                        registry,
                        env,
                        out.plan_template,
                        out.numeric_outputs,
                        err_out)) {
                    return false;
                }
            }
            else {
                if (!compile_plan_template(compile_spec, registry, env, out.plan_template, err_out)) {
                    return false;
                }
            }
            return true;
        }

        bool run_instantiated_plan_once(
            const CompiledPlanRuntimeTemplate& runtime_template,
            const ParamMap* overrides,
            const DataView& dv,
            const std::string& default_series,
            const ColumnId primary_col_id,
            const PlanSweepRuntimeOptions& opt,
            BacktestResult& result_out,
            std::string& err_out) {
            CompileResult compiled{};
            if (!instantiate_compiled_plan(runtime_template.plan_template, overrides, compiled, err_out)) {
                return false;
            }

            if (opt.override_mode) {
                compiled.plan.mode = opt.custom_mode;
            }
            if (opt.override_eval_mode) {
                compiled.exec.eval_mode = opt.eval_mode;
            }

            StrategyExecutor exec;
            ColumnRef primary{ default_series, primary_col_id };
            if (!exec.run(
                dv,
                primary,
                compiled.plan,
                compiled.exec,
                result_out,
                err_out,
                opt.cancel_requested)) {
                return false;
            }

            if (!runtime_template.numeric_outputs.empty()) {
                ProgramEvaluationBuffers buffers{};
                if (!evaluate_strategy_plan_program_buffers(dv, primary, compiled.plan, buffers, err_out)) {
                    return false;
                }
                collect_reported_values(runtime_template.numeric_outputs, buffers, result_out);
            }

            return true;
        }

    } // namespace

    std::vector<std::string> derive_plan_sweep_report_warnings(
        const PlanSweepReport& report) {
        return build_sweep_report_warnings(report);
    }

    void print_plan_sweep_report(
        const PlanSweepReport& report,
        std::ostream& out)
    {
        auto format_requested_value = [](double value) {
            std::ostringstream oss;
            oss << std::setprecision(12) << value;
            return oss.str();
        };

        auto print_requested_range = [&out, &format_requested_value](const SweepParameterRangeSummary& range) {
            out << "  " << range.name << ": ";
            if (!range.requested_range.values.empty()) {
                out << "[";
                for (std::size_t i = 0; i < range.requested_range.values.size(); ++i) {
                    if (i) out << ", ";
                    if (range.requested_range.is_int) {
                        out << static_cast<std::int64_t>(std::llround(range.requested_range.values[i]));
                    }
                    else {
                        std::ostringstream value_out;
                        value_out << std::setprecision(12) << range.requested_range.values[i];
                        out << value_out.str();
                    }
                }
                out << "]";
            }
            else {
                if (range.requested_range.is_int) {
                    out << static_cast<std::int64_t>(std::llround(range.requested_range.min))
                        << " .. " << static_cast<std::int64_t>(std::llround(range.requested_range.max))
                        << " step " << static_cast<std::int64_t>(std::llround(range.requested_range.step));
                }
                else {
                    out << format_requested_value(range.requested_range.min)
                        << " .. " << format_requested_value(range.requested_range.max)
                        << " step " << format_requested_value(range.requested_range.step);
                }
            }
            out << " (" << range.expanded_count << " values)\n";
        };

        auto print_candidate = [&out, &report](const SweepRankedCandidate& candidate, std::size_t rank) {
            out << "-- Rank " << rank << " | score=" << candidate.score << " --\n";
            out << "Params:\n";
            for (const auto& setting : candidate.parameter_values) {
                out << "  " << setting.name << ": "
                    << param_value_to_display_string(setting.value) << "\n";
            }
            out << "Run report:\n";
            print_run_report_summary(
                build_run_report(candidate.result, &report.resolved_objective),
                out,
                "  ");
            out << "\n";
        };

        out << "\n=== PlanSpec Sweep Results ===\n";
        out << "Engine: " << engine_name(report.requested_sweep.engine) << "\n";
        out << "Objective: " << describe_ranking_objective(report.resolved_objective) << "\n";
        out << "Top K: " << report.requested_sweep.top_k << "\n";
        out << "Max evals: " << report.requested_sweep.max_evals << "\n";
        out << "Param dims: " << report.parameter_count << "\n";
        out << "Total combos: ";
        if (report.total_combinations_overflow) {
            out << "(overflow)\n";
        }
        else {
            out << report.total_combinations << "\n";
        }
        out << "Candidate space: " << report.candidate_space_size;
        if (report.candidate_space_truncated) {
            out << " (truncated)";
        }
        out << "\n";
        out << "Evals run: " << report.evaluations_run << "\n";
        out << "Execution failures: " << report.execution_failures << "\n";
        out << "Compile failures: " << report.compile_failures << "\n";
        if (report.canceled) {
            out << "Canceled: true\n";
        }
        print_run_provenance_summary(report.run_provenance, out);
        out << "Candidate outcomes:\n"
            << "  completed: " << report.candidate_outcomes.completed << "\n"
            << "  completed_with_non_finite_metrics: " << report.candidate_outcomes.completed_with_non_finite_metrics << "\n"
            << "  failed: " << report.candidate_outcomes.failed << "\n"
            << "  canceled: " << report.candidate_outcomes.canceled << "\n"
            << "Objective rankability:\n"
            << "  not_evaluated: " << report.candidate_rankability.not_evaluated << "\n"
            << "  rankable: " << report.candidate_rankability.rankable << "\n"
            << "  missing_primary_value: " << report.candidate_rankability.missing_primary_value << "\n"
            << "  non_finite_primary_value: " << report.candidate_rankability.non_finite_primary_value << "\n"
            << "  missing_penalty_value: " << report.candidate_rankability.missing_penalty_value << "\n"
            << "  non_finite_penalty_value: " << report.candidate_rankability.non_finite_penalty_value << "\n";
        out << "Result diversity:\n"
            << "  requested: " << (report.result_diversity.requested ? "true" : "false") << "\n"
            << "  retained candidate pool: " << report.result_diversity.retained_candidate_pool << "\n"
            << "  returned unique trade hashes: " << report.result_diversity.returned_unique_trade_hashes << "\n"
            << "  returned duplicate trade-hash groups: " << report.result_diversity.returned_duplicate_trade_hash_groups << "\n"
            << "  largest returned trade-hash group: " << report.result_diversity.returned_largest_trade_hash_group << "\n";
        if (report.result_diversity.requested) {
            out << "  dedupe trade hash: " << (report.result_diversity.dedupe_trade_hash ? "true" : "false") << "\n"
                << "  basin aware: " << (report.result_diversity.basin_aware ? "true" : "false") << "\n"
                << "  oversample multiplier: " << report.result_diversity.oversample_multiplier << "\n"
                << "  max per basin: " << report.result_diversity.max_per_basin << "\n"
                << "  param bucket size: " << report.result_diversity.param_bucket_size << "\n"
                << "  min param distance: " << report.result_diversity.min_param_distance << "\n"
                << "  min trades: " << report.result_diversity.min_trades << "\n"
                << "  suppressed duplicate trade hashes: " << report.result_diversity.suppressed_duplicate_trade_hash << "\n"
                << "  suppressed basin candidates: " << report.result_diversity.suppressed_basin << "\n"
                << "  suppressed low-trade candidates: " << report.result_diversity.suppressed_min_trades << "\n"
                << "  returned basin count: " << report.result_diversity.returned_basin_count << "\n";
        }
        if (!report.warnings.empty()) {
            out << "Sweep warnings:\n";
            for (const auto& warning : report.warnings) {
                out << "  - " << warning << "\n";
            }
        }
        if (!report.last_execution_failure.empty()) {
            out << "Last execution failure: " << report.last_execution_failure << "\n";
        }
        if (!report.last_compile_error.empty()) {
            out << "Last compile error: " << report.last_compile_error << "\n";
        }

        if (!report.stage_summaries.empty()) {
            out << "Stages:\n";
            for (const auto& stage : report.stage_summaries) {
                out << "  " << sweep_progress_stage_name(stage.stage)
                    << ": " << stage.done << "/" << stage.total << "\n";
            }
        }

        if (!report.parameter_ranges.empty()) {
            out << "Sweep params:\n";
            for (const auto& range : report.parameter_ranges) {
                print_requested_range(range);
            }
        }

        switch (report.requested_sweep.engine) {
        case PlanSweepEngineKind::CoarseFine:
            out << "Engine settings:\n"
                << "  stage 1 K: " << report.requested_sweep.coarse_fine.k_stage1 << "\n"
                << "  stage 2 K: " << report.requested_sweep.coarse_fine.k_stage2 << "\n"
                << "  coarse step multiplier: " << report.requested_sweep.coarse_fine.coarse_step_multiplier << "\n"
                << "  stage 1 stride: " << report.requested_sweep.coarse_fine.stage1_stride << "\n"
                << "  suppression hops: " << report.requested_sweep.coarse_fine.suppression_hops << "\n"
                << "  stage 2 hops: " << report.requested_sweep.coarse_fine.stage2_hops << "\n"
                << "  depollute stage 1: " << (report.requested_sweep.coarse_fine.depollute_stage1 ? "true" : "false") << "\n"
                << "  dedupe stage 2: " << (report.requested_sweep.coarse_fine.dedupe_stage2 ? "true" : "false")
                << " (candidate-index dedupe, not trade-hash outcome dedupe)\n";
            break;
        case PlanSweepEngineKind::MonteCarlo:
            out << "Engine settings:\n"
                << "  draws: " << report.requested_sweep.monte_carlo.draws << "\n"
                << "  seed: " << report.requested_sweep.monte_carlo.seed << "\n"
                << "  dedupe: " << (report.requested_sweep.monte_carlo.dedupe ? "true" : "false")
                << " (parameter-index sample dedupe, not trade-hash outcome dedupe)\n"
                << "  monte carlo top K: " << report.requested_sweep.monte_carlo.top_k << "\n";
            break;
        case PlanSweepEngineKind::Exhaustive:
            break;
        }

        out << "TopK: " << report.top_candidates.size() << "\n";
        if (!report.top_candidates.empty()) {
            out << "Ranked candidate reports:\n\n";
        }
        for (std::size_t i = 0; i < report.top_candidates.size(); ++i) {
            print_candidate(report.top_candidates[i], i + 1);
        }

        if (!report.top_candidates.empty()) {
            out << "=== Best Candidate Run Report (Rank 1) ===\n";
            print_run_report_summary(
                build_run_report(
                    report.top_candidates.front().result,
                    &report.resolved_objective),
                out);
        }
    }

    bool run_custom_plan_spec(
        const DataView& dv,
        const ColumnRef& primary,
        const PlanSpec& spec,
        const PlanSweepRuntimeOptions& opt,
        std::ostream& out,
        BacktestResult* result_out,
        PlanSweepReport* sweep_report_out,
        const RunProvenanceSummary* run_provenance)
    {
        if (!dv.has_col(primary)) {
            out << "PlanSpec input error: primary series/column is missing from DataView.\n";
            return false;
        }
        if (opt.cancel_requested && opt.cancel_requested()) {
            out << "PlanSpec execution canceled.\n";
            return false;
        }
        PlanSpec normalized_spec = spec;
        normalize_runtime_aliases_in_place(normalized_spec);
        const bool has_sweep =
            normalized_spec.sweep &&
            (normalized_spec.sweep->enabled || !normalized_spec.sweep->params.empty());
        const RankingObjectiveSpec sweep_objective =
            has_sweep ? resolve_sweep_objective(*normalized_spec.sweep) : RankingObjectiveSpec{};
        const bool collect_numeric_outputs =
            has_sweep && ranking_objective_needs_numeric_outputs(sweep_objective);
        if (sweep_report_out) {
            *sweep_report_out = PlanSweepReport{};
        }
        const std::string default_series = primary.series;
        std::string runtime_validation_err;
        if (!validate_planspec_runtime_requirements_for_dataview(
            normalized_spec, dv, default_series, nullptr, runtime_validation_err)) {
            out << "PlanSpec runtime validation failed: " << runtime_validation_err << "\n";
            return false;
        }
        const ColumnId primary_col = primary.col;

        CompiledPlanRuntimeTemplate runtime_template{};
        std::string compile_err;
        if (!build_compiled_plan_runtime_template(
            normalized_spec,
            dv,
            default_series,
            collect_numeric_outputs,
            runtime_template,
            compile_err)) {
            out << "PlanSpec compile failed: " << compile_err << "\n";
            return false;
        }
        if (opt.cancel_requested && opt.cancel_requested()) {
            out << "PlanSpec execution canceled.\n";
            return false;
        }

        auto run_one = [&](const ParamMap* overrides, BacktestResult& result_out, std::string& err_out) -> bool {
            if (!validate_planspec_runtime_requirements_for_dataview(
                normalized_spec, dv, default_series, overrides, err_out)) {
                return false;
            }
            return run_instantiated_plan_once(
                runtime_template,
                overrides,
                dv,
                default_series,
                primary_col,
                opt,
                result_out,
                err_out);
        };

        if (normalized_spec.sweep && (normalized_spec.sweep->enabled || !normalized_spec.sweep->params.empty())) {
            PlanSweepSpec sw = *normalized_spec.sweep;
            apply_runtime_sweep_defaults(sw);

            PlanSpecSweepDims dims;
            std::string sweep_err;
            if (!build_planspec_sweep_dims(sw, dims, sweep_err)) {
                out << "PlanSpec sweep error: " << sweep_err << "\n";
                return false;
            }

            const auto sweep_started = std::chrono::steady_clock::now();
            const std::size_t total_combos = dims.total_combinations();
            const bool total_combinations_overflow =
                (total_combos == std::numeric_limits<std::size_t>::max());
            const std::size_t bounded_candidate_space_size =
                total_combinations_overflow ? sw.max_evals : std::min(sw.max_evals, total_combos);
            const bool monte_carlo_engine = (sw.engine == PlanSweepEngineKind::MonteCarlo);
            const bool coarse_fine_engine = (sw.engine == PlanSweepEngineKind::CoarseFine);
            if ((monte_carlo_engine || coarse_fine_engine) && total_combinations_overflow) {
                out << "PlanSpec sweep error: "
                    << (monte_carlo_engine ? "monte_carlo" : "coarse_fine")
                    << " requires an indexable full cartesian grid.\n";
                return false;
            }
            std::size_t engine_candidate_space_size = bounded_candidate_space_size;
            bool candidate_space_truncated =
                total_combinations_overflow || bounded_candidate_space_size < total_combos;
            if (monte_carlo_engine) {
                engine_candidate_space_size = total_combos;
                candidate_space_truncated = false;
            }
            else if (coarse_fine_engine) {
                engine_candidate_space_size = total_combos;
            }

            PlanSweepReport sweep_report{};
            sweep_report.requested_sweep = sw;
            sweep_report.resolved_objective = sweep_objective;
            if (run_provenance != nullptr) {
                sweep_report.run_provenance = *run_provenance;
            }
            sweep_report.parameter_ranges = build_parameter_range_summaries(sw, dims);
            sweep_report.parameter_count = dims.names.size();
            sweep_report.total_combinations_overflow = total_combinations_overflow;
            if (!sweep_report.total_combinations_overflow) {
                sweep_report.total_combinations = total_combos;
            }
            sweep_report.candidate_space_size = engine_candidate_space_size;
            sweep_report.candidate_space_truncated = candidate_space_truncated;

            std::size_t compile_failures = 0;
            std::string last_compile_error;
            std::size_t execution_failures = 0;
            std::string last_execution_failure;

            using IndexVec = std::vector<std::size_t>;

            auto eval_index = [&](std::size_t idx) -> BacktestResult {
                if (idx >= engine_candidate_space_size) {
                    BacktestResult bad{};
                    bad.pnl = -std::numeric_limits<double>::infinity();
                    bad.max_drawdown = std::numeric_limits<double>::infinity();
                    return bad;
                }

                const IndexVec p = decode_candidate_point(dims, idx);
                ParamMap overrides{};

                for (std::size_t i = 0; i < dims.names.size(); ++i) {
                    const std::string& name = dims.names[i];
                    const double v = dims.values[i][p[i]];
                    if (dims.is_int[i]) overrides[name] = static_cast<std::int64_t>(std::llround(v));
                    else overrides[name] = v;
                }

                BacktestResult r{};
                std::string err_run;
                if (!run_one(&overrides, r, err_run)) {
                    const bool canceled =
                        (opt.cancel_requested && opt.cancel_requested()) ||
                        err_run.find("canceled") != std::string::npos;
                    if (canceled) {
                        increment_run_outcome_counts(
                            sweep_report.candidate_outcomes,
                            RunOutcomeState::Canceled);
                    }
                    else {
                        ++compile_failures;
                        ++execution_failures;
                        last_compile_error = err_run;
                        last_execution_failure = err_run;
                        increment_run_outcome_counts(
                            sweep_report.candidate_outcomes,
                            RunOutcomeState::Failed);
                    }
                    increment_run_rankability_counts(
                        sweep_report.candidate_rankability,
                        RunRankabilityState::NotEvaluated);
                    r.pnl = -std::numeric_limits<double>::infinity();
                    r.max_drawdown = std::numeric_limits<double>::infinity();
                    r.trades = 0;
                    r.trade_hash = 0;
                }
                else {
                    if (!err_run.empty()) last_compile_error = err_run;
                    const RunReport report =
                        build_run_report(r, &sweep_objective, run_provenance);
                    increment_run_outcome_counts(
                        sweep_report.candidate_outcomes,
                        report.outcome);
                    increment_run_rankability_counts(
                        sweep_report.candidate_rankability,
                        report.objective.rankability);
                }
                return r;
                };

            auto score_fn = [&](const BacktestResult& r) -> double {
                return evaluate_ranking_objective(sweep_objective, r);
                };

            auto ostream_progress_cb = make_ostream_progress_cb(
                out,
                std::chrono::milliseconds(opt.ostream_progress_throttle_ms));
            auto progress_cb = [&](const SweepProgress& progress) {
                SweepProgress enriched = progress;
                populate_candidate_parameter_values(dims, enriched.current_best);
                upsert_stage_summary(
                    sweep_report,
                    enriched.stage,
                    enriched.stage_done,
                    enriched.stage_total);
                if (!enriched.current_best.empty()) {
                    sweep_report.top_candidates = enriched.current_best;
                }
                if (ostream_progress_cb) {
                    ostream_progress_cb(enriched);
                }
                if (opt.progress_cb) {
                    opt.progress_cb(enriched);
                }
            };
            std::vector<IndexedCandidate> best;
            std::size_t eval_count = 0;
            const std::size_t retained_top_k =
                internal_retained_top_k(sw, engine_candidate_space_size);

            switch (sw.engine) {
            case PlanSweepEngineKind::Exhaustive: {
                auto eval_token = [&](const SweepCandidateToken& token) -> BacktestResult {
                    return eval_index(token.id);
                    };

                DenseIndexSweepSpace space(engine_candidate_space_size);
                FunctionTrialEvaluator evaluator(eval_token);
                FunctionObjective objective(score_fn);
                ExhaustiveIndexTopKPolicy policy;
                SweepSessionConfig session_cfg{};
                session_cfg.top_k = retained_top_k;
                session_cfg.progress_cb = progress_cb;
                session_cfg.cancel_cb = opt.cancel_requested;

                auto session = run_sweep_session(
                    space,
                    evaluator,
                    objective,
                    policy,
                    session_cfg);

                best = std::move(session.best_first);
                eval_count = session.eval_count;
                upsert_stage_summary(
                    sweep_report,
                    SweepProgressStage::CandidateEvaluation,
                    session.eval_count,
                    session.total_candidates);
                break;
            }

            case PlanSweepEngineKind::CoarseFine:
            case PlanSweepEngineKind::MonteCarlo: {
                SweepIndexConfig cfg{};
                SweepIndexProblem problem{};
                problem.eval = [&](std::size_t idx) -> BacktestResult {
                    return eval_index(idx);
                    };

                if (sw.engine == PlanSweepEngineKind::CoarseFine) {
                    if (engine_candidate_space_size != total_combos) {
                        out << "PlanSpec sweep error: coarse_fine requires full cartesian grid "
                            << "(set max_evals to total combinations).\n";
                        return false;
                    }

                    const std::size_t nd = dims.values.size();
                    std::vector<std::size_t> radices;
                    radices.reserve(nd);
                    for (const auto& v : dims.values) radices.push_back(v.size());

                    auto unpack = [radices, nd](std::size_t idx, std::vector<std::size_t>& coords) {
                        coords.assign(nd, 0);
                        for (std::size_t d = 0; d < nd; ++d) {
                            const auto r = radices[d];
                            if (r == 0) continue;
                            coords[d] = idx % r;
                            idx /= r;
                        }
                        };

                    auto pack = [radices, nd](const std::vector<std::size_t>& coords) -> std::size_t {
                        std::size_t mult = 1;
                        std::size_t out_idx = 0;
                        for (std::size_t d = 0; d < nd; ++d) {
                            out_idx += coords[d] * mult;
                            mult *= radices[d];
                        }
                        return out_idx;
                        };

                    problem.neighbors = [unpack, pack, radices, nd](std::size_t idx) -> std::vector<std::size_t> {
                        std::vector<std::size_t> coords;
                        unpack(idx, coords);

                        std::vector<std::size_t> out;
                        out.reserve(1 + 2 * nd);
                        out.push_back(idx);

                        for (std::size_t d = 0; d < nd; ++d) {
                            if (coords[d] > 0) {
                                auto c = coords;
                                --c[d];
                                out.push_back(pack(c));
                            }
                            if (coords[d] + 1 < radices[d]) {
                                auto c = coords;
                                ++c[d];
                                out.push_back(pack(c));
                            }
                        }
                        return out;
                        };

                    cfg.kind = SweepIndexKind::CoarseFine;
                    cfg.coarse_fine.k_stage1 = sw.coarse_fine.k_stage1;
                    cfg.coarse_fine.k_stage2 =
                        sw.result_diversity.enabled
                        ? std::max(sw.coarse_fine.k_stage2, retained_top_k)
                        : sw.coarse_fine.k_stage2;
                    cfg.coarse_fine.opt.depollute_stage1 = sw.coarse_fine.depollute_stage1;
                    cfg.coarse_fine.opt.dedupe_stage2 = sw.coarse_fine.dedupe_stage2;
                    cfg.coarse_fine.opt.suppression_hops = sw.coarse_fine.suppression_hops;
                    cfg.coarse_fine.opt.stage2_hops = sw.coarse_fine.stage2_hops;
                    cfg.coarse_fine.opt.max_evaluations = sw.max_evals;

                    bool derive_refinement_from_budget = false;
                    if (sw.coarse_fine.coarse_step_multiplier > 1) {
                        cfg.coarse_fine.stage1_indices = make_per_dimension_coarse_indices(
                            dims,
                            sw.coarse_fine.coarse_step_multiplier,
                            sw.max_evals);
                        derive_refinement_from_budget = true;
                    }
                    else if (sw.coarse_fine.stage1_stride > 1) {
                        cfg.coarse_fine.stage1_indices = make_flat_stride_indices(
                            engine_candidate_space_size,
                            sw.coarse_fine.stage1_stride,
                            sw.max_evals);
                    }
                    else if (sw.max_evals > 0 && engine_candidate_space_size > sw.max_evals) {
                        const std::size_t refinement_reserve =
                            estimate_coarse_fine_refinement_reserve(sw, dims);
                        const std::size_t stage1_budget =
                            sw.max_evals > refinement_reserve
                            ? (sw.max_evals - refinement_reserve)
                            : sw.max_evals;
                        const std::size_t auto_stride =
                            std::max<std::size_t>(
                                2,
                                ceil_div(engine_candidate_space_size, std::max<std::size_t>(1, stage1_budget)));
                        sw.coarse_fine.stage1_stride = auto_stride;
                        sweep_report.requested_sweep = sw;
                        cfg.coarse_fine.stage1_indices = make_flat_stride_indices(
                            engine_candidate_space_size,
                            auto_stride,
                            sw.max_evals);
                        derive_refinement_from_budget = true;
                    }

                    if (derive_refinement_from_budget && cfg.coarse_fine.stage1_indices) {
                        const std::size_t derived_stage2_hops =
                            derive_stage2_hops_for_remaining_budget(
                                sw,
                                dims,
                                cfg.coarse_fine.stage1_indices->size());
                        if (derived_stage2_hops > sw.coarse_fine.stage2_hops) {
                            sw.coarse_fine.stage2_hops = derived_stage2_hops;
                            sweep_report.requested_sweep = sw;
                            cfg.coarse_fine.opt.stage2_hops = derived_stage2_hops;
                        }
                    }
                }
                else {
                    cfg.kind = SweepIndexKind::MonteCarlo;
                    cfg.monte_carlo.draws = std::min(sw.monte_carlo.draws, sw.max_evals);
                    cfg.monte_carlo.seed = sw.monte_carlo.seed;
                    cfg.monte_carlo.dedupe = sw.monte_carlo.dedupe;
                    cfg.monte_carlo.top_k =
                        (sw.monte_carlo.top_k > 0) ? sw.monte_carlo.top_k : sw.top_k;
                    if (sw.result_diversity.enabled) {
                        cfg.monte_carlo.top_k = std::max(cfg.monte_carlo.top_k, retained_top_k);
                    }
                }

                problem.param_count = engine_candidate_space_size;
                auto sweep = make_sweep_index(cfg);
                const auto run = sweep->run(problem, score_fn, progress_cb, opt.cancel_requested);
                best = run.has_two_stage ? run.two_stage.stage2_best_first : run.best_first;
                eval_count = run.eval_count;
                if (run.has_two_stage) {
                    upsert_stage_summary(
                        sweep_report,
                        SweepProgressStage::CoarseSearch,
                        run.two_stage.stage1_evaluated,
                        run.two_stage.stage1_evaluated);
                    upsert_stage_summary(
                        sweep_report,
                        SweepProgressStage::LocalRefinement,
                        run.two_stage.stage2_evaluated,
                        run.two_stage.stage2_evaluated);
                }
                else if (sw.engine == PlanSweepEngineKind::MonteCarlo) {
                    const std::size_t monte_carlo_stage_total =
                        cfg.monte_carlo.dedupe
                        ? std::min(cfg.monte_carlo.draws, problem.param_count)
                        : cfg.monte_carlo.draws;
                    upsert_stage_summary(
                        sweep_report,
                        SweepProgressStage::MonteCarloSampling,
                        run.eval_count,
                        monte_carlo_stage_total);
                }
                break;
            }
            }

            sweep_report.compile_failures = compile_failures;
            sweep_report.execution_failures = execution_failures;
            sweep_report.last_execution_failure = last_execution_failure;
            sweep_report.last_compile_error = last_compile_error;
            sweep_report.evaluations_run = eval_count;

            if (opt.cancel_requested && opt.cancel_requested()) {
                sweep_report.canceled = true;
                if (sweep_report_out) {
                    *sweep_report_out = sweep_report;
                }
                out << "PlanSpec execution canceled.\n";
                return false;
            }

            std::sort(best.begin(), best.end(),
                [&](const IndexedCandidate& a, const IndexedCandidate& b) {
                    if (a.score != b.score) return a.score > b.score;
                    if (ranking_objective_prefers_left(sweep_objective, a.result, b.result)) return true;
                    if (ranking_objective_prefers_left(sweep_objective, b.result, a.result)) return false;
                    return a.index < b.index;
                });

            best = apply_result_diversity(
                best,
                dims,
                sw,
                sweep_report.result_diversity);

            const auto after_engine = std::chrono::steady_clock::now();
            const double ranking_elapsed =
                std::chrono::duration<double>(after_engine - sweep_started).count();
            const auto ranked_candidates = make_sweep_ranked_candidates(best, dims);
            progress_cb(make_sweep_progress(
                eval_count,
                engine_candidate_space_size,
                ranking_elapsed,
                0.0,
                SweepProgressStage::Ranking,
                1,
                1,
                ranked_candidates));

            sweep_report.top_candidates = ranked_candidates;
            sweep_report.warnings = derive_plan_sweep_report_warnings(sweep_report);
            progress_cb(make_sweep_progress(
                eval_count,
                engine_candidate_space_size,
                ranking_elapsed,
                0.0,
                SweepProgressStage::ReportAssembly,
                1,
                1,
                sweep_report.top_candidates));

            if (!sweep_report.top_candidates.empty() && result_out) {
                *result_out = sweep_report.top_candidates.front().result;
            }
            if (sweep_report_out) {
                *sweep_report_out = sweep_report;
            }
            print_plan_sweep_report(sweep_report, out);
            return true;
        }

        BacktestResult r;
        std::string e;
        if (!run_one(nullptr, r, e)) {
            if (opt.cancel_requested && opt.cancel_requested()) {
                out << "PlanSpec execution canceled.\n";
            }
            else {
                out << "PlanSpec compile failed: " << e << "\n";
            }
            return false;
        }
        if (result_out) {
            *result_out = r;
        }
        print_run_report_summary(
            build_run_report(r, nullptr, run_provenance),
            out);
        return true;
    }

} // namespace sugar
