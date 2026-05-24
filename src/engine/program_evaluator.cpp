#include "engine/program_evaluator.h"
#include "program_evaluator_internal.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <iterator>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ops/op_kernels_math.h"
#include "op_kernels_core.h"
#include "op_kernels_ms.h"
#include "op_kernels_spatial.h"
#include "op_kernels_topology.h"
#include "op_kernels_ts.h"
#include "op_registry_internal.h"
#include "strategy_plan_internal.h"

namespace sugar {

    namespace {
        const char* debug_warnings_env() {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
            const char* raw = std::getenv("SUGARBOT_DEBUG_WARNINGS");
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
            return raw;
        }

        bool cross_relation_debug_enabled() {
            static const bool enabled = [] {
                const char* raw = debug_warnings_env();
                if (raw == nullptr) {
                    return false;
                }
                const std::string value(raw);
                return !value.empty() &&
                    value != "0" &&
                    value != "false" &&
                    value != "FALSE" &&
                    value != "off" &&
                    value != "OFF";
            }();
            return enabled;
        }

        bool is_supported_numeric_op(const std::string& op) {
            static const std::unordered_set<std::string> kOps = {
                "core.close",
                "core.const",
                "core.precomputed_num",
                "core.ref",
                "core.select",
                "spatial.identity",
                "spatial.radius_mean",
                "st.radius_mean",
                "topology.neighbor_mean",
                "topology.neighbor_min",
                "topology.neighbor_max",
                "topology.weighted_sum",
                "topology.degree",
                "ms.diff",
                "ms.ratio",
                "ms.zscore",
                "ms.correlation",
                "ts.roc",
                "ts.sma",
                "ts.ema",
                "ts.stddev",
                "ts.stddev_ema",
                "ts.zscore_sma",
                "ts.zscore_ema",
                "ts.roll_max",
                "ts.roll_min",
                "ts.donchian_upper",
                "ts.donchian_lower",
                "ts.donchian_mid",
                "ts.donchian_width",
                "ts.percentile",
                "ts.entropy",
                "ts.autocorrelation",
                "ts.hurst",
                "ts.normalize_minmax",
                "ts.atr",
                "ts.rsi",
                "ts.atr_wilder",
                "ts.macd",
                "ts.bollinger_width",
                "ts.regime_change",
                "ts.volatility_ratio",
                "ts.shift",
                "ts.delta",
                "ts.true_range",
                "ts.true_range_ohlc",
                "ts.neg",
                "ts.abs",
                "ts.log",
                "ts.exp",
                "ts.sqrt",
                "ts.pow_const",
                "ts.normalize_minmax_global",
                "ts.ratio_of_ma",
                "ts.add",
                "ts.sub",
                "ts.mul",
                "ts.div",
                "ts.min",
                "ts.max",
                "ts.clamp_const",
                "ts.clamp",
            };
            return kOps.find(op) != kOps.end();
        }

        bool is_supported_bool_op(const std::string& op) {
            static const std::unordered_set<std::string> kOps = {
                "core.bool_const",
                "core.precomputed_bool",
                "logic.not",
                "logic.and",
                "logic.or",
                "logic.nand",
                "logic.nor",
                "cmp.lt",
                "cmp.le",
                "cmp.gt",
                "cmp.ge",
                "cmp.eq",
                "cmp.ne",
                "cmp.cross_up",
                "cmp.cross_down",
            };
            return kOps.find(op) != kOps.end();
        }

        double qnan() {
            return std::numeric_limits<double>::quiet_NaN();
        }

        const OperatorRegistry& default_registry() {
            static const OperatorRegistry kRegistry = make_default_registry();
            return kRegistry;
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

        struct ProgramEvalContext {
            const DataView& view;
            const ColumnRef& primary;
            const CompileEnvStorage* env;
            std::size_t row_count;
        };

        struct ProgramInputBuffer {
            ValueType type{ ValueType::Num };
            std::span<const double> num{};
            std::span<const std::uint8_t> boo{};
        };

        bool validate_numeric_size(const NumericBuffer& v, std::size_t n, std::string& err, const std::string& label) {
            if (v.size() != n) {
                err = label + " produced size " + std::to_string(v.size()) + ", expected " + std::to_string(n);
                return false;
            }
            return true;
        }

        bool validate_bool_size(const BoolBuffer& v, std::size_t n, std::string& err, const std::string& label) {
            if (v.size() != n) {
                err = label + " produced size " + std::to_string(v.size()) + ", expected " + std::to_string(n);
                return false;
            }
            return true;
        }

        std::span<const double> primary_close(const ProgramEvalContext& ctx) {
            return ctx.view.col(ctx.primary);
        }

        bool resolve_numeric_op_topology(const ProgramEvalContext& ctx,
            const std::string& op_name,
            const ResolvedParamMap& params,
            std::unique_ptr<NeighborhoodQuery>& out,
            std::string& err) {
            const auto& registry = default_registry();
            const auto* op = registry.get(op_name);
            if (!op) {
                err = op_name + " is not registered in the runtime operator registry";
                return false;
            }

            std::string topology_name;
            if (!resolve_bound_topology_name(*op, params, topology_name, err)) {
                err = op_name + " topology binding failed: " + err;
                return false;
            }
            if (topology_name.empty()) {
                err = op_name + " requires a resolvable topology name, but none was bound";
                return false;
            }

            const TopologySpec* spec = ctx.view.topology(topology_name);
            if (!spec) {
                err = op_name + " topology '" + topology_name + "' is not available on this runtime";
                return false;
            }
            if (!topology_family_matches(op->spec.required_topology_families, spec->family)) {
                err = op_name + " topology '" + topology_name + "' has family '" +
                    std::string(topology_family_name(spec->family)) +
                    "', expected one of {" + join_topology_families(op->spec.required_topology_families) + "}";
                return false;
            }
            for (const auto query_kind : op->spec.required_topology_query_kinds) {
                if (!spec->supports_query_kind(query_kind)) {
                    err = op_name + " topology '" + topology_name + "' does not support query kind '" +
                        std::string(topology_query_kind_name(query_kind)) +
                        "'; required kinds are {" + join_topology_query_kinds(op->spec.required_topology_query_kinds) + "}";
                    return false;
                }
            }

            RuntimeLocalitySubject runtime_subject{};
            if (!resolve_runtime_locality_subject(*op, params, ctx.view, ctx.primary.series, runtime_subject, err)) {
                err = op_name + " locality subject resolution failed: " + err;
                return false;
            }

            try {
                out = ctx.view.resolve_topology(runtime_subject, topology_name);
            }
            catch (const std::exception& ex) {
                err = op_name + " topology resolution failed: " + std::string(ex.what());
                return false;
            }
            return static_cast<bool>(out);
        }

        std::string normalized_direction_string(std::string value) {
            value.erase(std::remove_if(value.begin(), value.end(),
                [](unsigned char ch) { return std::isspace(ch) != 0 || ch == '_' || ch == '-'; }),
                value.end());
            std::transform(value.begin(), value.end(), value.begin(),
                [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            return value;
        }

        bool parse_topology_direction(
            const ResolvedParamMap& params,
            GraphNeighborhoodDirection& out,
            std::string& err) {
            const std::string raw = as_str(params, "direction").value_or("outgoing");
            const std::string normalized = normalized_direction_string(raw);
            if (normalized.empty() || normalized == "outgoing" || normalized == "out") {
                out = GraphNeighborhoodDirection::Outgoing;
                return true;
            }
            if (normalized == "incoming" || normalized == "in") {
                out = GraphNeighborhoodDirection::Incoming;
                return true;
            }
            if (normalized == "both" || normalized == "all") {
                out = GraphNeighborhoodDirection::Both;
                return true;
            }
            err = "unknown topology direction '" + raw + "'; expected outgoing, incoming, or both";
            return false;
        }

        std::size_t topology_max_neighbors(const ResolvedParamMap& params) {
            const auto value = as_i64(params, "max_neighbors");
            if (!value.has_value()) {
                return std::numeric_limits<std::size_t>::max();
            }
            return static_cast<std::size_t>(std::max<std::int64_t>(0, *value));
        }

        bool parse_topology_hierarchy_traversal(
            const ResolvedParamMap& params,
            std::string& out,
            std::string& err) {
            const auto value = as_str(params, "traversal");
            if (!value.has_value()) {
                out.clear();
                return true;
            }

            const std::string normalized = normalized_direction_string(*value);
            if (normalized.empty() ||
                normalized == "children" ||
                normalized == "child" ||
                normalized == "descendants" ||
                normalized == "descendant") {
                out = "children";
                return true;
            }
            if (normalized == "parent" ||
                normalized == "parents" ||
                normalized == "ancestor" ||
                normalized == "ancestors") {
                out = "parent";
                return true;
            }
            if (normalized == "sibling" ||
                normalized == "siblings") {
                out = "siblings";
                return true;
            }
            if (normalized == "adjacent" ||
                normalized == "parentchildren" ||
                normalized == "parentschildren" ||
                normalized == "parentandchildren") {
                out = "parent_and_children";
                return true;
            }

            err = "unknown topology hierarchy traversal '" + *value +
                "'; expected children, parent, siblings, or parent_and_children";
            return false;
        }

        bool parse_topology_options(
            const ResolvedParamMap& params,
            op_kernels::TopologyNeighborhoodOptions& out,
            std::string& err) {
            if (!parse_topology_direction(params, out.direction, err)) {
                return false;
            }
            out.include_anchor = as_bool(params, "include_anchor").value_or(false);
            out.max_neighbors = topology_max_neighbors(params);
            out.graph_version = as_str(params, "version").value_or(std::string{});
            if (!parse_topology_hierarchy_traversal(params, out.hierarchy_traversal, err)) {
                return false;
            }

            const auto max_depth = as_i64(params, "max_depth");
            out.has_hierarchy_max_depth = max_depth.has_value();
            out.hierarchy_max_depth = max_depth.has_value()
                ? static_cast<std::size_t>(std::max<std::int64_t>(0, *max_depth))
                : std::size_t{ 1 };
            return true;
        }

        BoolBuffer compare_vec(std::span<const double> a, std::span<const double> b, const std::string& op_name) {
            const std::size_t n = std::max(a.size(), b.size());
            BoolBuffer out(n, kBoolFalse);
            for (std::size_t i = 0; i < n; ++i) {
                const double x = (i < a.size()) ? a[i] : qnan();
                const double y = (i < b.size()) ? b[i] : qnan();
                if (!std::isfinite(x) || !std::isfinite(y)) {
                    out[i] = kBoolFalse;
                    continue;
                }

                if (op_name == "cmp.lt") out[i] = (x < y) ? kBoolTrue : kBoolFalse;
                else if (op_name == "cmp.le") out[i] = (x <= y) ? kBoolTrue : kBoolFalse;
                else if (op_name == "cmp.gt") out[i] = (x > y) ? kBoolTrue : kBoolFalse;
                else if (op_name == "cmp.ge") out[i] = (x >= y) ? kBoolTrue : kBoolFalse;
                else if (op_name == "cmp.eq") out[i] = (x == y) ? kBoolTrue : kBoolFalse;
                else if (op_name == "cmp.ne") out[i] = (x != y) ? kBoolTrue : kBoolFalse;
            }
            return out;
        }

        BoolBuffer cross_vec(std::span<const double> a, std::span<const double> b, bool up) {
            const std::size_t n = std::max(a.size(), b.size());
            BoolBuffer out(n, kBoolFalse);
            if (n < 2) return out;

            std::size_t finite_pairs = 0;
            std::size_t rhs_relation_true = 0;
            std::size_t crosses = 0;

            for (std::size_t i = 1; i < n; ++i) {
                const double a0 = (i - 1 < a.size()) ? a[i - 1] : qnan();
                const double a1 = (i < a.size()) ? a[i] : qnan();
                const double b0 = (i - 1 < b.size()) ? b[i - 1] : qnan();
                const double b1 = (i < b.size()) ? b[i] : qnan();

                if (!std::isfinite(a0) || !std::isfinite(a1) || !std::isfinite(b0) || !std::isfinite(b1)) {
                    out[i] = kBoolFalse;
                    continue;
                }

                ++finite_pairs;
                if (up) {
                    if (a1 > b1) ++rhs_relation_true;
                    out[i] = (a0 <= b0 && a1 > b1) ? kBoolTrue : kBoolFalse;
                }
                else {
                    if (a1 < b1) ++rhs_relation_true;
                    out[i] = (a0 >= b0 && a1 < b1) ? kBoolTrue : kBoolFalse;
                }

                if (out[i]) ++crosses;
            }

            if (cross_relation_debug_enabled() &&
                finite_pairs >= 50 &&
                rhs_relation_true == 0 &&
                crosses == 0) {
                static bool warned = false;
                if (!warned) {
                    warned = true;
                    std::cerr
                        << "[SugarBot][debug] cross_" << (up ? "up" : "down")
                        << ": LHS is never " << (up ? ">" : "<")
                        << " RHS on finite bars. \nIf RHS is a rolling extreme/envelope that includes the current bar "
                        << "(e.g., Donchian upper/lower), this pattern is often structurally impossible. \n"
                        << "Consider aligning RHS to map_shift(RHS, 1).\n";
                }
            }

            return out;
        }

        bool eval_numeric_op(const ProgramEvalContext& ctx,
            const NumericProgramNode& node,
            const std::vector<ProgramInputBuffer>& inputs,
            NumericBuffer& out,
            std::string& err) {
            const auto& op = node.op_name;
            const auto n = ctx.row_count;

            auto unary_num = [&](auto fn) {
                out = fn(inputs.at(0).num);
                return validate_numeric_size(out, n, err, op);
            };
            auto binary_num = [&](auto fn) {
                out = fn(inputs.at(0).num, inputs.at(1).num);
                return validate_numeric_size(out, n, err, op);
            };

            if (op == "core.close") {
                const auto col = primary_close(ctx);
                out.assign(col.begin(), col.end());
                return true;
            }
            if (op == "core.const") {
                const double value = as_f64(node.params, "value").value_or(0.0);
                out.assign(n, value);
                return true;
            }
            if (op == "core.precomputed_num" || op == "core.ref") {
                const std::string key = (op == "core.precomputed_num")
                    ? as_str(node.params, "name").value_or(std::string{})
                    : as_str(node.params, "ref").value_or(std::string{});
                if (!ctx.env) {
                    err = op + " missing compile env storage";
                    return false;
                }
                auto it = ctx.env->numeric_leaf_values.find(key);
                if (it == ctx.env->numeric_leaf_values.end()) {
                    err = op + " leaf not found: " + key;
                    return false;
                }
                out = it->second;
                return validate_numeric_size(out, n, err, op);
            }
            if (op == "core.select") {
                out = op_kernels::core_select(inputs.at(0).boo, inputs.at(1).num, inputs.at(2).num);
                return validate_numeric_size(out, n, err, op);
            }
            if (op == "spatial.identity") {
                out.assign(inputs.at(0).num.begin(), inputs.at(0).num.end());
                return validate_numeric_size(out, n, err, op);
            }
            if (op == "spatial.radius_mean") {
                const double radius = as_f64(node.params, "radius").value_or(0.0);
                const bool include_self = as_bool(node.params, "include_self").value_or(true);
                std::unique_ptr<NeighborhoodQuery> nq;
                if (!resolve_numeric_op_topology(ctx, op, node.params, nq, err)) return false;
                out = op_kernels::spatial_radius_mean(inputs.at(0).num, *nq, radius, include_self);
                return validate_numeric_size(out, n, err, op);
            }
            if (op == "st.radius_mean") {
                const std::size_t last_n = static_cast<std::size_t>(std::max<std::int64_t>(1, as_i64(node.params, "last_n").value_or(1)));
                const double radius = as_f64(node.params, "radius").value_or(0.0);
                std::unique_ptr<NeighborhoodQuery> nq;
                if (!resolve_numeric_op_topology(ctx, op, node.params, nq, err)) return false;
                out = op_kernels::st_radius_mean(inputs.at(0).num, *nq, last_n, radius);
                return validate_numeric_size(out, n, err, op);
            }
            if (op == "topology.neighbor_mean" ||
                op == "topology.neighbor_min" ||
                op == "topology.neighbor_max" ||
                op == "topology.weighted_sum") {
                op_kernels::TopologyNeighborhoodOptions options{};
                if (!parse_topology_options(node.params, options, err)) {
                    err = op + " parameter validation failed: " + err;
                    return false;
                }
                std::unique_ptr<NeighborhoodQuery> nq;
                if (!resolve_numeric_op_topology(ctx, op, node.params, nq, err)) return false;
                try {
                    if (op == "topology.neighbor_mean") {
                        out = op_kernels::topology_neighbor_mean(inputs.at(0).num, *nq, options);
                    }
                    else if (op == "topology.neighbor_min") {
                        out = op_kernels::topology_neighbor_min(inputs.at(0).num, *nq, options);
                    }
                    else if (op == "topology.neighbor_max") {
                        out = op_kernels::topology_neighbor_max(inputs.at(0).num, *nq, options);
                    }
                    else {
                        out = op_kernels::topology_weighted_sum(inputs.at(0).num, *nq, options);
                    }
                }
                catch (const std::exception& ex) {
                    err = op + " topology query failed: " + std::string(ex.what());
                    return false;
                }
                return validate_numeric_size(out, n, err, op);
            }
            if (op == "topology.degree") {
                op_kernels::TopologyNeighborhoodOptions options{};
                if (!parse_topology_options(node.params, options, err)) {
                    err = op + " parameter validation failed: " + err;
                    return false;
                }
                std::unique_ptr<NeighborhoodQuery> nq;
                if (!resolve_numeric_op_topology(ctx, op, node.params, nq, err)) return false;
                try {
                    out = op_kernels::topology_degree(*nq, options, n);
                }
                catch (const std::exception& ex) {
                    err = op + " topology query failed: " + std::string(ex.what());
                    return false;
                }
                return validate_numeric_size(out, n, err, op);
            }
            if (op == "ms.diff") return binary_num(op_kernels::ms_diff);
            if (op == "ms.ratio") return binary_num(op_kernels::ms_ratio);
            if (op == "ms.zscore") {
                const std::size_t window = static_cast<std::size_t>(std::max<std::int64_t>(1, as_i64(node.params, "n").value_or(20)));
                out = op_kernels::ms_zscore(inputs.at(0).num, window);
                return validate_numeric_size(out, n, err, op);
            }
            if (op == "ms.correlation") {
                const std::size_t window = static_cast<std::size_t>(std::max<std::int64_t>(1, as_i64(node.params, "n").value_or(1)));
                out = op_kernels::ms_correlation(inputs.at(0).num, inputs.at(1).num, window);
                return validate_numeric_size(out, n, err, op);
            }
            if (op == "ts.roc") {
                const std::size_t k = static_cast<std::size_t>(std::max<std::int64_t>(1, as_i64(node.params, "k").value_or(1)));
                out = op_kernels::ts_roc(inputs.at(0).num, k);
                return validate_numeric_size(out, n, err, op);
            }
            if (op == "ts.sma") {
                const std::size_t window = static_cast<std::size_t>(std::max<std::int64_t>(1, as_i64(node.params, "n").value_or(1)));
                out = op_kernels::ts_sma(inputs.at(0).num, window);
                return validate_numeric_size(out, n, err, op);
            }
            if (op == "ts.ema") {
                const std::size_t window = static_cast<std::size_t>(std::max<std::int64_t>(1, as_i64(node.params, "n").value_or(1)));
                out = op_kernels::ts_ema(inputs.at(0).num, window);
                return validate_numeric_size(out, n, err, op);
            }
            if (op == "ts.stddev") {
                const std::size_t window = static_cast<std::size_t>(std::max<std::int64_t>(1, as_i64(node.params, "n").value_or(1)));
                out = op_kernels_math::stddev(inputs.at(0).num, window);
                return validate_numeric_size(out, n, err, op);
            }
            if (op == "ts.stddev_ema") {
                const std::size_t window = static_cast<std::size_t>(std::max<std::int64_t>(1, as_i64(node.params, "n").value_or(1)));
                out = op_kernels_math::stddev_ema(inputs.at(0).num, window);
                return validate_numeric_size(out, n, err, op);
            }
            if (op == "ts.zscore_sma") {
                const std::size_t window = static_cast<std::size_t>(std::max<std::int64_t>(1, as_i64(node.params, "n").value_or(1)));
                out = op_kernels_math::zscore_sma(inputs.at(0).num, window);
                return validate_numeric_size(out, n, err, op);
            }
            if (op == "ts.zscore_ema") {
                const std::size_t window = static_cast<std::size_t>(std::max<std::int64_t>(1, as_i64(node.params, "n").value_or(1)));
                out = op_kernels_math::zscore_ema(inputs.at(0).num, window);
                return validate_numeric_size(out, n, err, op);
            }
            if (op == "ts.roll_max") {
                const std::size_t window = static_cast<std::size_t>(std::max<std::int64_t>(1, as_i64(node.params, "n").value_or(1)));
                out = op_kernels::ts_roll_max(inputs.at(0).num, window);
                return validate_numeric_size(out, n, err, op);
            }
            if (op == "ts.roll_min") {
                const std::size_t window = static_cast<std::size_t>(std::max<std::int64_t>(1, as_i64(node.params, "n").value_or(1)));
                out = op_kernels::ts_roll_min(inputs.at(0).num, window);
                return validate_numeric_size(out, n, err, op);
            }
            if (op == "ts.donchian_upper") {
                const std::size_t window = static_cast<std::size_t>(std::max<std::int64_t>(1, as_i64(node.params, "n").value_or(1)));
                out = op_kernels::ts_donchian_upper(inputs.at(0).num, window);
                return validate_numeric_size(out, n, err, op);
            }
            if (op == "ts.donchian_lower") {
                const std::size_t window = static_cast<std::size_t>(std::max<std::int64_t>(1, as_i64(node.params, "n").value_or(1)));
                out = op_kernels::ts_donchian_lower(inputs.at(0).num, window);
                return validate_numeric_size(out, n, err, op);
            }
            if (op == "ts.donchian_mid") {
                const std::size_t window = static_cast<std::size_t>(std::max<std::int64_t>(1, as_i64(node.params, "n").value_or(1)));
                out = op_kernels::ts_donchian_mid(inputs.at(0).num, window);
                return validate_numeric_size(out, n, err, op);
            }
            if (op == "ts.donchian_width") {
                const std::size_t window = static_cast<std::size_t>(std::max<std::int64_t>(1, as_i64(node.params, "n").value_or(1)));
                out = op_kernels::ts_donchian_width(inputs.at(0).num, window);
                return validate_numeric_size(out, n, err, op);
            }
            if (op == "ts.percentile") {
                const std::size_t window = static_cast<std::size_t>(std::max<std::int64_t>(1, as_i64(node.params, "n").value_or(1)));
                const double percentile = as_f64(node.params, "q").value_or(50.0);
                out = op_kernels::ts_percentile(inputs.at(0).num, window, percentile);
                return validate_numeric_size(out, n, err, op);
            }
            if (op == "ts.entropy") {
                const std::size_t window = static_cast<std::size_t>(std::max<std::int64_t>(1, as_i64(node.params, "n").value_or(1)));
                const std::size_t bins = static_cast<std::size_t>(std::max<std::int64_t>(1, as_i64(node.params, "bins").value_or(1)));
                out = op_kernels::ts_entropy(inputs.at(0).num, window, bins);
                return validate_numeric_size(out, n, err, op);
            }
            if (op == "ts.autocorrelation") {
                const std::size_t window = static_cast<std::size_t>(std::max<std::int64_t>(2, as_i64(node.params, "n").value_or(2)));
                const std::size_t lag = static_cast<std::size_t>(std::max<std::int64_t>(1, as_i64(node.params, "lag").value_or(1)));
                out = op_kernels::ts_autocorrelation(inputs.at(0).num, window, lag);
                return validate_numeric_size(out, n, err, op);
            }
            if (op == "ts.hurst") {
                const std::size_t window = static_cast<std::size_t>(std::max<std::int64_t>(2, as_i64(node.params, "n").value_or(2)));
                out = op_kernels::ts_hurst(inputs.at(0).num, window);
                return validate_numeric_size(out, n, err, op);
            }
            if (op == "ts.normalize_minmax") {
                const std::size_t window = static_cast<std::size_t>(std::max<std::int64_t>(1, as_i64(node.params, "n").value_or(1)));
                out = op_kernels::ts_normalize_minmax(inputs.at(0).num, window);
                return validate_numeric_size(out, n, err, op);
            }
            if (op == "ts.atr") {
                const std::size_t window = static_cast<std::size_t>(std::max<std::int64_t>(1, as_i64(node.params, "n").value_or(1)));
                out = op_kernels::ts_atr_from_series(inputs.at(0).num, window);
                return validate_numeric_size(out, n, err, op);
            }
            if (op == "ts.rsi") {
                const std::size_t window = static_cast<std::size_t>(std::max<std::int64_t>(1, as_i64(node.params, "n").value_or(1)));
                out = op_kernels::ts_rsi_wilder(inputs.at(0).num, window);
                return validate_numeric_size(out, n, err, op);
            }
            if (op == "ts.atr_wilder") {
                const std::size_t window = static_cast<std::size_t>(std::max<std::int64_t>(1, as_i64(node.params, "n").value_or(1)));
                out = op_kernels::ts_atr_wilder(inputs.at(0).num, window);
                return validate_numeric_size(out, n, err, op);
            }
            if (op == "ts.macd") {
                const std::size_t fast = static_cast<std::size_t>(std::max<std::int64_t>(1, as_i64(node.params, "fast").value_or(1)));
                const std::size_t slow = static_cast<std::size_t>(std::max<std::int64_t>(1, as_i64(node.params, "slow").value_or(1)));
                const std::size_t signal = static_cast<std::size_t>(std::max<std::int64_t>(1, as_i64(node.params, "signal").value_or(1)));
                out = op_kernels::ts_macd_histogram(inputs.at(0).num, fast, slow, signal);
                return validate_numeric_size(out, n, err, op);
            }
            if (op == "ts.bollinger_width") {
                const std::size_t window = static_cast<std::size_t>(std::max<std::int64_t>(1, as_i64(node.params, "n").value_or(1)));
                const double k = as_f64(node.params, "k").value_or(2.0);
                out = op_kernels::ts_bollinger_width(inputs.at(0).num, window, k);
                return validate_numeric_size(out, n, err, op);
            }
            if (op == "ts.regime_change") {
                const std::size_t window = static_cast<std::size_t>(std::max<std::int64_t>(2, as_i64(node.params, "n").value_or(2)));
                out = op_kernels::ts_regime_change(inputs.at(0).num, window);
                return validate_numeric_size(out, n, err, op);
            }
            if (op == "ts.volatility_ratio") {
                const std::size_t fast_n = static_cast<std::size_t>(std::max<std::int64_t>(1, as_i64(node.params, "fast_n").value_or(1)));
                const std::size_t slow_n = static_cast<std::size_t>(std::max<std::int64_t>(1, as_i64(node.params, "slow_n").value_or(1)));
                out = op_kernels::ts_volatility_ratio(inputs.at(0).num, fast_n, slow_n);
                return validate_numeric_size(out, n, err, op);
            }
            if (op == "ts.shift") {
                const std::size_t k = static_cast<std::size_t>(std::max<std::int64_t>(0, as_i64(node.params, "k").value_or(0)));
                const double fill = as_f64(node.params, "fill").value_or(qnan());
                out = op_kernels::ts_shift(inputs.at(0).num, k, fill);
                return validate_numeric_size(out, n, err, op);
            }
            if (op == "ts.delta") return unary_num(op_kernels::ts_delta);
            if (op == "ts.true_range") return unary_num(op_kernels::ts_true_range_delta);
            if (op == "ts.true_range_ohlc") {
                out = op_kernels::ts_true_range_ohlc(inputs.at(0).num, inputs.at(1).num, inputs.at(2).num);
                return validate_numeric_size(out, n, err, op);
            }
            if (op == "ts.neg") return unary_num(op_kernels::ts_neg);
            if (op == "ts.abs") return unary_num(op_kernels::ts_abs);
            if (op == "ts.log") return unary_num(op_kernels::ts_log);
            if (op == "ts.exp") return unary_num(op_kernels::ts_exp);
            if (op == "ts.sqrt") return unary_num(op_kernels::ts_sqrt);
            if (op == "ts.pow_const") {
                const double p = as_f64(node.params, "p").value_or(1.0);
                out = op_kernels::ts_pow_const(inputs.at(0).num, p);
                return validate_numeric_size(out, n, err, op);
            }
            if (op == "ts.normalize_minmax_global") return unary_num(op_kernels::ts_normalize_minmax_global);
            if (op == "ts.ratio_of_ma") {
                out = op_kernels::ts_ratio_of_ma(primary_close(ctx), inputs.at(0).num);
                return validate_numeric_size(out, n, err, op);
            }
            if (op == "ts.add") return binary_num(op_kernels::ts_add);
            if (op == "ts.sub") return binary_num(op_kernels::ts_sub);
            if (op == "ts.mul") return binary_num(op_kernels::ts_mul);
            if (op == "ts.div") return binary_num(op_kernels::ts_div);
            if (op == "ts.min") return binary_num(op_kernels::ts_min);
            if (op == "ts.max") return binary_num(op_kernels::ts_max);
            if (op == "ts.clamp_const") {
                const double lo = as_f64(node.params, "lo").value_or(0.0);
                const double hi = as_f64(node.params, "hi").value_or(0.0);
                out = op_kernels::ts_clamp_const(inputs.at(0).num, lo, hi);
                return validate_numeric_size(out, n, err, op);
            }
            if (op == "ts.clamp") {
                out = op_kernels::ts_clamp(inputs.at(0).num, inputs.at(1).num, inputs.at(2).num);
                return validate_numeric_size(out, n, err, op);
            }

            err = "unsupported numeric op in compiled program: " + op;
            return false;
        }

        bool eval_bool_op(const ProgramEvalContext& ctx,
            const BoolProgramNode& node,
            const std::vector<ProgramInputBuffer>& inputs,
            BoolBuffer& out,
            std::string& err) {
            const auto& op = node.op_name;
            const auto n = ctx.row_count;

            if (op == "core.bool_const") {
                const bool value = as_bool(node.params, "value").value_or(false);
                out.assign(n, value ? kBoolTrue : kBoolFalse);
                return true;
            }
            if (op == "core.precomputed_bool") {
                const std::string key = as_str(node.params, "name").value_or(std::string{});
                if (!ctx.env) {
                    err = op + " missing compile env storage";
                    return false;
                }
                auto it = ctx.env->bool_leaf_values.find(key);
                if (it == ctx.env->bool_leaf_values.end()) {
                    err = op + " leaf not found: " + key;
                    return false;
                }
                out = it->second;
                return validate_bool_size(out, n, err, op);
            }
            if (op == "logic.not") {
                out.assign(inputs.at(0).boo.size(), kBoolFalse);
                for (std::size_t i = 0; i < inputs.at(0).boo.size(); ++i) out[i] = inputs.at(0).boo[i] ? kBoolFalse : kBoolTrue;
                return validate_bool_size(out, n, err, op);
            }
            if (op == "logic.and" || op == "logic.or" || op == "logic.nand" || op == "logic.nor") {
                const std::size_t m = std::max(inputs.at(0).boo.size(), inputs.at(1).boo.size());
                out.assign(m, kBoolFalse);
                for (std::size_t i = 0; i < m; ++i) {
                    const std::uint8_t a = (i < inputs.at(0).boo.size()) ? inputs.at(0).boo[i] : kBoolFalse;
                    const std::uint8_t b = (i < inputs.at(1).boo.size()) ? inputs.at(1).boo[i] : kBoolFalse;
                    if (op == "logic.and") out[i] = (a && b) ? kBoolTrue : kBoolFalse;
                    else if (op == "logic.or") out[i] = (a || b) ? kBoolTrue : kBoolFalse;
                    else if (op == "logic.nand") out[i] = (a && b) ? kBoolFalse : kBoolTrue;
                    else out[i] = (a || b) ? kBoolFalse : kBoolTrue;
                }
                return validate_bool_size(out, n, err, op);
            }
            if (op == "cmp.lt" || op == "cmp.le" || op == "cmp.gt" || op == "cmp.ge" || op == "cmp.eq" || op == "cmp.ne") {
                out = compare_vec(inputs.at(0).num, inputs.at(1).num, op);
                return validate_bool_size(out, n, err, op);
            }
            if (op == "cmp.cross_up") {
                out = cross_vec(inputs.at(0).num, inputs.at(1).num, true);
                return validate_bool_size(out, n, err, op);
            }
            if (op == "cmp.cross_down") {
                out = cross_vec(inputs.at(0).num, inputs.at(1).num, false);
                return validate_bool_size(out, n, err, op);
            }

            err = "unsupported bool op in compiled program: " + op;
            return false;
        }

    } // namespace

    bool evaluate_strategy_plan_program_buffers(
        const DataView& dv,
        const ColumnRef& primary,
        const StrategyPlan& plan,
        ProgramEvaluationBuffers& out,
        std::string& err) {
        const StrategyPlanStorage* storage = plan.storage.get();
        if (!storage) {
            err = "compiled strategy plan has no program storage";
            return false;
        }
        if (!dv.has_col(primary)) {
            err = "primary series/column missing from DataView";
            return false;
        }

        const ProgramEvalContext ctx{
            dv,
            primary,
            storage->env.get(),
            dv.col(primary).size()
        };

        const auto& program = storage->program;
        out = ProgramEvaluationBuffers{};
        out.numeric_slots.resize(program.numeric_slot_count);
        out.bool_slots.resize(program.bool_slot_count);

        auto eval_numeric_node = [&](const NumericProgramNode& node) -> bool {
            std::vector<ProgramInputBuffer> inputs;
            inputs.reserve(node.inputs.size());
            for (const auto& in : node.inputs) {
                if (in.type == ValueType::Num) {
                    if (in.slot >= out.numeric_slots.size()) {
                        err = "numeric input slot out of range for op " + node.op_name;
                        return false;
                    }
                    inputs.push_back(ProgramInputBuffer{
                        ValueType::Num,
                        std::span<const double>(out.numeric_slots[in.slot].data(), out.numeric_slots[in.slot].size()),
                        {}
                    });
                }
                else {
                    if (in.slot >= out.bool_slots.size()) {
                        err = "bool input slot out of range for op " + node.op_name;
                        return false;
                    }
                    inputs.push_back(ProgramInputBuffer{
                        ValueType::Bool,
                        {},
                        std::span<const std::uint8_t>(out.bool_slots[in.slot].data(), out.bool_slots[in.slot].size())
                    });
                }
            }

            if (node.output_slot >= out.numeric_slots.size()) {
                err = "numeric output slot out of range for op " + node.op_name;
                return false;
            }
            if (!eval_numeric_op(ctx, node, inputs, out.numeric_slots[node.output_slot], err)) {
                return false;
            }
            return true;
        };

        auto eval_bool_node = [&](const BoolProgramNode& node) -> bool {
            std::vector<ProgramInputBuffer> inputs;
            inputs.reserve(node.inputs.size());
            for (const auto& in : node.inputs) {
                if (in.type == ValueType::Num) {
                    if (in.slot >= out.numeric_slots.size()) {
                        err = "numeric input slot out of range for op " + node.op_name;
                        return false;
                    }
                    inputs.push_back(ProgramInputBuffer{
                        ValueType::Num,
                        std::span<const double>(out.numeric_slots[in.slot].data(), out.numeric_slots[in.slot].size()),
                        {}
                    });
                }
                else {
                    if (in.slot >= out.bool_slots.size()) {
                        err = "bool input slot out of range for op " + node.op_name;
                        return false;
                    }
                    inputs.push_back(ProgramInputBuffer{
                        ValueType::Bool,
                        {},
                        std::span<const std::uint8_t>(out.bool_slots[in.slot].data(), out.bool_slots[in.slot].size())
                    });
                }
            }

            if (node.output_slot >= out.bool_slots.size()) {
                err = "bool output slot out of range for op " + node.op_name;
                return false;
            }
            if (!eval_bool_op(ctx, node, inputs, out.bool_slots[node.output_slot], err)) {
                return false;
            }
            return true;
        };

        if (program.evaluation_order.empty()) {
            for (const auto& node : program.numeric_nodes) {
                if (!eval_numeric_node(node)) return false;
            }
            for (const auto& node : program.bool_nodes) {
                if (!eval_bool_node(node)) return false;
            }
        }
        else {
            for (const auto& step : program.evaluation_order) {
                if (step.type == ValueType::Bool) {
                    if (step.index >= program.bool_nodes.size()) {
                        err = "bool evaluation-order index out of range";
                        return false;
                    }
                    if (!eval_bool_node(program.bool_nodes[step.index])) return false;
                }
                else {
                    if (step.index >= program.numeric_nodes.size()) {
                        err = "numeric evaluation-order index out of range";
                        return false;
                    }
                    if (!eval_numeric_node(program.numeric_nodes[step.index])) return false;
                }
            }
        }

        struct DynamicEvaluationState {
            std::vector<BoolBuffer> subgraph_active{};
            RunDynamicExecutionSummary summary{};
        };

        auto mix_route_digest = [](std::uint64_t digest,
                                   std::uint64_t value) -> std::uint64_t {
            return digest ^
                (value + 0x9e3779b97f4a7c15ULL + (digest << 6) + (digest >> 2));
        };

        auto subgraph_name = [&](std::size_t index) -> std::string {
            const auto& dynamic = program.dynamic_execution;
            if (index >= dynamic.subgraphs.size()) {
                return {};
            }
            return dynamic.subgraphs[index].id;
        };

        auto append_subgraph_with_closure = [&](std::vector<std::size_t>& out_indices,
                                                std::size_t subgraph_index) {
            const auto& dynamic = program.dynamic_execution;
            if (subgraph_index >= dynamic.subgraphs.size()) {
                return;
            }
            if (std::find(out_indices.begin(), out_indices.end(), subgraph_index) ==
                out_indices.end()) {
                out_indices.push_back(subgraph_index);
            }
            const auto& closure =
                dynamic.subgraphs[subgraph_index].dependency_closure_subgraphs;
            for (const auto dep : closure) {
                if (dep < dynamic.subgraphs.size() &&
                    std::find(out_indices.begin(), out_indices.end(), dep) ==
                        out_indices.end()) {
                    out_indices.push_back(dep);
                }
            }
        };

        auto append_dynamic_sample = [&](DynamicEvaluationState& state,
                                         std::size_t row,
                                         const CompiledDynamicRoute& route,
                                         std::string selected_case_id,
                                         bool fallback_used,
                                         bool condition_value,
                                         const std::vector<std::size_t>& active_subgraphs) {
            auto& summary = state.summary;
            if (summary.decision_samples.size() >= summary.decision_sample_limit) {
                return;
            }
            RunDynamicDecisionSample sample{};
            sample.row = row;
            sample.route_id = route.id;
            sample.selected_case_id = std::move(selected_case_id);
            sample.fallback_used = fallback_used;
            sample.condition_value = condition_value;
            sample.active_subgraphs.reserve(active_subgraphs.size());
            for (const auto subgraph_index : active_subgraphs) {
                sample.active_subgraphs.push_back(subgraph_name(subgraph_index));
            }
            summary.decision_samples.push_back(std::move(sample));
        };

        auto build_dynamic_evaluation_state =
            [&](DynamicEvaluationState& state) -> bool {
            const auto& dynamic = program.dynamic_execution;
            if (!dynamic.enabled) {
                return true;
            }

            state = DynamicEvaluationState{};
            state.subgraph_active.assign(
                dynamic.subgraphs.size(),
                BoolBuffer(ctx.row_count, kBoolFalse));

            auto& summary = state.summary;
            summary.enabled = true;
            summary.mode = "declared_dynamic";
            summary.decision_sample_limit = dynamic.decision_sample_limit;
            summary.max_active_subgraph_cap = dynamic.max_active_subgraphs;
            summary.route_decision_digest = 0x6d04f77f9fd41d5dULL;
            summary.subgraph_ids.reserve(dynamic.subgraphs.size());
            for (const auto& subgraph : dynamic.subgraphs) {
                summary.subgraph_ids.push_back(subgraph.id);
            }

            summary.routes.reserve(dynamic.routes.size());
            for (const auto& route : dynamic.routes) {
                RunDynamicRouteSummary route_summary{};
                route_summary.id = route.id;
                route_summary.condition_node_id = route.condition_node_id;
                route_summary.condition_is_constant = route.condition_is_constant;
                route_summary.constant_condition_value = route.constant_condition_value;
                route_summary.fallback_subgraphs.reserve(route.fallback_subgraphs.size());
                for (const auto subgraph_index : route.fallback_subgraphs) {
                    route_summary.fallback_subgraphs.push_back(subgraph_name(subgraph_index));
                }
                route_summary.cases.reserve(route.cases.size());
                for (const auto& route_case : route.cases) {
                    RunDynamicRouteCaseSummary case_summary{};
                    case_summary.id = route_case.id;
                    case_summary.when = route_case.when;
                    case_summary.activate_subgraphs.reserve(
                        route_case.activate_subgraphs.size());
                    for (const auto subgraph_index : route_case.activate_subgraphs) {
                        case_summary.activate_subgraphs.push_back(
                            subgraph_name(subgraph_index));
                    }
                    route_summary.cases.push_back(std::move(case_summary));
                }
                summary.routes.push_back(std::move(route_summary));
            }

            summary.outputs.reserve(dynamic.outputs.size());
            for (const auto& output : dynamic.outputs) {
                RunDynamicOutputSummary output_summary{};
                output_summary.id = output.id;
                output_summary.node_id = output.node_id;
                output_summary.owning_subgraph = subgraph_name(output.owning_subgraph);
                summary.outputs.push_back(std::move(output_summary));
            }

            for (std::size_t row = 0; row < ctx.row_count; ++row) {
                for (std::size_t route_index = 0;
                     route_index < dynamic.routes.size();
                     ++route_index) {
                    const auto& route = dynamic.routes[route_index];
                    if (route.condition_slot >= out.bool_slots.size()) {
                        err = "dynamic route condition slot out of range for route '" +
                            route.id + "'";
                        return false;
                    }
                    const auto& condition = out.bool_slots[route.condition_slot];
                    if (row >= condition.size()) {
                        err = "dynamic route condition buffer is shorter than row domain for route '" +
                            route.id + "'";
                        return false;
                    }
                    const bool condition_value = condition[row] != kBoolFalse;
                    auto& route_summary = summary.routes[route_index];
                    if (condition_value) {
                        ++route_summary.true_decisions;
                    }
                    else {
                        ++route_summary.false_decisions;
                    }

                    const std::vector<std::size_t>* selected =
                        &route.fallback_subgraphs;
                    std::string selected_case_id = "fallback";
                    bool fallback_used = true;
                    std::size_t selected_case_index = kInvalidProgramSlot;
                    for (std::size_t case_index = 0;
                         case_index < route.cases.size();
                         ++case_index) {
                        const auto& route_case = route.cases[case_index];
                        if (route_case.when == condition_value) {
                            selected = &route_case.activate_subgraphs;
                            selected_case_id = route_case.id;
                            fallback_used = false;
                            selected_case_index = case_index;
                            ++route_summary.cases[case_index].decisions;
                            break;
                        }
                    }
                    if (fallback_used) {
                        ++route_summary.fallback_decisions;
                        ++summary.fallback_decisions;
                    }

                    std::vector<std::size_t> active_with_closure;
                    for (const auto selected_subgraph : *selected) {
                        append_subgraph_with_closure(
                            active_with_closure,
                            selected_subgraph);
                    }
                    for (const auto active_subgraph : active_with_closure) {
                        if (active_subgraph < state.subgraph_active.size()) {
                            state.subgraph_active[active_subgraph][row] = kBoolTrue;
                        }
                    }

                    summary.route_decision_digest = mix_route_digest(
                        summary.route_decision_digest,
                        static_cast<std::uint64_t>(row));
                    summary.route_decision_digest = mix_route_digest(
                        summary.route_decision_digest,
                        static_cast<std::uint64_t>(route_index));
                    summary.route_decision_digest = mix_route_digest(
                        summary.route_decision_digest,
                        condition_value ? 1ULL : 0ULL);
                    summary.route_decision_digest = mix_route_digest(
                        summary.route_decision_digest,
                        fallback_used ? 1ULL : 0ULL);
                    summary.route_decision_digest = mix_route_digest(
                        summary.route_decision_digest,
                        selected_case_index == kInvalidProgramSlot
                            ? 0xffffffffffffffffULL
                            : static_cast<std::uint64_t>(selected_case_index));
                    for (const auto active_subgraph : active_with_closure) {
                        summary.route_decision_digest = mix_route_digest(
                            summary.route_decision_digest,
                            static_cast<std::uint64_t>(active_subgraph));
                    }

                    append_dynamic_sample(
                        state,
                        row,
                        route,
                        std::move(selected_case_id),
                        fallback_used,
                        condition_value,
                        active_with_closure);
                    ++summary.total_decisions;
                }

                std::size_t active_count = 0;
                for (const auto& mask : state.subgraph_active) {
                    if (row < mask.size() && mask[row] != kBoolFalse) {
                        ++active_count;
                    }
                }
                summary.max_active_subgraphs_per_row =
                    std::max(summary.max_active_subgraphs_per_row, active_count);
                if (dynamic.max_active_subgraphs > 0 &&
                    active_count > dynamic.max_active_subgraphs) {
                    summary.bounded_by_declared_caps = false;
                    err = "dynamic execution active subgraph count " +
                        std::to_string(active_count) +
                        " exceeds execution.bounds.max_active_subgraphs " +
                        std::to_string(dynamic.max_active_subgraphs);
                    return false;
                }
            }

            summary.emitted_decision_samples = summary.decision_samples.size();
            summary.suppressed_decisions =
                summary.total_decisions - summary.emitted_decision_samples;
            return true;
        };

        auto apply_dynamic_node_masks =
            [&](const DynamicEvaluationState& state) -> bool {
            const auto& dynamic = program.dynamic_execution;
            if (!dynamic.enabled) {
                return true;
            }
            for (std::size_t subgraph_index = 0;
                 subgraph_index < dynamic.subgraphs.size();
                 ++subgraph_index) {
                if (subgraph_index >= state.subgraph_active.size()) {
                    err = "dynamic subgraph mask index out of range";
                    return false;
                }
                const auto& active = state.subgraph_active[subgraph_index];
                if (active.size() != ctx.row_count) {
                    err = "dynamic subgraph mask has unexpected row count";
                    return false;
                }
                for (const auto& node : dynamic.subgraphs[subgraph_index].nodes) {
                    if (node.type == ValueType::Num) {
                        if (node.slot >= out.numeric_slots.size()) {
                            err = "dynamic numeric node slot out of range for node '" +
                                node.node_id + "'";
                            return false;
                        }
                        auto& buffer = out.numeric_slots[node.slot];
                        if (buffer.size() != ctx.row_count) {
                            err = "dynamic numeric node buffer has unexpected row count for node '" +
                                node.node_id + "'";
                            return false;
                        }
                        for (std::size_t row = 0; row < buffer.size(); ++row) {
                            if (active[row] == kBoolFalse) {
                                if (!std::isnan(buffer[row])) {
                                    ++out.dynamic_execution.inactive_node_rows;
                                }
                                buffer[row] = qnan();
                            }
                        }
                    }
                    else {
                        if (node.slot >= out.bool_slots.size()) {
                            err = "dynamic bool node slot out of range for node '" +
                                node.node_id + "'";
                            return false;
                        }
                        auto& buffer = out.bool_slots[node.slot];
                        if (buffer.size() != ctx.row_count) {
                            err = "dynamic bool node buffer has unexpected row count for node '" +
                                node.node_id + "'";
                            return false;
                        }
                        for (std::size_t row = 0; row < buffer.size(); ++row) {
                            if (active[row] == kBoolFalse) {
                                if (buffer[row] != kBoolFalse) {
                                    ++out.dynamic_execution.inactive_node_rows;
                                }
                                buffer[row] = kBoolFalse;
                            }
                        }
                    }
                }
            }
            return true;
        };

        DynamicEvaluationState dynamic_state{};
        if (!build_dynamic_evaluation_state(dynamic_state)) {
            return false;
        }
        out.dynamic_execution = dynamic_state.summary;
        if (!apply_dynamic_node_masks(dynamic_state)) {
            return false;
        }

        auto subgraph_active_for_row = [&](std::size_t subgraph,
                                           std::size_t row,
                                           std::string& route_err) -> bool {
            if (subgraph >= dynamic_state.subgraph_active.size()) {
                route_err = "dynamic output owning subgraph index out of range";
                return false;
            }
            const auto& mask = dynamic_state.subgraph_active[subgraph];
            if (row >= mask.size()) {
                route_err = "dynamic subgraph mask is shorter than row domain";
                return false;
            }
            return mask[row] != kBoolFalse;
        };

        auto find_dynamic_output_index = [&](const std::string& role)
            -> std::size_t {
            const auto& outputs = program.dynamic_execution.outputs;
            const auto found = std::find_if(
                outputs.begin(),
                outputs.end(),
                [&](const auto& output) {
                    return output.id == role;
                });
            return found == outputs.end()
                ? kInvalidProgramSlot
                : static_cast<std::size_t>(std::distance(outputs.begin(), found));
        };

        auto apply_dynamic_output_gate = [&](const std::string& role,
                                             BoolBuffer& dst) -> bool {
            if (!program.dynamic_execution.enabled) {
                return true;
            }
            const auto output_index = find_dynamic_output_index(role);
            if (output_index == kInvalidProgramSlot) {
                return true;
            }
            const auto& output = program.dynamic_execution.outputs[output_index];
            if (output.ref.type != ValueType::Bool) {
                err = "dynamic execution output '" + role + "' is not a bool output";
                return false;
            }
            if (output.owning_subgraph == kInvalidProgramSlot) {
                err = "dynamic execution output '" + role + "' has no owning subgraph";
                return false;
            }
            if (dst.size() != ctx.row_count) {
                err = "dynamic execution output '" + role +
                    "' produced size " + std::to_string(dst.size()) +
                    ", expected " + std::to_string(ctx.row_count);
                return false;
            }

            for (std::size_t row = 0; row < dst.size(); ++row) {
                std::string route_err;
                const bool active = subgraph_active_for_row(
                    output.owning_subgraph,
                    row,
                    route_err);
                if (!route_err.empty()) {
                    err = route_err;
                    return false;
                }
                if (active) {
                    if (output_index < out.dynamic_execution.outputs.size()) {
                        ++out.dynamic_execution.outputs[output_index].active_rows;
                    }
                }
                else {
                    if (output_index < out.dynamic_execution.outputs.size()) {
                        ++out.dynamic_execution.outputs[output_index].inactive_rows;
                    }
                    ++out.dynamic_execution.inactive_output_rows;
                    dst[row] = kBoolFalse;
                }
            }
            return true;
        };

        auto assign_bool = [&](const std::string& role,
                               std::size_t slot,
                               BoolBuffer& dst,
                               bool default_true = false) -> bool {
            if (slot == kInvalidProgramSlot) {
                dst.assign(ctx.row_count, default_true ? kBoolTrue : kBoolFalse);
                return true;
            }
            dst = out.bool_slots.at(slot);
            return apply_dynamic_output_gate(role, dst);
        };

        if (!assign_bool("entry_long", program.entry_long_slot, out.signals.entry_long)) return false;
        if (!assign_bool("exit_long", program.exit_long_slot, out.signals.exit_long)) return false;
        if (!assign_bool("entry_short", program.entry_short_slot, out.signals.entry_short)) return false;
        if (!assign_bool("exit_short", program.exit_short_slot, out.signals.exit_short)) return false;
        if (!assign_bool("allow_trade", program.allow_trade_slot, out.signals.allow_trade, true)) return false;
        return true;
    }

    bool evaluate_strategy_plan_program(
        const DataView& dv,
        const ColumnRef& primary,
        const StrategyPlan& plan,
        ProgramSignalOutputs& out,
        std::string& err) {
        ProgramEvaluationBuffers buffers{};
        if (!evaluate_strategy_plan_program_buffers(dv, primary, plan, buffers, err)) {
            return false;
        }
        out = std::move(buffers.signals);
        return true;
    }

    bool compiled_program_supports_numeric_op(const std::string& op_name) {
        return is_supported_numeric_op(op_name);
    }

    bool compiled_program_supports_bool_op(const std::string& op_name) {
        return is_supported_bool_op(op_name);
    }

} // namespace sugar
