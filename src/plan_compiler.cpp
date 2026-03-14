#include "plan_compiler.h"
#include "op_registry_internal.h"
#include "plan_compiler_internal.h"
#include "program_evaluator.h"
#include "strategy_plan_internal.h"

#include <algorithm>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

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
                    err = "node '" + node_id + "': precomputed numeric leaf '" + key + "' not found in CompileEnv";
                    return false;
                }
            }
            if (op.spec.name == "core.precomputed_bool") {
                const auto key = as_str(out, "name").value_or(std::string{});
                if (!storage || storage->bool_leaf_values.find(key) == storage->bool_leaf_values.end()) {
                    err = "node '" + node_id + "': precomputed bool leaf '" + key + "' not found in CompileEnv";
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
                err = "Unknown op '" + node.op + "' for numeric node '" + id + "'";
                return false;
            }
            if (op->spec.op_type != OpType::Numeric) {
                err = "Op '" + node.op + "' is not a numeric op";
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
                if (op->spec.inputs[i] != ValueType::Num) {
                    err = "Node '" + id + "' arg #" + std::to_string(i)
                        + " requires non-numeric input; cannot compile in numeric graph";
                    return false;
                }
                std::size_t child = 0;
                if (!build_numeric_template(ctx, node.args[i], child, err)) return false;
                compiled.inputs.push_back(ProgramInputRef{ ValueType::Num, child });
            }

            ctx.out->numeric_nodes.push_back(std::move(compiled));
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
                err = "Unknown op '" + node.op + "' for bool node '" + id + "'";
                return false;
            }
            if (op->spec.op_type != OpType::Bool) {
                err = "Op '" + node.op + "' is not a bool op";
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

    } // namespace

    bool compile_plan_template(const PlanSpec& spec,
        const OperatorRegistry& registry,
        const CompileEnv& env,
        CompiledPlanTemplate& out,
        std::string& err)
    {
        out = CompiledPlanTemplate{};
        out.name = spec.name;
        out.mode = spec.trade_mode;
        out.exec = spec.exec;
        out.plan_params = spec.params;
        out.env = env;

        TemplateBuildCtx ctx{ spec, registry, out };

        for (const auto& n : spec.numeric_nodes) {
            if (n.id.empty()) {
                err = "numeric node missing id";
                return false;
            }
            if (!ctx.num_nodes.emplace(n.id, &n).second) {
                err = "duplicate numeric node id: " + n.id;
                return false;
            }
        }
        for (const auto& n : spec.bool_nodes) {
            if (n.id.empty()) {
                err = "bool node missing id";
                return false;
            }
            if (!ctx.bool_nodes.emplace(n.id, &n).second) {
                err = "duplicate bool node id: " + n.id;
                return false;
            }
        }

        if (!bind_bool_template(ctx, spec.bind.entry_long, out.program.entry_long_slot, err)) return false;
        if (!bind_bool_template(ctx, spec.bind.exit_long, out.program.exit_long_slot, err)) return false;
        if (!bind_bool_template(ctx, spec.bind.entry_short, out.program.entry_short_slot, err)) return false;
        if (!bind_bool_template(ctx, spec.bind.exit_short, out.program.exit_short_slot, err)) return false;
        if (!bind_bool_template(ctx, spec.bind.allow_trade, out.program.allow_trade_slot, err)) return false;

        return true;
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
        storage->program.entry_long_slot = plan_template.program.entry_long_slot;
        storage->program.exit_long_slot = plan_template.program.exit_long_slot;
        storage->program.entry_short_slot = plan_template.program.entry_short_slot;
        storage->program.exit_short_slot = plan_template.program.exit_short_slot;
        storage->program.allow_trade_slot = plan_template.program.allow_trade_slot;
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
                if (input.type != ValueType::Num) {
                    err = "node '" + node.node_id + "': numeric op has non-numeric input slot";
                    return false;
                }
                if (input.slot >= numeric_warmups.size()) {
                    err = "node '" + node.node_id + "': numeric input slot out of range";
                    return false;
                }
                child_warmups.push_back(numeric_warmups[input.slot]);
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

        std::size_t first_valid = 0;
        auto bump = [&](std::size_t slot) {
            if (slot == kInvalidProgramSlot) return;
            if (slot < bool_warmups.size()) first_valid = std::max(first_valid, bool_warmups[slot]);
        };
        bump(plan.storage->program.entry_long_slot);
        bump(plan.storage->program.exit_long_slot);
        bump(plan.storage->program.entry_short_slot);
        bump(plan.storage->program.exit_short_slot);
        bump(plan.storage->program.allow_trade_slot);
        plan.first_valid_index = first_valid;

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

