#include "engine/program_evaluator.h"
#include "program_evaluator_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "ops/op_kernels_math.h"
#include "op_kernels_core.h"
#include "op_kernels_ms.h"
#include "op_kernels_spatial.h"
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

        auto assign_bool = [&](std::size_t slot, BoolBuffer& dst, bool default_true = false) {
            if (slot == kInvalidProgramSlot) {
                dst.assign(ctx.row_count, default_true ? kBoolTrue : kBoolFalse);
                return;
            }
            dst = out.bool_slots.at(slot);
        };

        assign_bool(program.entry_long_slot, out.signals.entry_long);
        assign_bool(program.exit_long_slot, out.signals.exit_long);
        assign_bool(program.entry_short_slot, out.signals.entry_short);
        assign_bool(program.exit_short_slot, out.signals.exit_short);
        assign_bool(program.allow_trade_slot, out.signals.allow_trade, true);
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
