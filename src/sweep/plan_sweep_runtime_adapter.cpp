#include "plan_sweep_runtime_adapter.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "engine/program_evaluator_internal.h"
#include "engine/strategy_executor.h"
#include "ingest/run_pipeline.h"
#include "op_registry_internal.h"
#include "ops/op_registry.h"
#include "sweep/metrics.h"

namespace sugar {

    namespace {
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
                const double summarized =
                    summarize_numeric_output(slot, NumericNodeSummaryMode::LastFiniteValue);
                result_out.reported_values.push_back(
                    NamedReportedValue{ output.node_id, summarized });
            }
        }

        std::string trim_copy(std::string s) {
            auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
            while (!s.empty() && is_space(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
            while (!s.empty() && is_space(static_cast<unsigned char>(s.back()))) s.pop_back();
            return s;
        }

        bool series_alias_and_col(
            const std::string& token,
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

        bool resolve_col_ref_token(
            const std::string& token,
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

        std::optional<ParamValue> find_param_value(
            const ParamMap& plan_params,
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

        bool resolve_param_to_string(
            const ParamValue& v,
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

        bool get_node_param(
            const NodeSpec& node,
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

            auto ensure_leaf_node_for_token =
                [&](const std::string& token, std::string& out_id) -> bool {
                    std::string key;
                    std::string e;
                    if (!resolve_token_to_leaf_key(token, default_series, dv, key, e)) {
                        err = e;
                        return false;
                    }
                    if (auto it = leaf_key_to_node_id.find(key);
                        it != leaf_key_to_node_id.end()) {
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
                else if ((node.op == "ms.diff" || node.op == "ms.ratio") &&
                         node.args.empty()) {
                    ParamValue pa{};
                    ParamValue pb{};
                    if (!get_node_param(src, "a", pa) ||
                        !get_node_param(src, "b", pb)) {
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

            for (std::size_t si = 0;
                 si < dv.bundle().series_keys.size() && si < dv.bundle().frames.size();
                 ++si) {
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
                    const std::string column_name = reg.name_by_id(id);
                    storage->numeric_leaf_values[leaf_key] = values;
                    if (skey == default_series) {
                        storage->numeric_leaf_values.emplace(column_name, std::move(values));
                    }
                }
            }

            return true;
        }
    } // namespace

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

} // namespace sugar
