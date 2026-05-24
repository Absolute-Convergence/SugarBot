#include "engine/plan_compiler.h"
#include "op_registry_internal.h"
#include "plan_compiler_internal.h"
#include "engine/program_evaluator.h"
#include "strategy_plan_internal.h"
#include "sweep/dynamic_execution.h"

#include <algorithm>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>

namespace sugar {

    namespace {
        std::size_t w_max(const std::vector<std::size_t>& v) {
            std::size_t m = 0;
            for (auto x : v) m = std::max(m, x);
            return m;
        }

        std::string cycle_path_text(const std::vector<std::string>& stack, const std::string& id) {
            std::ostringstream oss;
            auto it = std::find(stack.begin(), stack.end(), id);
            if (it == stack.end()) {
                oss << id << " -> " << id;
                return oss.str();
            }
            for (auto p = it; p != stack.end(); ++p) {
                if (p != it) oss << " -> ";
                oss << *p;
            }
            oss << " -> " << id;
            return oss.str();
        }

        std::string available_leaf_key_hint(const CompileEnvStorage* storage, bool numeric) {
            if (!storage) return {};
            std::vector<std::string> keys;
            if (numeric) {
                keys.reserve(storage->numeric_leaf_values.size());
                for (const auto& kv : storage->numeric_leaf_values) {
                    keys.push_back(kv.first);
                }
            }
            else {
                keys.reserve(storage->bool_leaf_values.size());
                for (const auto& kv : storage->bool_leaf_values) {
                    keys.push_back(kv.first);
                }
            }
            if (keys.empty()) return {};
            std::sort(keys.begin(), keys.end());
            constexpr std::size_t kLimit = 8;
            std::ostringstream oss;
            oss << " (available: ";
            const std::size_t count = std::min(keys.size(), kLimit);
            for (std::size_t i = 0; i < count; ++i) {
                if (i) oss << ", ";
                oss << keys[i];
            }
            if (keys.size() > kLimit) {
                oss << ", ...";
            }
            oss << ")";
            return oss.str();
        }

        std::string unknown_operator_compile_hint(const std::string& op_name) {
            if (op_name == "core.param") {
                return "; use core.const with params.value={\"ref\":\"param_name\"} "
                    "to expose a PlanSpec or sweep parameter as a numeric node";
            }
            return {};
        }

        class VisitGuard {
        public:
            VisitGuard(std::unordered_set<std::string>& visiting,
                std::vector<std::string>& stack,
                const std::string& id,
                const char* graph_name,
                std::string& err)
                : visiting_(visiting), stack_(stack), id_(id)
            {
                if (visiting_.find(id_) != visiting_.end()) {
                    err = std::string("Cycle detected in ") + graph_name
                        + ": " + cycle_path_text(stack_, id_);
                    return;
                }
                visiting_.insert(id_);
                stack_.push_back(id_);
                active_ = true;
            }

            ~VisitGuard() {
                if (!active_) return;
                if (!stack_.empty()) stack_.pop_back();
                visiting_.erase(id_);
            }

            explicit operator bool() const { return active_; }

        private:
            std::unordered_set<std::string>& visiting_;
            std::vector<std::string>& stack_;
            std::string id_;
            bool active_{ false };
        };

        struct TemplateBuildCtx {
            const PlanSpec& spec;
            const OperatorRegistry& reg;
            CompiledPlanTemplate* out{ nullptr };

            std::unordered_map<std::string, const NodeSpec*> num_nodes;
            std::unordered_map<std::string, const NodeSpec*> bool_nodes;

            std::unordered_map<std::string, std::size_t> num_slot_cache;
            std::unordered_map<std::string, std::size_t> bool_slot_cache;

            std::unordered_set<std::string> num_build_visiting;
            std::unordered_set<std::string> bool_build_visiting;
            std::vector<std::string> num_build_stack;
            std::vector<std::string> bool_build_stack;

            TemplateBuildCtx(const PlanSpec& s,
                const OperatorRegistry& r,
                CompiledPlanTemplate& tpl)
                : spec(s), reg(r), out(&tpl) {
            }
        };

        bool build_numeric_template(TemplateBuildCtx& ctx, const std::string& id, std::size_t& out, std::string& err);
        bool build_bool_template(TemplateBuildCtx& ctx, const std::string& id, std::size_t& out, std::string& err);

        bool resolve_template_params(const OpEntry& op,
            const std::string& node_id,
            const ParamMap& node_params,
            const ParamMap& plan_params,
            const ParamMap* overrides,
            const CompileEnv& env,
            ResolvedParamMap& out,
            std::string& err) {
            if (!resolve_and_validate_params(op, node_params, plan_params, overrides, out, err)) {
                err = "node '" + node_id + "' (" + op.spec.name + "): " + err;
                return false;
            }

            const CompileEnvStorage* storage = compile_env_storage(env);
            if (op.spec.name == "core.precomputed_num") {
                const auto key = as_str(out, "name").value_or(std::string{});
                if (!storage || storage->numeric_leaf_values.find(key) == storage->numeric_leaf_values.end()) {
                    err = "node '" + node_id + "': precomputed numeric leaf '" + key +
                        "' not found in CompileEnv" + available_leaf_key_hint(storage, true) +
                        "; for CSV columns use the exact column name from the layout's value_cols or an explicit series:column name";
                    return false;
                }
            }
            if (op.spec.name == "core.precomputed_bool") {
                const auto key = as_str(out, "name").value_or(std::string{});
                if (!storage || storage->bool_leaf_values.find(key) == storage->bool_leaf_values.end()) {
                    err = "node '" + node_id + "': precomputed bool leaf '" + key +
                        "' not found in CompileEnv" + available_leaf_key_hint(storage, false);
                    return false;
                }
            }
            return true;
        }

        bool build_numeric_template(TemplateBuildCtx& ctx, const std::string& id, std::size_t& out, std::string& err) {
            if (auto it = ctx.num_slot_cache.find(id); it != ctx.num_slot_cache.end()) {
                out = it->second;
                return true;
            }
            VisitGuard visiting(ctx.num_build_visiting, ctx.num_build_stack, id, "PlanSpec numeric node graph", err);
            if (!visiting) return false;

            auto nit = ctx.num_nodes.find(id);
            if (nit == ctx.num_nodes.end()) {
                err = "Unknown numeric node id '" + id + "'";
                return false;
            }
            const NodeSpec& node = *nit->second;
            const OpEntry* op = ctx.reg.get(node.op);
            if (!op) {
                err = "Unknown op '" + node.op + "' for numeric node '" + id + "'" +
                    unknown_operator_compile_hint(node.op);
                return false;
            }
            if (op->spec.op_type != OpType::Numeric) {
                err = "Op '" + node.op + "' is not a numeric op; move bool operators to bool_nodes. "
                    "Numeric nodes can consume bool nodes only through numeric ops that declare bool inputs, such as core.select.";
                return false;
            }
            if (!compiled_program_supports_numeric_op(node.op)) {
                err = "Compiled-program backend does not support numeric op '" + node.op + "'";
                return false;
            }
            if (node.args.size() != op->spec.inputs.size()) {
                err = "Node '" + id + "' expected " + std::to_string(op->spec.inputs.size())
                    + " args, got " + std::to_string(node.args.size());
                return false;
            }

            NumericProgramTemplateNode compiled{};
            compiled.node_id = node.id;
            compiled.op = *op;
            compiled.node_params = node.params;
            compiled.output_slot = ctx.out->program.numeric_slot_count++;
            compiled.inputs.reserve(node.args.size());

            for (std::size_t i = 0; i < node.args.size(); ++i) {
                if (op->spec.inputs[i] == ValueType::Bool) {
                    std::size_t child = 0;
                    if (!build_bool_template(ctx, node.args[i], child, err)) return false;
                    compiled.inputs.push_back(ProgramInputRef{ ValueType::Bool, child });
                }
                else {
                    std::size_t child = 0;
                    if (!build_numeric_template(ctx, node.args[i], child, err)) return false;
                    compiled.inputs.push_back(ProgramInputRef{ ValueType::Num, child });
                }
            }

            ctx.out->numeric_nodes.push_back(std::move(compiled));
            ctx.out->program.evaluation_order.push_back(ProgramExecutionStep{
                ValueType::Num,
                ctx.out->numeric_nodes.size() - 1
            });
            out = ctx.out->numeric_nodes.back().output_slot;
            ctx.num_slot_cache[id] = out;
            return true;
        }

        bool build_bool_template(TemplateBuildCtx& ctx, const std::string& id, std::size_t& out, std::string& err) {
            if (auto it = ctx.bool_slot_cache.find(id); it != ctx.bool_slot_cache.end()) {
                out = it->second;
                return true;
            }
            VisitGuard visiting(ctx.bool_build_visiting, ctx.bool_build_stack, id, "PlanSpec bool node graph", err);
            if (!visiting) return false;

            auto bit = ctx.bool_nodes.find(id);
            if (bit == ctx.bool_nodes.end()) {
                err = "Unknown bool node id '" + id + "'";
                return false;
            }
            const NodeSpec& node = *bit->second;
            const OpEntry* op = ctx.reg.get(node.op);
            if (!op) {
                err = "Unknown op '" + node.op + "' for bool node '" + id + "'" +
                    unknown_operator_compile_hint(node.op);
                return false;
            }
            if (op->spec.op_type != OpType::Bool) {
                err = "Op '" + node.op + "' is not a bool op; move numeric operators to numeric_nodes.";
                return false;
            }
            if (!compiled_program_supports_bool_op(node.op)) {
                err = "Compiled-program backend does not support bool op '" + node.op + "'";
                return false;
            }
            if (node.args.size() != op->spec.inputs.size()) {
                err = "Node '" + id + "' expected " + std::to_string(op->spec.inputs.size())
                    + " args, got " + std::to_string(node.args.size());
                return false;
            }

            BoolProgramTemplateNode compiled{};
            compiled.node_id = node.id;
            compiled.op = *op;
            compiled.node_params = node.params;
            compiled.output_slot = ctx.out->program.bool_slot_count++;
            compiled.inputs.reserve(node.args.size());

            for (std::size_t i = 0; i < node.args.size(); ++i) {
                if (op->spec.inputs[i] == ValueType::Bool) {
                    std::size_t child = 0;
                    if (!build_bool_template(ctx, node.args[i], child, err)) return false;
                    compiled.inputs.push_back(ProgramInputRef{ ValueType::Bool, child });
                }
                else {
                    std::size_t child = 0;
                    if (!build_numeric_template(ctx, node.args[i], child, err)) return false;
                    compiled.inputs.push_back(ProgramInputRef{ ValueType::Num, child });
                }
            }

            ctx.out->bool_nodes.push_back(std::move(compiled));
            ctx.out->program.evaluation_order.push_back(ProgramExecutionStep{
                ValueType::Bool,
                ctx.out->bool_nodes.size() - 1
            });
            out = ctx.out->bool_nodes.back().output_slot;
            ctx.bool_slot_cache[id] = out;
            return true;
        }

        bool bind_bool_template(TemplateBuildCtx& ctx, const std::string& id, std::size_t& dst, std::string& err) {
            if (id.empty()) {
                dst = kInvalidProgramSlot;
                return true;
            }
            return build_bool_template(ctx, id, dst, err);
        }

        bool declared_dynamic_execution_enabled(const PlanSpec& spec) {
            return spec.execution.has_value() &&
                spec.execution->mode == PlanExecutionMode::DeclaredDynamic;
        }

        std::string dynamic_validation_issues_text(
            const PlanDynamicExecutionValidationReport& report) {
            std::ostringstream oss;
            for (std::size_t i = 0; i < report.issues.size(); ++i) {
                const auto& issue = report.issues[i];
                if (i) oss << "; ";
                oss << plan_dynamic_execution_validation_code_name(issue.code);
                if (!issue.subject.empty()) {
                    oss << "(" << issue.subject << ")";
                }
                if (!issue.detail.empty()) {
                    oss << ": " << issue.detail;
                }
            }
            return oss.str();
        }

        bool validate_declared_dynamic_execution_for_compile(
            const PlanSpec& spec,
            std::string& err) {
            const auto report = validate_plan_dynamic_execution(spec);
            if (report.valid) {
                return true;
            }
            err = "declared_dynamic execution validation failed: " +
                dynamic_validation_issues_text(report);
            return false;
        }

        bool build_any_template(
            TemplateBuildCtx& ctx,
            const std::string& id,
            CompiledDynamicNodeRef& out,
            std::string& err) {
            if (ctx.num_nodes.find(id) != ctx.num_nodes.end()) {
                std::size_t slot = 0;
                if (!build_numeric_template(ctx, id, slot, err)) {
                    return false;
                }
                out = CompiledDynamicNodeRef{ id, ValueType::Num, slot };
                return true;
            }
            if (ctx.bool_nodes.find(id) != ctx.bool_nodes.end()) {
                std::size_t slot = 0;
                if (!build_bool_template(ctx, id, slot, err)) {
                    return false;
                }
                out = CompiledDynamicNodeRef{ id, ValueType::Bool, slot };
                return true;
            }
            err = "Unknown dynamic execution node id '" + id + "'";
            return false;
        }

        std::optional<bool> literal_bool_const_value(
            const TemplateBuildCtx& ctx,
            const std::string& id) {
            auto it = ctx.bool_nodes.find(id);
            if (it == ctx.bool_nodes.end() || !it->second) {
                return std::nullopt;
            }
            const NodeSpec& node = *it->second;
            if (node.op != "core.bool_const") {
                return std::nullopt;
            }
            auto value = node.params.find("value");
            if (value == node.params.end()) {
                return std::nullopt;
            }
            if (const auto* b = std::get_if<bool>(&value->second)) {
                return *b;
            }
            return std::nullopt;
        }

        bool append_subgraph_indices(
            const std::vector<std::string>& ids,
            const std::unordered_map<std::string, std::size_t>& subgraph_indices,
            std::vector<std::size_t>& out,
            std::string& err) {
            out.clear();
            out.reserve(ids.size());
            for (const auto& id : ids) {
                const auto it = subgraph_indices.find(id);
                if (it == subgraph_indices.end()) {
                    err = "declared_dynamic execution references unknown subgraph '" + id + "'";
                    return false;
                }
                out.push_back(it->second);
            }
            return true;
        }

        void collect_subgraph_dependency_closure(
            std::size_t index,
            const std::vector<CompiledDynamicSubgraph>& subgraphs,
            std::unordered_set<std::size_t>& seen,
            std::vector<std::size_t>& out) {
            if (index >= subgraphs.size()) {
                return;
            }
            for (const auto dep : subgraphs[index].depends_on_subgraphs) {
                if (!seen.insert(dep).second) {
                    continue;
                }
                collect_subgraph_dependency_closure(dep, subgraphs, seen, out);
                out.push_back(dep);
            }
        }

        const std::string* find_execution_output_node(
            const PlanExecutionSpec& execution,
            const std::string& id) {
            for (const auto& output : execution.outputs) {
                if (output.id == id) {
                    return &output.node;
                }
            }
            return nullptr;
        }

        std::size_t find_dynamic_node_owner(
            const CompiledDynamicExecutionMetadata& metadata,
            const std::string& node_id) {
            for (std::size_t subgraph_index = 0;
                 subgraph_index < metadata.subgraphs.size();
                 ++subgraph_index) {
                const auto& subgraph = metadata.subgraphs[subgraph_index];
                const auto found = std::find_if(
                    subgraph.nodes.begin(),
                    subgraph.nodes.end(),
                    [&](const auto& node) {
                        return node.node_id == node_id;
                    });
                if (found != subgraph.nodes.end()) {
                    return subgraph_index;
                }
            }
            return kInvalidProgramSlot;
        }

        bool compile_dynamic_execution_metadata(
            TemplateBuildCtx& ctx,
            const PlanExecutionSpec& execution,
            CompiledProgram& program,
            std::string& err) {
            CompiledDynamicExecutionMetadata metadata{};
            metadata.mode = CompiledDynamicExecutionMode::DeclaredDynamic;
            metadata.enabled = true;
            metadata.constant_route_only = true;
            metadata.max_active_subgraphs = execution.bounds.max_active_subgraphs;
            metadata.decision_sample_limit = execution.bounds.decision_sample_limit;

            std::unordered_map<std::string, std::size_t> subgraph_indices;
            subgraph_indices.reserve(execution.subgraphs.size());
            for (std::size_t i = 0; i < execution.subgraphs.size(); ++i) {
                subgraph_indices.emplace(execution.subgraphs[i].id, i);
            }

            metadata.subgraphs.reserve(execution.subgraphs.size());
            for (const auto& subgraph : execution.subgraphs) {
                CompiledDynamicSubgraph compiled{};
                compiled.id = subgraph.id;
                compiled.nodes.reserve(subgraph.nodes.size());
                for (const auto& node_id : subgraph.nodes) {
                    CompiledDynamicNodeRef ref{};
                    if (!build_any_template(ctx, node_id, ref, err)) {
                        err = "declared_dynamic subgraph '" + subgraph.id + "': " + err;
                        return false;
                    }
                    compiled.nodes.push_back(std::move(ref));
                }
                metadata.subgraphs.push_back(std::move(compiled));
            }

            for (std::size_t i = 0; i < execution.subgraphs.size(); ++i) {
                if (!append_subgraph_indices(
                        execution.subgraphs[i].depends_on,
                        subgraph_indices,
                        metadata.subgraphs[i].depends_on_subgraphs,
                        err)) {
                    err = "declared_dynamic subgraph '" + execution.subgraphs[i].id + "': " + err;
                    return false;
                }
            }
            for (std::size_t i = 0; i < metadata.subgraphs.size(); ++i) {
                std::unordered_set<std::size_t> seen;
                collect_subgraph_dependency_closure(
                    i,
                    metadata.subgraphs,
                    seen,
                    metadata.subgraphs[i].dependency_closure_subgraphs);
            }

            metadata.routes.reserve(execution.routes.size());
            for (const auto& route : execution.routes) {
                CompiledDynamicRoute compiled{};
                compiled.id = route.id;
                compiled.condition_node_id = route.condition;
                if (!build_bool_template(ctx, route.condition, compiled.condition_slot, err)) {
                    err = "declared_dynamic route '" + route.id + "': " + err;
                    return false;
                }
                const auto constant_condition =
                    literal_bool_const_value(ctx, route.condition);
                if (constant_condition.has_value()) {
                    compiled.condition_is_constant = true;
                    compiled.constant_condition_value = *constant_condition;
                }
                else {
                    metadata.constant_route_only = false;
                }

                compiled.cases.reserve(route.cases.size());
                const std::vector<std::string>* selected_subgraphs =
                    &route.fallback_subgraphs;
                for (const auto& route_case : route.cases) {
                    CompiledDynamicRouteCase compiled_case{};
                    compiled_case.id = route_case.id;
                    compiled_case.when = route_case.when;
                    if (!append_subgraph_indices(
                            route_case.activate_subgraphs,
                            subgraph_indices,
                            compiled_case.activate_subgraphs,
                            err)) {
                        err = "declared_dynamic route case '" + route_case.id + "': " + err;
                        return false;
                    }
                    if (constant_condition.has_value() &&
                        route_case.when == *constant_condition &&
                        selected_subgraphs == &route.fallback_subgraphs) {
                        selected_subgraphs = &route_case.activate_subgraphs;
                    }
                    compiled.cases.push_back(std::move(compiled_case));
                }
                if (!append_subgraph_indices(
                        route.fallback_subgraphs,
                        subgraph_indices,
                        compiled.fallback_subgraphs,
                        err)) {
                    err = "declared_dynamic route '" + route.id + "': " + err;
                    return false;
                }
                if (constant_condition.has_value()) {
                    if (!append_subgraph_indices(
                            *selected_subgraphs,
                            subgraph_indices,
                            compiled.selected_subgraphs,
                            err)) {
                        err = "declared_dynamic route '" + route.id + "': " + err;
                        return false;
                    }
                }
                metadata.routes.push_back(std::move(compiled));
            }

            metadata.outputs.reserve(execution.outputs.size());
            for (const auto& output : execution.outputs) {
                CompiledDynamicNodeRef ref{};
                if (!build_any_template(ctx, output.node, ref, err)) {
                    err = "declared_dynamic output '" + output.id + "': " + err;
                    return false;
                }
                const std::size_t owning_subgraph =
                    find_dynamic_node_owner(metadata, output.node);
                if (owning_subgraph == kInvalidProgramSlot) {
                    err = "declared_dynamic output '" + output.id +
                        "' references node '" + output.node +
                        "' without a declared owning subgraph";
                    return false;
                }
                metadata.outputs.push_back(CompiledDynamicOutput{
                    output.id,
                    output.node,
                    ProgramInputRef{ ref.type, ref.slot },
                    owning_subgraph,
                });
            }

            program.dynamic_execution = std::move(metadata);
            return true;
        }

        bool bind_static_strategy_surface(
            TemplateBuildCtx& ctx,
            CompiledProgram& program,
            std::string& err) {
            if (!bind_bool_template(ctx, ctx.spec.bind.entry_long, program.entry_long_slot, err)) return false;
            if (!bind_bool_template(ctx, ctx.spec.bind.exit_long, program.exit_long_slot, err)) return false;
            if (!bind_bool_template(ctx, ctx.spec.bind.entry_short, program.entry_short_slot, err)) return false;
            if (!bind_bool_template(ctx, ctx.spec.bind.exit_short, program.exit_short_slot, err)) return false;
            if (!bind_bool_template(ctx, ctx.spec.bind.allow_trade, program.allow_trade_slot, err)) return false;
            return true;
        }

        bool build_declared_dynamic_binding_surface(
            TemplateBuildCtx& ctx,
            std::string& err) {
            std::size_t ignored = kInvalidProgramSlot;
            if (!bind_bool_template(ctx, ctx.spec.bind.entry_long, ignored, err)) return false;
            if (!bind_bool_template(ctx, ctx.spec.bind.exit_long, ignored, err)) return false;
            if (!bind_bool_template(ctx, ctx.spec.bind.entry_short, ignored, err)) return false;
            if (!bind_bool_template(ctx, ctx.spec.bind.exit_short, ignored, err)) return false;
            if (!bind_bool_template(ctx, ctx.spec.bind.allow_trade, ignored, err)) return false;
            return true;
        }

        bool bind_declared_dynamic_output(
            TemplateBuildCtx& ctx,
            const PlanExecutionSpec& execution,
            const std::string& role,
            const std::string& role_binding,
            std::size_t& dst,
            std::string& err) {
            if (role_binding.empty()) {
                dst = kInvalidProgramSlot;
                return true;
            }
            const std::string* output_node = find_execution_output_node(execution, role);
            if (!output_node) {
                err = "declared_dynamic execution output missing for active strategy binding '" + role + "'";
                return false;
            }
            if (ctx.bool_nodes.find(*output_node) == ctx.bool_nodes.end()) {
                err = "declared_dynamic execution output '" + role +
                    "' must reference a bool node; got '" + *output_node + "'";
                return false;
            }
            return build_bool_template(ctx, *output_node, dst, err);
        }

        bool bind_declared_dynamic_strategy_outputs(
            TemplateBuildCtx& ctx,
            const PlanExecutionSpec& execution,
            CompiledProgram& program,
            std::string& err) {
            if (!bind_declared_dynamic_output(ctx, execution, "entry_long", ctx.spec.bind.entry_long, program.entry_long_slot, err)) return false;
            if (!bind_declared_dynamic_output(ctx, execution, "exit_long", ctx.spec.bind.exit_long, program.exit_long_slot, err)) return false;
            if (!bind_declared_dynamic_output(ctx, execution, "entry_short", ctx.spec.bind.entry_short, program.entry_short_slot, err)) return false;
            if (!bind_declared_dynamic_output(ctx, execution, "exit_short", ctx.spec.bind.exit_short, program.exit_short_slot, err)) return false;
            if (!bind_declared_dynamic_output(ctx, execution, "allow_trade", ctx.spec.bind.allow_trade, program.allow_trade_slot, err)) return false;
            return true;
        }

        void initialize_plan_template(
            const PlanSpec& spec,
            const CompileEnv& env,
            CompiledPlanTemplate& out) {
            out = CompiledPlanTemplate{};
            out.name = spec.name;
            out.mode = spec.trade_mode;
            out.exec = spec.exec;
            out.plan_params = spec.params;
            out.env = env;
        }

        bool seed_template_ctx(TemplateBuildCtx& ctx, std::string& err) {
            for (const auto& n : ctx.spec.numeric_nodes) {
                if (n.id.empty()) {
                    err = "numeric node missing id";
                    return false;
                }
                if (!ctx.num_nodes.emplace(n.id, &n).second) {
                    err = "duplicate numeric node id: " + n.id;
                    return false;
                }
            }
            for (const auto& n : ctx.spec.bool_nodes) {
                if (n.id.empty()) {
                    err = "bool node missing id";
                    return false;
                }
                if (!ctx.bool_nodes.emplace(n.id, &n).second) {
                    err = "duplicate bool node id: " + n.id;
                    return false;
                }
            }
            return true;
        }

        std::size_t resolve_execution_first_valid_index(
            const CompiledProgram& program,
            const std::vector<std::size_t>& bool_warmups) {
            std::size_t first_valid = 0;
            auto bump = [&](std::size_t slot) {
                if (slot == kInvalidProgramSlot) return;
                if (slot < bool_warmups.size()) {
                    first_valid = std::max(first_valid, bool_warmups[slot]);
                }
            };

            // Strategy execution only consumes the bound bool signal surface.
            // Keep the warmup boundary scoped to those bound outputs so unused
            // feature branches do not silently delay execution.
            bump(program.entry_long_slot);
            bump(program.exit_long_slot);
            bump(program.entry_short_slot);
            bump(program.exit_short_slot);
            bump(program.allow_trade_slot);
            if (program.dynamic_execution.enabled) {
                for (const auto& route : program.dynamic_execution.routes) {
                    bump(route.condition_slot);
                }
            }
            return first_valid;
        }

    } // namespace

    bool compile_plan_template(const PlanSpec& spec,
        const OperatorRegistry& registry,
        const CompileEnv& env,
        CompiledPlanTemplate& out,
        std::string& err)
    {
        initialize_plan_template(spec, env, out);

        TemplateBuildCtx ctx{ spec, registry, out };
        if (!seed_template_ctx(ctx, err)) return false;

        if (declared_dynamic_execution_enabled(spec)) {
            if (!validate_declared_dynamic_execution_for_compile(spec, err)) return false;
            if (!build_declared_dynamic_binding_surface(ctx, err)) return false;
            if (!compile_dynamic_execution_metadata(ctx, *spec.execution, out.program, err)) return false;
            if (!bind_declared_dynamic_strategy_outputs(ctx, *spec.execution, out.program, err)) return false;
            return true;
        }

        if (!bind_static_strategy_surface(ctx, out.program, err)) return false;

        return true;
    }

    bool compile_plan_template_with_numeric_outputs(const PlanSpec& spec,
        const OperatorRegistry& registry,
        const CompileEnv& env,
        CompiledPlanTemplate& out,
        std::vector<NamedNumericOutput>& numeric_outputs,
        std::string& err) {
        initialize_plan_template(spec, env, out);
        numeric_outputs.clear();

        TemplateBuildCtx ctx{ spec, registry, out };
        if (!seed_template_ctx(ctx, err)) return false;

        numeric_outputs.reserve(spec.numeric_nodes.size());
        for (const auto& node : spec.numeric_nodes) {
            std::size_t output_slot = 0;
            if (!build_numeric_template(ctx, node.id, output_slot, err)) {
                err = "numeric output '" + node.id + "': " + err;
                return false;
            }
            numeric_outputs.push_back(NamedNumericOutput{ node.id, output_slot });
        }

        if (declared_dynamic_execution_enabled(spec)) {
            if (!validate_declared_dynamic_execution_for_compile(spec, err)) return false;
            if (!build_declared_dynamic_binding_surface(ctx, err)) return false;
            if (!compile_dynamic_execution_metadata(ctx, *spec.execution, out.program, err)) return false;
            if (!bind_declared_dynamic_strategy_outputs(ctx, *spec.execution, out.program, err)) return false;
            return true;
        }

        if (!bind_static_strategy_surface(ctx, out.program, err)) return false;

        return true;
    }

    bool compile_feature_dump_template(const PlanSpec& spec,
        const OperatorRegistry& registry,
        const CompileEnv& env,
        CompiledPlanTemplate& out,
        std::vector<NamedNumericOutput>& numeric_outputs,
        std::string& err) {
        if (spec.numeric_nodes.empty()) {
            err = "feature-dump requires at least one numeric node";
            return false;
        }
        return compile_plan_template_with_numeric_outputs(spec, registry, env, out, numeric_outputs, err);
    }

    bool instantiate_compiled_plan(const CompiledPlanTemplate& plan_template,
        const ParamMap* overrides,
        CompileResult& out,
        std::string& err)
    {
        auto storage = std::make_shared<StrategyPlanStorage>();
        storage->env = plan_template.env.storage;
        storage->program.output_shape = plan_template.program.output_shape;
        storage->program.kernel_access = plan_template.program.kernel_access;
        storage->program.scheduling = plan_template.program.scheduling;
        storage->program.numeric_slot_count = plan_template.program.numeric_slot_count;
        storage->program.bool_slot_count = plan_template.program.bool_slot_count;
        storage->program.evaluation_order = plan_template.program.evaluation_order;
        storage->program.entry_long_slot = plan_template.program.entry_long_slot;
        storage->program.exit_long_slot = plan_template.program.exit_long_slot;
        storage->program.entry_short_slot = plan_template.program.entry_short_slot;
        storage->program.exit_short_slot = plan_template.program.exit_short_slot;
        storage->program.allow_trade_slot = plan_template.program.allow_trade_slot;
        storage->program.dynamic_execution = plan_template.program.dynamic_execution;
        storage->program.numeric_nodes.reserve(plan_template.numeric_nodes.size());
        storage->program.bool_nodes.reserve(plan_template.bool_nodes.size());

        std::vector<std::size_t> numeric_warmups(plan_template.program.numeric_slot_count, 0);
        std::vector<std::size_t> bool_warmups(plan_template.program.bool_slot_count, 0);

        for (const auto& node : plan_template.numeric_nodes) {
            ResolvedParamMap params;
            if (!resolve_template_params(node.op,
                node.node_id,
                node.node_params,
                plan_template.plan_params,
                overrides,
                plan_template.env,
                params,
                err)) {
                return false;
            }

            NumericProgramNode compiled{};
            compiled.op_name = node.op.spec.name;
            compiled.inputs = node.inputs;
            compiled.params = params;
            compiled.output_slot = node.output_slot;
            storage->program.numeric_nodes.push_back(std::move(compiled));

            std::vector<std::size_t> child_warmups;
            child_warmups.reserve(node.inputs.size());
            for (const auto& input : node.inputs) {
                if (input.type == ValueType::Bool) {
                    if (input.slot >= bool_warmups.size()) {
                        err = "node '" + node.node_id + "': bool input slot out of range";
                        return false;
                    }
                    child_warmups.push_back(bool_warmups[input.slot]);
                }
                else {
                    if (input.slot >= numeric_warmups.size()) {
                        err = "node '" + node.node_id + "': numeric input slot out of range";
                        return false;
                    }
                    child_warmups.push_back(numeric_warmups[input.slot]);
                }
            }

            std::size_t warmup = node.op.warmup_num
                ? node.op.warmup_num(child_warmups, storage->program.numeric_nodes.back().params)
                : w_max(child_warmups);
            if (node.output_slot >= numeric_warmups.size()) {
                err = "node '" + node.node_id + "': numeric output slot out of range";
                return false;
            }
            numeric_warmups[node.output_slot] = warmup;
        }

        for (const auto& node : plan_template.bool_nodes) {
            ResolvedParamMap params;
            if (!resolve_template_params(node.op,
                node.node_id,
                node.node_params,
                plan_template.plan_params,
                overrides,
                plan_template.env,
                params,
                err)) {
                return false;
            }

            BoolProgramNode compiled{};
            compiled.op_name = node.op.spec.name;
            compiled.inputs = node.inputs;
            compiled.params = params;
            compiled.output_slot = node.output_slot;
            storage->program.bool_nodes.push_back(std::move(compiled));

            std::vector<std::size_t> bool_child_warmups;
            std::vector<std::size_t> num_child_warmups;
            bool_child_warmups.reserve(node.inputs.size());
            num_child_warmups.reserve(node.inputs.size());
            for (const auto& input : node.inputs) {
                if (input.type == ValueType::Bool) {
                    if (input.slot >= bool_warmups.size()) {
                        err = "node '" + node.node_id + "': bool input slot out of range";
                        return false;
                    }
                    bool_child_warmups.push_back(bool_warmups[input.slot]);
                }
                else {
                    if (input.slot >= numeric_warmups.size()) {
                        err = "node '" + node.node_id + "': numeric input slot out of range";
                        return false;
                    }
                    num_child_warmups.push_back(numeric_warmups[input.slot]);
                }
            }

            std::size_t warmup = node.op.warmup_bool
                ? node.op.warmup_bool(bool_child_warmups, num_child_warmups, storage->program.bool_nodes.back().params)
                : std::max(w_max(bool_child_warmups), w_max(num_child_warmups));
            if (node.output_slot >= bool_warmups.size()) {
                err = "node '" + node.node_id + "': bool output slot out of range";
                return false;
            }
            bool_warmups[node.output_slot] = warmup;
        }

        StrategyPlan plan{};
        plan.name = plan_template.name;
        plan.mode = plan_template.mode;
        plan.storage = std::move(storage);

        plan.first_valid_index = resolve_execution_first_valid_index(
            plan.storage->program,
            bool_warmups);

        out.plan = std::move(plan);
        out.exec = plan_template.exec;
        return true;
    }

    bool compile_plan(const PlanSpec& spec,
        const OperatorRegistry& registry,
        const CompileEnv& env,
        CompileResult& out,
        std::string& err)
    {
        CompiledPlanTemplate plan_template{};
        if (!compile_plan_template(spec, registry, env, plan_template, err)) {
            return false;
        }
        return instantiate_compiled_plan(plan_template, nullptr, out, err);
    }

} // namespace sugar

