#include "app_support/feature_dump.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "engine/plan_compiler.h"
#include "engine/program_evaluator_internal.h"
#include "engine/plan_compiler_internal.h"
#include "op_registry_internal.h"
#include "ops/plan_spec_runtime_alias.h"
#include "ops/op_registry.h"
#include "ingest/data_view.h"

namespace sugar::app {

    namespace {

        std::string format_numeric_cell(double value) {
            if (!std::isfinite(value)) {
                return "nan";
            }
            std::ostringstream oss;
            oss << std::setprecision(17) << value;
            return oss.str();
        }

        std::string trim_copy(std::string text) {
            auto is_space = [](unsigned char ch) {
                return std::isspace(ch) != 0;
            };
            while (!text.empty() && is_space(static_cast<unsigned char>(text.front()))) {
                text.erase(text.begin());
            }
            while (!text.empty() && is_space(static_cast<unsigned char>(text.back()))) {
                text.pop_back();
            }
            return text;
        }

        std::optional<ParamValue> find_plan_param(
            const ParamMap& params,
            const std::string& name) {
            auto it = params.find(name);
            if (it == params.end()) return std::nullopt;
            return it->second;
        }

        bool resolve_param_to_string(
            const ParamValue& value,
            const ParamMap& plan_params,
            std::string& out,
            std::string& err) {
            if (std::holds_alternative<std::string>(value)) {
                out = std::get<std::string>(value);
                return true;
            }
            if (std::holds_alternative<ParamRef>(value)) {
                const auto& ref = std::get<ParamRef>(value).name;
                const auto resolved = find_plan_param(plan_params, ref);
                if (!resolved.has_value()) {
                    out = ref;
                    return true;
                }
                if (std::holds_alternative<std::string>(*resolved)) {
                    out = std::get<std::string>(*resolved);
                    return true;
                }
                err = "param ref '" + ref + "' did not resolve to string";
                return false;
            }
            err = "expected string or {ref:...}";
            return false;
        }

        bool resolve_node_string_param(
            const NodeSpec& node,
            const ParamMap& plan_params,
            const std::string& name,
            std::string& out,
            std::string& err) {
            auto it = node.params.find(name);
            if (it == node.params.end()) {
                err = "missing param '" + name + "'";
                return false;
            }
            if (!resolve_param_to_string(it->second, plan_params, out, err)) {
                err = "param '" + name + "': " + err;
                return false;
            }
            return true;
        }

        bool split_column_token(
            const std::string& token,
            const std::string& default_series,
            std::string& out_series,
            std::string& out_column) {
            const auto pos = token.find(':');
            if (pos == std::string::npos) {
                out_series = default_series;
                out_column = trim_copy(token);
            }
            else {
                out_series = trim_copy(token.substr(0, pos));
                out_column = trim_copy(token.substr(pos + 1));
            }
            return !out_series.empty() && !out_column.empty();
        }

        bool resolve_column_token(
            const std::string& token,
            const std::string& default_series,
            const DataView& dv,
            ColumnRef& out,
            std::string& err) {
            if (!dv.bundle().schema) {
                err = "runtime schema is missing";
                return false;
            }

            std::string series;
            std::string column;
            if (!split_column_token(token, default_series, series, column)) {
                err = "invalid column ref token '" + token + "'";
                return false;
            }

            ColumnId id{};
            if (!dv.bundle().schema->columns().find(column, id)) {
                err = "unknown column '" + column + "' in token '" + token + "'";
                return false;
            }

            out = ColumnRef{ std::move(series), id };
            if (!dv.has_col(out)) {
                err = "missing DataView column for token '" + token + "'";
                return false;
            }
            return true;
        }

        std::string column_display_label(
            const ColumnRef& ref,
            const std::string& default_series,
            const DataView& dv) {
            const auto& reg = dv.bundle().schema->columns();
            const std::string column = ref.col.v < reg.size()
                ? reg.name_by_id(ref.col)
                : "col#" + std::to_string(ref.col.v);
            if (ref.series == default_series) return column;
            return ref.series + ":" + column;
        }

        std::string primary_column_label(
            const ResolvedRuntimeInput& runtime,
            const DataView& dv) {
            if (!dv.bundle().schema) {
                return "col#" + std::to_string(runtime.primary_ref.col.v);
            }
            const auto& reg = dv.bundle().schema->columns();
            if (runtime.primary_ref.col.v >= reg.size()) {
                return "col#" + std::to_string(runtime.primary_ref.col.v);
            }
            return reg.name_by_id(runtime.primary_ref.col);
        }

        bool populate_numeric_leaf_env(
            const DataView& dv,
            const std::string& default_series,
            CompileEnv& env,
            std::string& err) {
            if (!dv.bundle().schema) {
                err = "runtime schema is missing";
                return false;
            }

            auto storage = make_compile_env_storage();
            const auto& reg = dv.bundle().schema->columns();
            for (std::size_t si = 0;
                 si < dv.bundle().series_keys.size() && si < dv.bundle().frames.size();
                 ++si) {
                const auto& series_key = dv.bundle().series_keys[si];
                const auto& frame = dv.bundle().frames[si];
                for (std::size_t ci = 0; ci < reg.size(); ++ci) {
                    ColumnId id{ static_cast<std::uint32_t>(ci) };
                    if (!frame.has_col(id)) continue;

                    ColumnRef ref{ series_key, id };
                    const auto col = dv.col(ref);
                    std::vector<double> values(col.begin(), col.end());
                    const std::string column_name = reg.name_by_id(id);
                    storage->numeric_leaf_values[series_key + ":" + column_name] = values;
                    if (series_key == default_series) {
                        storage->numeric_leaf_values[column_name] = std::move(values);
                    }
                }
            }
            env.storage = std::move(storage);
            return true;
        }

        bool build_feature_dump_compile_spec(
            const PlanSpec& spec,
            const DataView& dv,
            const std::string& default_series,
            PlanSpec& out_spec,
            CompileEnv& out_env,
            std::string& err) {
            out_spec = spec;
            for (auto& node : out_spec.numeric_nodes) {
                node.op = normalize_plan_spec_runtime_op(node.op);
            }
            for (auto& node : out_spec.bool_nodes) {
                node.op = normalize_plan_spec_runtime_op(node.op);
            }

            if (!populate_numeric_leaf_env(dv, default_series, out_env, err)) {
                return false;
            }

            for (auto& node : out_spec.numeric_nodes) {
                if (node.op != "core.ref") continue;

                std::string token;
                if (!resolve_node_string_param(node, spec.params, "ref", token, err)) {
                    err = "node '" + node.id + "': " + err;
                    return false;
                }

                ColumnRef ref{};
                if (!resolve_column_token(token, default_series, dv, ref, err)) {
                    err = "node '" + node.id + "': " + err;
                    return false;
                }

                node.op = "core.precomputed_num";
                node.args.clear();
                node.params.clear();
                node.params["name"] = ref.series + ":" +
                    dv.bundle().schema->columns().name_by_id(ref.col);
            }
            return true;
        }

        std::string display_label_for_column_token(
            const std::string& token,
            const std::string& default_series,
            const DataView& dv) {
            ColumnRef ref{};
            std::string err;
            if (!resolve_column_token(token, default_series, dv, ref, err)) {
                return token;
            }
            return column_display_label(ref, default_series, dv);
        }

        std::string feature_header_label(
            const NamedNumericOutput& feature,
            const NodeSpec* original_node,
            const ParamMap& plan_params,
            const ResolvedRuntimeInput& runtime,
            const DataView& dv) {
            if (original_node == nullptr) return feature.node_id;

            const std::string op = normalize_plan_spec_runtime_op(original_node->op);
            const std::string& default_series = runtime.primary_ref.series;
            if (op == "core.close") {
                return primary_column_label(runtime, dv);
            }
            if (op == "core.ref") {
                std::string token;
                std::string err;
                if (resolve_node_string_param(*original_node, plan_params, "ref", token, err)) {
                    return display_label_for_column_token(token, default_series, dv);
                }
            }
            if (op == "core.precomputed_num") {
                std::string token;
                std::string err;
                if (resolve_node_string_param(*original_node, plan_params, "name", token, err)) {
                    return display_label_for_column_token(token, default_series, dv);
                }
            }
            return feature.node_id;
        }

        std::vector<std::string> build_feature_header_labels(
            const PlanSpec& spec,
            const std::vector<NamedNumericOutput>& numeric_outputs,
            const ResolvedRuntimeInput& runtime,
            const DataView& dv,
            bool has_time_axis) {
            std::unordered_map<std::string, const NodeSpec*> nodes_by_id;
            nodes_by_id.reserve(spec.numeric_nodes.size());
            for (const auto& node : spec.numeric_nodes) {
                nodes_by_id.emplace(node.id, &node);
            }

            std::unordered_set<std::string> used{
                "series_key",
                "entity_key",
                "row_index",
            };
            if (has_time_axis) used.insert("time_key");

            std::vector<std::string> labels;
            labels.reserve(numeric_outputs.size());
            for (const auto& feature : numeric_outputs) {
                const NodeSpec* original_node = nullptr;
                if (auto it = nodes_by_id.find(feature.node_id); it != nodes_by_id.end()) {
                    original_node = it->second;
                }

                std::string label = feature_header_label(
                    feature,
                    original_node,
                    spec.params,
                    runtime,
                    dv);
                if (label.empty()) label = feature.node_id;

                std::string unique = label;
                if (!used.insert(unique).second) {
                    unique = label + "__" + feature.node_id;
                    std::size_t suffix = 2;
                    while (!used.insert(unique).second) {
                        unique = label + "__" + feature.node_id + "_" + std::to_string(suffix++);
                    }
                }
                labels.push_back(std::move(unique));
            }
            return labels;
        }

    } // namespace

    bool dump_feature_table(
        const sugar::ResolvedRuntimeInput& runtime,
        const sugar::PlanSpec& spec,
        std::ostream& out,
        std::string& err) {
        if (!runtime.aligned_bundle) {
            err = "feature-dump requires an aligned runtime bundle";
            return false;
        }
        if (runtime.primary_series_index == static_cast<std::size_t>(-1) ||
            runtime.primary_series_index >= runtime.aligned_bundle->frames.size()) {
            err = "feature-dump primary series index is invalid";
            return false;
        }

        const sugar::DataView dv(*runtime.aligned_bundle);

        sugar::PlanSpec compile_spec{};
        sugar::CompileEnv compile_env{};
        std::string adapt_err;
        if (!build_feature_dump_compile_spec(
                spec,
                dv,
                runtime.primary_ref.series,
                compile_spec,
                compile_env,
                adapt_err)) {
            err = "feature-dump compile failed: " + adapt_err;
            return false;
        }

        const auto registry = sugar::make_default_registry();
        sugar::CompiledPlanTemplate plan_template{};
        std::vector<sugar::NamedNumericOutput> numeric_outputs;
        std::string compile_err;
        if (!sugar::compile_feature_dump_template(
                compile_spec,
                registry,
                compile_env,
                plan_template,
                numeric_outputs,
                compile_err)) {
            err = "feature-dump compile failed: " + compile_err;
            return false;
        }

        sugar::CompileResult compiled{};
        std::string instantiate_err;
        if (!sugar::instantiate_compiled_plan(plan_template, nullptr, compiled, instantiate_err)) {
            err = "feature-dump instantiate failed: " + instantiate_err;
            return false;
        }

        sugar::ProgramEvaluationBuffers buffers{};
        std::string eval_err;
        if (!sugar::evaluate_strategy_plan_program_buffers(
                dv,
                runtime.primary_ref,
                compiled.plan,
                buffers,
                eval_err)) {
            err = "feature-dump evaluation failed: " + eval_err;
            return false;
        }

        const auto& frame = runtime.aligned_bundle->frames[runtime.primary_series_index];
        const bool has_time_axis = frame.has_time_axis();
        const auto time_axis = frame.time();
        const std::string entity_key =
            runtime.primary_entity_key.empty() ? runtime.primary_ref.series : runtime.primary_entity_key;
        const std::vector<std::string> header_labels =
            build_feature_header_labels(spec, numeric_outputs, runtime, dv, has_time_axis);

        out << "series_key,entity_key,row_index";
        if (has_time_axis) {
            out << ",time_key";
        }
        for (const auto& label : header_labels) {
            out << "," << label;
        }
        out << "\n";

        for (std::size_t row = 0; row < frame.row_count(); ++row) {
            out << runtime.primary_ref.series
                << "," << entity_key
                << "," << row;
            if (has_time_axis) {
                out << "," << time_axis[row];
            }
            for (const auto& feature : numeric_outputs) {
                if (feature.output_slot >= buffers.numeric_slots.size() ||
                    row >= buffers.numeric_slots[feature.output_slot].size()) {
                    err = "feature-dump numeric output slot is out of range";
                    return false;
                }
                out << "," << format_numeric_cell(buffers.numeric_slots[feature.output_slot][row]);
            }
            out << "\n";
        }

        return true;
    }

} // namespace sugar::app
