#include "plan_compiler.h"

#include <algorithm>
#include <sstream>
#include <unordered_map>

namespace sugar {

    namespace {
        std::string join(const std::vector<std::string>& v, const char* sep) {
            std::ostringstream oss;
            for (std::size_t i = 0; i < v.size(); ++i) {
                if (i) oss << sep;
                oss << v[i];
            }
            return oss.str();
        }

        std::size_t w_max(const std::vector<std::size_t>& v) {
            std::size_t m = 0;
            for (auto x : v) m = std::max(m, x);
            return m;
        }

        struct Ctx {
            const PlanSpec& spec;
            const OperatorRegistry& reg;
            const CompileEnv& env;
            const ParamMap* overrides{ nullptr };

            std::unordered_map<std::string, const NodeSpec*> num_nodes;
            std::unordered_map<std::string, const NodeSpec*> bool_nodes;

            std::unordered_map<std::string, IndicatorPtr> num_cache;
            std::unordered_map<std::string, BoolSignalPtr> bool_cache;

            std::unordered_map<std::string, std::size_t> num_warm_cache;
            std::unordered_map<std::string, std::size_t> bool_warm_cache;
        };

        bool build_numeric(Ctx& ctx, const std::string& id, IndicatorPtr& out, std::string& err);
        bool build_bool(Ctx& ctx, const std::string& id, BoolSignalPtr& out, std::string& err);

        bool warm_numeric(Ctx& ctx, const std::string& id, std::size_t& out, std::string& err);
        bool warm_bool(Ctx& ctx, const std::string& id, std::size_t& out, std::string& err);

        bool resolve_op_params(Ctx& ctx, const OpEntry& op, const NodeSpec& node, ResolvedParamMap& pm, std::string& err) {
            const ParamMap* overrides = ctx.overrides;
            if (!resolve_and_validate_params(op, node.params, ctx.spec.params, overrides, pm, err)) {
                err = "node '" + node.id + "' (" + node.op + "): " + err;
                return false;
            }
            if (node.op == "core.precomputed_num") {
                auto nm = as_str(pm, "name");
                if (!nm || ctx.env.numeric_leaves.find(*nm) == ctx.env.numeric_leaves.end()) {
                    err = "node '" + node.id + "': precomputed numeric leaf '" + (nm ? *nm : std::string{}) + "' not found in CompileEnv";
                    return false;
                }
            }
            if (node.op == "core.precomputed_bool") {
                auto nm = as_str(pm, "name");
                if (!nm || ctx.env.bool_leaves.find(*nm) == ctx.env.bool_leaves.end()) {
                    err = "node '" + node.id + "': precomputed bool leaf '" + (nm ? *nm : std::string{}) + "' not found in CompileEnv";
                    return false;
                }
            }
            return true;
        }

        bool build_numeric(Ctx& ctx, const std::string& id, IndicatorPtr& out, std::string& err) {
            if (auto it = ctx.num_cache.find(id); it != ctx.num_cache.end()) { out = it->second; return true; }
            auto nit = ctx.num_nodes.find(id);
            if (nit == ctx.num_nodes.end()) { err = "Unknown numeric node id '" + id + "'"; return false; }
            const NodeSpec& node = *nit->second;
            const OpEntry* op = ctx.reg.get(node.op);
            if (!op) { err = "Unknown op '" + node.op + "' for numeric node '" + id + "'"; return false; }
            if (op->spec.op_type != OpType::Numeric) { err = "Op '" + node.op + "' is not a numeric op"; return false; }
            if (node.args.size() != op->spec.inputs.size()) {
                err = "Node '" + id + "' expected " + std::to_string(op->spec.inputs.size()) + " args, got " + std::to_string(node.args.size());
                return false;
            }
            std::vector<IndicatorPtr> inputs;
            inputs.reserve(node.args.size());
            for (std::size_t i = 0; i < node.args.size(); ++i) {
                if (op->spec.inputs[i] != ValueType::Num) { err = "Node '" + id + "' arg #" + std::to_string(i) + " expects bool input"; return false; }
                IndicatorPtr child;
                if (!build_numeric(ctx, node.args[i], child, err)) return false;
                inputs.push_back(std::move(child));
            }
            ResolvedParamMap pm;
            if (!resolve_op_params(ctx, *op, node, pm, err)) return false;
            if (!op->build_num) { err = "Op '" + node.op + "' has no numeric builder"; return false; }
            out = op->build_num(inputs, pm, ctx.env);
            ctx.num_cache[id] = out;
            return true;
        }

        bool build_bool(Ctx& ctx, const std::string& id, BoolSignalPtr& out, std::string& err) {
            if (auto it = ctx.bool_cache.find(id); it != ctx.bool_cache.end()) { out = it->second; return true; }
            auto bit = ctx.bool_nodes.find(id);
            if (bit == ctx.bool_nodes.end()) { err = "Unknown bool node id '" + id + "'"; return false; }
            const NodeSpec& node = *bit->second;
            const OpEntry* op = ctx.reg.get(node.op);
            if (!op) { err = "Unknown op '" + node.op + "' for bool node '" + id + "'"; return false; }
            if (op->spec.op_type != OpType::Bool) { err = "Op '" + node.op + "' is not a bool op"; return false; }
            if (node.args.size() != op->spec.inputs.size()) {
                err = "Node '" + id + "' expected " + std::to_string(op->spec.inputs.size()) + " args, got " + std::to_string(node.args.size());
                return false;
            }

            std::vector<BoolSignalPtr> bool_in;
            std::vector<IndicatorPtr> num_in;
            for (std::size_t i = 0; i < node.args.size(); ++i) {
                if (op->spec.inputs[i] == ValueType::Bool) {
                    BoolSignalPtr c;
                    if (!build_bool(ctx, node.args[i], c, err)) return false;
                    bool_in.push_back(std::move(c));
                }
                else {
                    IndicatorPtr c;
                    if (!build_numeric(ctx, node.args[i], c, err)) return false;
                    num_in.push_back(std::move(c));
                }
            }

            ResolvedParamMap pm;
            if (!resolve_op_params(ctx, *op, node, pm, err)) return false;
            if (!op->build_bool) { err = "Op '" + node.op + "' has no bool builder"; return false; }
            out = op->build_bool(bool_in, num_in, pm, ctx.env);
            ctx.bool_cache[id] = out;
            return true;
        }

        bool warm_numeric(Ctx& ctx, const std::string& id, std::size_t& out, std::string& err) {
            if (auto it = ctx.num_warm_cache.find(id); it != ctx.num_warm_cache.end()) { out = it->second; return true; }
            auto nit = ctx.num_nodes.find(id);
            if (nit == ctx.num_nodes.end()) { err = "Unknown numeric node id '" + id + "'"; return false; }
            const NodeSpec& node = *nit->second;
            const OpEntry* op = ctx.reg.get(node.op);
            if (!op) { err = "Unknown op '" + node.op + "' for numeric node '" + id + "'"; return false; }
            if (!op->warmup_num) {
                std::vector<std::size_t> cw;
                cw.reserve(node.args.size());
                for (const auto& a : node.args) { std::size_t w = 0; if (!warm_numeric(ctx, a, w, err)) return false; cw.push_back(w); }
                out = w_max(cw);
                ctx.num_warm_cache[id] = out;
                return true;
            }
            ResolvedParamMap pm;
            if (!resolve_op_params(ctx, *op, node, pm, err)) return false;

            std::vector<std::size_t> cw;
            cw.reserve(node.args.size());
            for (const auto& a : node.args) { std::size_t w = 0; if (!warm_numeric(ctx, a, w, err)) return false; cw.push_back(w); }
            out = op->warmup_num(cw, pm);
            ctx.num_warm_cache[id] = out;
            return true;
        }

        bool warm_bool(Ctx& ctx, const std::string& id, std::size_t& out, std::string& err) {
            if (auto it = ctx.bool_warm_cache.find(id); it != ctx.bool_warm_cache.end()) { out = it->second; return true; }
            auto bit = ctx.bool_nodes.find(id);
            if (bit == ctx.bool_nodes.end()) { err = "Unknown bool node id '" + id + "'"; return false; }
            const NodeSpec& node = *bit->second;
            const OpEntry* op = ctx.reg.get(node.op);
            if (!op) { err = "Unknown op '" + node.op + "' for bool node '" + id + "'"; return false; }

            ResolvedParamMap pm;
            if (!resolve_op_params(ctx, *op, node, pm, err)) return false;

            std::vector<std::size_t> bw;
            std::vector<std::size_t> nw;
            for (std::size_t i = 0; i < node.args.size(); ++i) {
                const auto& aid = node.args[i];
                if (op->spec.inputs[i] == ValueType::Bool) {
                    std::size_t w = 0; if (!warm_bool(ctx, aid, w, err)) return false; bw.push_back(w);
                }
                else {
                    std::size_t w = 0; if (!warm_numeric(ctx, aid, w, err)) return false; nw.push_back(w);
                }
            }

            if (op->warmup_bool) out = op->warmup_bool(bw, nw, pm);
            else out = std::max(w_max(bw), w_max(nw));
            ctx.bool_warm_cache[id] = out;
            return true;
        }

        bool bind_bool(Ctx& ctx, const std::string& id, BoolSignalPtr& dst, std::string& err) {
            if (id.empty()) { dst.reset(); return true; }
            return build_bool(ctx, id, dst, err);
        }
    }

    bool compile_plan(const PlanSpec& spec,
        const OperatorRegistry& registry,
        const CompileEnv& env,
        CompileResult& out,
        std::string& err)
    {
        Ctx ctx{ spec, registry, env };

        for (const auto& n : spec.numeric_nodes) {
            if (n.id.empty()) { err = "numeric node missing id"; return false; }
            if (!ctx.num_nodes.emplace(n.id, &n).second) { err = "duplicate numeric node id: " + n.id; return false; }
        }
        for (const auto& n : spec.bool_nodes) {
            if (n.id.empty()) { err = "bool node missing id"; return false; }
            if (!ctx.bool_nodes.emplace(n.id, &n).second) { err = "duplicate bool node id: " + n.id; return false; }
        }

        StrategyPlan plan;
        plan.name = spec.name;
        plan.mode = spec.trade_mode;

        if (!bind_bool(ctx, spec.bind.entry_long, plan.signals.entry_long, err)) return false;
        if (!bind_bool(ctx, spec.bind.exit_long, plan.signals.exit_long, err)) return false;
        if (!bind_bool(ctx, spec.bind.entry_short, plan.signals.entry_short, err)) return false;
        if (!bind_bool(ctx, spec.bind.exit_short, plan.signals.exit_short, err)) return false;
        if (!bind_bool(ctx, spec.bind.allow_trade, plan.signals.allow_trade, err)) return false;

        std::size_t fv = 0;
        auto bump = [&](const std::string& id) {
            if (id.empty()) return true;
            std::size_t w = 0;
            if (!warm_bool(ctx, id, w, err)) return false;
            fv = std::max(fv, w);
            return true;
            };
        if (!bump(spec.bind.entry_long)) return false;
        if (!bump(spec.bind.exit_long)) return false;
        if (!bump(spec.bind.entry_short)) return false;
        if (!bump(spec.bind.exit_short)) return false;
        if (!bump(spec.bind.allow_trade)) return false;

        plan.first_valid_index = fv;

        out.plan = std::move(plan);
        out.exec = spec.exec;
        return true;
    }

} // namespace sugar
