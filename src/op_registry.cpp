#include "op_registry.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
#include <ostream>

namespace sugar {

    namespace {
        bool is_numeric_type(const std::string& t) {
            return t == "int" || t == "float";
        }

        std::size_t w_max(const std::vector<std::size_t>& v) {
            std::size_t m = 0;
            for (auto x : v) m = std::max(m, x);
            return m;
        }

        std::string vt(ValueType t) {
            return (t == ValueType::Num) ? "num" : "bool";
        }
        std::string ot(OpType t) {
            return (t == OpType::Numeric) ? "numeric" : "bool";
        }

        std::string resolved_param_to_string(const ResolvedParam& v) {
            if (std::holds_alternative<std::nullptr_t>(v)) return "null";
            if (std::holds_alternative<bool>(v)) return std::get<bool>(v) ? "true" : "false";
            if (std::holds_alternative<std::int64_t>(v)) return std::to_string(std::get<std::int64_t>(v));
            if (std::holds_alternative<double>(v)) {
                std::ostringstream oss;
                oss << std::get<double>(v);
                return oss.str();
            }
            if (std::holds_alternative<std::string>(v)) return "\"" + std::get<std::string>(v) + "\"";
            return "?";
        }

        // ------------------ REGISTRATION ------------------

        void register_core_ops(OperatorRegistry& r) {
            {
                OpEntry e;
                e.spec = OpSpec{
                    "core.close",
                    OpType::Numeric,
                    {},
                    ValueType::Num,
                    {},
                    "core",
                    false,
                    "0"
                };
                e.build_num = [](const std::vector<IndicatorPtr>&, const ResolvedParamMap&, const CompileEnv&) {
                    return std::make_shared<CloseIndicator>();
                    };
                e.warmup_num = [](const std::vector<std::size_t>&, const ResolvedParamMap&) { return std::size_t(0); };
                r.add(std::move(e));
            }

            {
                OpEntry e;
                OpParamSpec p; p.name = "value"; p.type = "float"; p.required = true;
                e.spec = OpSpec{
                    "core.const",
                    OpType::Numeric,
                    {},
                    ValueType::Num,
                    {p},
                    "core",
                    false,
                    "0"
                };
                e.build_num = [](const std::vector<IndicatorPtr>&, const ResolvedParamMap& pm, const CompileEnv&) {
                    const auto v = as_f64(pm, "value").value_or(0.0);
                    return constant(v);
                    };
                e.warmup_num = [](const std::vector<std::size_t>&, const ResolvedParamMap&) { return std::size_t(0); };
                r.add(std::move(e));
            }

            {
                OpEntry e;
                OpParamSpec p; p.name = "name"; p.type = "string"; p.required = true;
                e.spec = OpSpec{
                    "core.precomputed_num",
                    OpType::Numeric,
                    {},
                    ValueType::Num,
                    {p},
                    "core",
                    false,
                    "env-defined (typically 0)"
                };
                e.build_num = [](const std::vector<IndicatorPtr>&, const ResolvedParamMap& pm, const CompileEnv& env) -> IndicatorPtr {
                    const auto key = as_str(pm, "name").value_or(std::string{});
                    auto it = env.numeric_leaves.find(key);
                    if (it == env.numeric_leaves.end()) {
                        return constant(std::numeric_limits<double>::quiet_NaN());
                    }
                    return it->second;
                    };
                e.warmup_num = [](const std::vector<std::size_t>&, const ResolvedParamMap&) { return std::size_t(0); };
                r.add(std::move(e));
            }

            {
                OpEntry e;
                OpParamSpec p; p.name = "value"; p.type = "bool"; p.required = true;
                e.spec = OpSpec{
                    "core.bool_const",
                    OpType::Bool,
                    {},
                    ValueType::Bool,
                    {p},
                    "core",
                    false,
                    "0"
                };
                e.build_bool = [](const std::vector<BoolSignalPtr>&, const std::vector<IndicatorPtr>&, const ResolvedParamMap& pm, const CompileEnv&) {
                    const bool v = as_bool(pm, "value").value_or(false);
                    return bconst(v);
                    };
                e.warmup_bool = [](const std::vector<std::size_t>&, const std::vector<std::size_t>&, const ResolvedParamMap&) { return std::size_t(0); };
                r.add(std::move(e));
            }

            {
                OpEntry e;
                OpParamSpec p; p.name = "name"; p.type = "string"; p.required = true;
                e.spec = OpSpec{
                    "core.precomputed_bool",
                    OpType::Bool,
                    {},
                    ValueType::Bool,
                    {p},
                    "core",
                    false,
                    "env-defined (typically 0)"
                };
                e.build_bool = [](const std::vector<BoolSignalPtr>&, const std::vector<IndicatorPtr>&, const ResolvedParamMap& pm, const CompileEnv& env) -> BoolSignalPtr {
                    const auto key = as_str(pm, "name").value_or(std::string{});
                    auto it = env.bool_leaves.find(key);
                    if (it == env.bool_leaves.end()) {
                        return bconst(false);
                    }
                    return it->second;
                    };
                e.warmup_bool = [](const std::vector<std::size_t>&, const std::vector<std::size_t>&, const ResolvedParamMap&) { return std::size_t(0); };
                r.add(std::move(e));
            }
        }

        void register_ts_ops(OperatorRegistry& r) {
            auto add_unary_n = [&](const char* name,
                std::function<IndicatorPtr(IndicatorPtr, std::size_t)> fn,
                std::function<std::size_t(std::size_t, std::size_t)> warm,
                const char* warm_desc,
                const char* param_name) {

                    OpEntry e;
                    OpParamSpec p; p.name = param_name; p.type = "int"; p.required = true; p.min_num = 1;
                    e.spec = OpSpec{
                        name,
                        OpType::Numeric,
                        {ValueType::Num},
                        ValueType::Num,
                        {p},
                        "ts",
                        false,
                        warm_desc
                    };
                    e.build_num = [fn, param_name](const std::vector<IndicatorPtr>& in, const ResolvedParamMap& pm, const CompileEnv&) {
                        const auto n = static_cast<std::size_t>(std::max<std::int64_t>(1, as_i64(pm, param_name).value_or(1)));
                        return fn(in.at(0), n);
                        };
                    e.warmup_num = [warm, param_name](const std::vector<std::size_t>& cw, const ResolvedParamMap& pm) {
                        const auto n = static_cast<std::size_t>(std::max<std::int64_t>(1, as_i64(pm, param_name).value_or(1)));
                        return warm(cw.at(0), n);
                        };
                    r.add(std::move(e));
                };

            {
                OpEntry e;
                OpParamSpec p; p.name = "k"; p.type = "int"; p.required = true; p.min_num = 1;
                e.spec = OpSpec{
                    "ts.roc",
                    OpType::Numeric,
                    {ValueType::Num},
                    ValueType::Num,
                    {p},
                    "ts",
                    false,
                    "child_warmup + k"
                };
                e.build_num = [](const std::vector<IndicatorPtr>& in, const ResolvedParamMap& pm, const CompileEnv&) {
                    const auto k = static_cast<std::size_t>(std::max<std::int64_t>(1, as_i64(pm, "k").value_or(1)));
                    return map_roc(in.at(0), k);
                    };
                e.warmup_num = [](const std::vector<std::size_t>& cw, const ResolvedParamMap& pm) {
                    const auto k = static_cast<std::size_t>(std::max<std::int64_t>(1, as_i64(pm, "k").value_or(1)));
                    return cw.at(0) + k;
                    };
                r.add(std::move(e));
            }

            add_unary_n("ts.sma", map_sma, [](std::size_t child, std::size_t n) { return child + (n - 1); }, "child_warmup + (n-1)", "n");
            add_unary_n("ts.ema", map_ema, [](std::size_t child, std::size_t n) { return child + (n - 1); }, "child_warmup + (n-1)", "n");
            add_unary_n("ts.roll_max", map_rolling_max, [](std::size_t child, std::size_t n) { return child + (n - 1); }, "child_warmup + (n-1)", "n");
            add_unary_n("ts.roll_min", map_rolling_min, [](std::size_t child, std::size_t n) { return child + (n - 1); }, "child_warmup + (n-1)", "n");
            add_unary_n("ts.normalize_minmax", map_normalize_minmax, [](std::size_t child, std::size_t n) { return child + (n - 1); }, "child_warmup + (n-1)", "n");

            {
                OpEntry e;
                OpParamSpec p1; p1.name = "k"; p1.type = "int"; p1.required = true; p1.min_num = 0;
                OpParamSpec p2; p2.name = "fill"; p2.type = "float"; p2.required = false;
                e.spec = OpSpec{
                    "ts.shift",
                    OpType::Numeric,
                    {ValueType::Num},
                    ValueType::Num,
                    {p1, p2},
                    "ts",
                    false,
                    "child_warmup + k"
                };
                e.build_num = [](const std::vector<IndicatorPtr>& in, const ResolvedParamMap& pm, const CompileEnv&) {
                    const auto k = static_cast<std::size_t>(std::max<std::int64_t>(0, as_i64(pm, "k").value_or(0)));
                    const auto fill = as_f64(pm, "fill").value_or(std::numeric_limits<double>::quiet_NaN());
                    return map_shift(in.at(0), k, fill);
                    };
                e.warmup_num = [](const std::vector<std::size_t>& cw, const ResolvedParamMap& pm) {
                    const auto k = static_cast<std::size_t>(std::max<std::int64_t>(0, as_i64(pm, "k").value_or(0)));
                    return cw.at(0) + k;
                    };
                r.add(std::move(e));
            }

            {
                OpEntry e;
                e.spec = OpSpec{
                    "ts.delta",
                    OpType::Numeric,
                    {ValueType::Num},
                    ValueType::Num,
                    {},
                    "ts",
                    false,
                    "child_warmup + 1"
                };
                e.build_num = [](const std::vector<IndicatorPtr>& in, const ResolvedParamMap&, const CompileEnv&) {
                    return map_delta(in.at(0));
                    };
                e.warmup_num = [](const std::vector<std::size_t>& cw, const ResolvedParamMap&) {
                    return cw.at(0) + 1;
                    };
                r.add(std::move(e));
            }

            auto add_unary0 = [&](const char* name, std::function<IndicatorPtr(IndicatorPtr)> fn, const char* warm_desc) {
                OpEntry e;
                e.spec = OpSpec{
                    name,
                    OpType::Numeric,
                    {ValueType::Num},
                    ValueType::Num,
                    {},
                    "ts",
                    false,
                    warm_desc
                };
                e.build_num = [fn](const std::vector<IndicatorPtr>& in, const ResolvedParamMap&, const CompileEnv&) {
                    return fn(in.at(0));
                    };
                e.warmup_num = [](const std::vector<std::size_t>& cw, const ResolvedParamMap&) { return cw.at(0); };
                r.add(std::move(e));
                };
            add_unary0("ts.neg", map_negate, "child_warmup");
            add_unary0("ts.abs", map_abs, "child_warmup");
            add_unary0("ts.log", map_log, "child_warmup");
            add_unary0("ts.exp", map_exp, "child_warmup");
            add_unary0("ts.sqrt", map_sqrt, "child_warmup");

            {
                OpEntry e;
                OpParamSpec p; p.name = "p"; p.type = "float"; p.required = true;
                e.spec = OpSpec{
                    "ts.pow_const",
                    OpType::Numeric,
                    {ValueType::Num},
                    ValueType::Num,
                    {p},
                    "ts",
                    false,
                    "child_warmup"
                };
                e.build_num = [](const std::vector<IndicatorPtr>& in, const ResolvedParamMap& pm, const CompileEnv&) {
                    const auto pwr = as_f64(pm, "p").value_or(1.0);
                    return map_pow(in.at(0), pwr);
                    };
                e.warmup_num = [](const std::vector<std::size_t>& cw, const ResolvedParamMap&) { return cw.at(0); };
                r.add(std::move(e));
            }

            {
                OpEntry e;
                e.spec = OpSpec{
                    "ts.normalize_minmax_global",
                    OpType::Numeric,
                    {ValueType::Num},
                    ValueType::Num,
                    {},
                    "ts",
                    false,
                    "child_warmup"
                };
                e.build_num = [](const std::vector<IndicatorPtr>& in, const ResolvedParamMap&, const CompileEnv&) {
                    return map_normalize_minmax_global(in.at(0));
                    };
                e.warmup_num = [](const std::vector<std::size_t>& cw, const ResolvedParamMap&) { return cw.at(0); };
                r.add(std::move(e));
            }

            {
                OpEntry e;
                e.spec = OpSpec{
                    "ts.ratio_of_ma",
                    OpType::Numeric,
                    {ValueType::Num},
                    ValueType::Num,
                    {},
                    "ts",
                    false,
                    "child_warmup"
                };
                e.build_num = [](const std::vector<IndicatorPtr>& in, const ResolvedParamMap&, const CompileEnv&) {
                    return map_ratio_of_ma(in.at(0));
                    };
                e.warmup_num = [](const std::vector<std::size_t>& cw, const ResolvedParamMap&) { return cw.at(0); };
                r.add(std::move(e));
            }

            auto add_bin = [&](const char* name, std::function<IndicatorPtr(IndicatorPtr, IndicatorPtr)> fn) {
                OpEntry e;
                e.spec = OpSpec{
                    name,
                    OpType::Numeric,
                    {ValueType::Num, ValueType::Num},
                    ValueType::Num,
                    {},
                    "ts",
                    false,
                    "max(child_warmups)"
                };
                e.build_num = [fn](const std::vector<IndicatorPtr>& in, const ResolvedParamMap&, const CompileEnv&) {
                    return fn(in.at(0), in.at(1));
                    };
                e.warmup_num = [](const std::vector<std::size_t>& cw, const ResolvedParamMap&) { return std::max(cw.at(0), cw.at(1)); };
                r.add(std::move(e));
                };

            add_bin("ts.add", map_add);
            add_bin("ts.sub", map_sub);
            add_bin("ts.mul", map_mul);
            add_bin("ts.div", map_div);
            add_bin("ts.min", map_min);
            add_bin("ts.max", map_max);

            {
                OpEntry e;
                OpParamSpec lo; lo.name = "lo"; lo.type = "float"; lo.required = true;
                OpParamSpec hi; hi.name = "hi"; hi.type = "float"; hi.required = true;
                e.spec = OpSpec{
                    "ts.clamp_const",
                    OpType::Numeric,
                    {ValueType::Num},
                    ValueType::Num,
                    {lo, hi},
                    "ts",
                    false,
                    "child_warmup"
                };
                e.build_num = [](const std::vector<IndicatorPtr>& in, const ResolvedParamMap& pm, const CompileEnv&) {
                    const double lo = as_f64(pm, "lo").value_or(0.0);
                    const double hi = as_f64(pm, "hi").value_or(0.0);
                    return map_clamp_const(in.at(0), lo, hi);
                    };
                e.warmup_num = [](const std::vector<std::size_t>& cw, const ResolvedParamMap&) { return cw.at(0); };
                r.add(std::move(e));
            }

            {
                OpEntry e;
                e.spec = OpSpec{
                    "ts.clamp",
                    OpType::Numeric,
                    {ValueType::Num, ValueType::Num, ValueType::Num},
                    ValueType::Num,
                    {},
                    "ts",
                    false,
                    "max(child_warmups)"
                };
                e.build_num = [](const std::vector<IndicatorPtr>& in, const ResolvedParamMap&, const CompileEnv&) {
                    return map_clamp(in.at(0), in.at(1), in.at(2));
                    };
                e.warmup_num = [](const std::vector<std::size_t>& cw, const ResolvedParamMap&) { return std::max({ cw.at(0), cw.at(1), cw.at(2) }); };
                r.add(std::move(e));
            }
        }

        void register_logic_ops(OperatorRegistry& r) {
            auto add_unary_bool = [&](const char* name, std::function<BoolSignalPtr(BoolSignalPtr)> fn) {
                OpEntry e;
                e.spec = OpSpec{
                    name,
                    OpType::Bool,
                    {ValueType::Bool},
                    ValueType::Bool,
                    {},
                    "logic",
                    false,
                    "child_warmup"
                };
                e.build_bool = [fn](const std::vector<BoolSignalPtr>& b, const std::vector<IndicatorPtr>&, const ResolvedParamMap&, const CompileEnv&) {
                    return fn(b.at(0));
                    };
                e.warmup_bool = [](const std::vector<std::size_t>& bw, const std::vector<std::size_t>&, const ResolvedParamMap&) { return bw.at(0); };
                r.add(std::move(e));
                };
            auto add_bin_bool = [&](const char* name, std::function<BoolSignalPtr(BoolSignalPtr, BoolSignalPtr)> fn, const char* warm_desc) {
                OpEntry e;
                e.spec = OpSpec{
                    name,
                    OpType::Bool,
                    {ValueType::Bool, ValueType::Bool},
                    ValueType::Bool,
                    {},
                    "logic",
                    false,
                    warm_desc
                };
                e.build_bool = [fn](const std::vector<BoolSignalPtr>& b, const std::vector<IndicatorPtr>&, const ResolvedParamMap&, const CompileEnv&) {
                    return fn(b.at(0), b.at(1));
                    };
                e.warmup_bool = [](const std::vector<std::size_t>& bw, const std::vector<std::size_t>&, const ResolvedParamMap&) { return std::max(bw.at(0), bw.at(1)); };
                r.add(std::move(e));
                };

            add_unary_bool("logic.not", bnot);
            add_bin_bool("logic.and", band, "max(child_warmups)");
            add_bin_bool("logic.or", bor, "max(child_warmups)");
            add_bin_bool("logic.nand", bnand, "max(child_warmups)");
            add_bin_bool("logic.nor", bnor, "max(child_warmups)");

            auto add_cmp = [&](const char* name, CmpOp op) {
                OpEntry e;
                e.spec = OpSpec{
                    name,
                    OpType::Bool,
                    {ValueType::Num, ValueType::Num},
                    ValueType::Bool,
                    {},
                    "cmp",
                    false,
                    "max(child_warmups)"
                };
                e.build_bool = [op](const std::vector<BoolSignalPtr>&, const std::vector<IndicatorPtr>& n, const ResolvedParamMap&, const CompileEnv&) {
                    switch (op) {
                    case CmpOp::LT: return blt(n.at(0), n.at(1));
                    case CmpOp::LE: return ble(n.at(0), n.at(1));
                    case CmpOp::GT: return bgt(n.at(0), n.at(1));
                    case CmpOp::GE: return bge(n.at(0), n.at(1));
                    case CmpOp::EQ: return beq(n.at(0), n.at(1));
                    case CmpOp::NE: return bne(n.at(0), n.at(1));
                    }
                    return bconst(false);
                    };
                e.warmup_bool = [](const std::vector<std::size_t>&, const std::vector<std::size_t>& nw, const ResolvedParamMap&) {
                    return std::max(nw.at(0), nw.at(1));
                    };
                r.add(std::move(e));
                };

            add_cmp("cmp.lt", CmpOp::LT);
            add_cmp("cmp.le", CmpOp::LE);
            add_cmp("cmp.gt", CmpOp::GT);
            add_cmp("cmp.ge", CmpOp::GE);
            add_cmp("cmp.eq", CmpOp::EQ);
            add_cmp("cmp.ne", CmpOp::NE);

            auto add_cross = [&](const char* name, CrossDir dir) {
                OpEntry e;
                e.spec = OpSpec{
                    name,
                    OpType::Bool,
                    {ValueType::Num, ValueType::Num},
                    ValueType::Bool,
                    {},
                    "cmp",
                    false,
                    "max(child_warmups) + 1 (needs previous bar)"
                };
                e.build_bool = [dir](const std::vector<BoolSignalPtr>&, const std::vector<IndicatorPtr>& n, const ResolvedParamMap&, const CompileEnv&) {
                    return (dir == CrossDir::Up) ? cross_up(n.at(0), n.at(1)) : cross_down(n.at(0), n.at(1));
                    };
                e.warmup_bool = [](const std::vector<std::size_t>&, const std::vector<std::size_t>& nw, const ResolvedParamMap&) {
                    return std::max(nw.at(0), nw.at(1)) + 1;
                    };
                r.add(std::move(e));
                };
            add_cross("cmp.cross_up", CrossDir::Up);
            add_cross("cmp.cross_down", CrossDir::Down);
        }

        // ------------------ DESCRIPTION DUMP ------------------

        void emit_one_op(std::ostream& os, const OpEntry& e) {
            os << "- **" << e.spec.name << "**"
                << "  \n  module: `" << e.spec.module << "`"
                << "  \n  op_type: `" << ot(e.spec.op_type) << "`"
                << "  \n  signature: (";

            for (std::size_t i = 0; i < e.spec.inputs.size(); ++i) {
                if (i) os << ", ";
                os << vt(e.spec.inputs[i]);
            }
            os << ") -> " << vt(e.spec.output);

            if (e.spec.is_macro) os << "  \n  kind: `macro`";
            else os << "  \n  kind: `primitive`";

            os << "  \n  warmup: `" << (e.spec.warmup_desc.empty() ? "unspecified" : e.spec.warmup_desc) << "`";

            if (!e.spec.params.empty()) {
                os << "  \n  params:";
                for (const auto& p : e.spec.params) {
                    os << "  \n    - `" << p.name << "`: " << p.type;
                    os << (p.required ? " (required)" : " (optional)");
                    if (p.default_value) os << ", default=" << resolved_param_to_string(*p.default_value);
                    if (p.min_num) os << ", min=" << *p.min_num;
                    if (p.max_num) os << ", max=" << *p.max_num;
                }
            }
            os << "\n";
        }
    }

    bool OperatorRegistry::has(const std::string& op_name) const {
        return ops_.find(op_name) != ops_.end();
    }

    const OpEntry* OperatorRegistry::get(const std::string& op_name) const {
        auto it = ops_.find(op_name);
        if (it == ops_.end()) return nullptr;
        return &it->second;
    }

    std::vector<std::string> OperatorRegistry::list_ops() const {
        std::vector<std::string> out;
        out.reserve(ops_.size());
        for (const auto& kv : ops_) out.push_back(kv.first);
        std::sort(out.begin(), out.end());
        return out;
    }

    std::vector<std::string> OperatorRegistry::list_modules() const {
        std::vector<std::string> m;
        for (const auto& kv : ops_) {
            if (std::find(m.begin(), m.end(), kv.second.spec.module) == m.end()) m.push_back(kv.second.spec.module);
        }
        std::sort(m.begin(), m.end());
        return m;
    }

    void OperatorRegistry::add(OpEntry e) {
        ops_[e.spec.name] = std::move(e);
    }

    OperatorRegistry make_default_registry() {
        OperatorRegistry r;
        register_core_ops(r);
        register_ts_ops(r);
        register_logic_ops(r);
        return r;
    }

    // ---- NEW: registry description output ----

    std::string OperatorRegistry::describe_text(bool include_modules_header) const {
        std::ostringstream oss;
        print_description(oss, include_modules_header);
        return oss.str();
    }

    void OperatorRegistry::print_description(std::ostream& os, bool include_modules_header) const {
        // deterministic order
        std::vector<const OpEntry*> ordered;
        ordered.reserve(ops_.size());
        for (const auto& kv : ops_) ordered.push_back(&kv.second);

        std::sort(ordered.begin(), ordered.end(),
            [](const OpEntry* a, const OpEntry* b) {
                if (a->spec.module != b->spec.module) return a->spec.module < b->spec.module;
                return a->spec.name < b->spec.name;
            });

        if (include_modules_header) {
            auto mods = list_modules();
            os << "# SugarBot Operator Registry (capabilities of this build)\n\n";
            os << "Modules loaded: ";
            for (std::size_t i = 0; i < mods.size(); ++i) {
                if (i) os << ", ";
                os << "`" << mods[i] << "`";
            }
            os << "\n\n";
            os << "Type legend: `num` = numeric series, `bool` = boolean series\n\n";

            os << "## PlanSpec / Custom Strategy JSON\n\n";
            os << "Custom User Strategy (menu item 99) loads a PlanSpec JSON, compiles a node graph into a StrategyPlan,\n";
            os << "then runs it through the normal StrategyExecutor.\n\n";

            os << "### Root schema (v1)\n";
            os << "Required:\n";
            os << "- `name` (string)\n";
            os << "- `bindings` (object)\n\n";
            os << "Optional:\n";
            os << "- `version` (int, default 1)\n";
            os << "- `required` (array<string>)  // informational only in this build\n";
            os << "- `trade_mode` (string: LongOnly | LongShort)\n";
            os << "- `exec_settings` (object)    // NOTE: JSON key is exec_settings (not 'exec')\n";
            os << "- `params` (object)           // top-level param table for {ref:...}\n";
            os << "- `numeric_nodes` (array<NodeSpec>)\n";
            os << "- `bool_nodes` (array<NodeSpec>)\n";
            os << "- `sweep` (object)            // optional sweep extension (see below)\n\n";

            os << "### NodeSpec\n";
            os << "- `id` (string, unique within its node list)\n";
            os << "- `op` (string)               // must exist in this registry; see modules below\n";
            os << "- `args` (array<string>)      // node ids; order must match the op signature\n";
            os << "- `params` (object)           // op params (scalars or {ref:\"paramName\"})\n\n";

            os << "### Type rules / graph wiring\n";
            os << "- `numeric_nodes` may only reference other numeric node ids in `args`.\n";
            os << "- `bool_nodes` may reference bool or numeric ids depending on the operator signature:\n";
            os << "  - logic.* ops consume bool inputs\n";
            os << "  - cmp.* ops consume numeric inputs\n";
            os << "- Node `args` are untyped in JSON; the compiler infers each arg's type from the operator signature.\n\n";

            os << "### Param refs and precedence\n";
            os << "- A param value may be a literal (number/bool/string/null), or a reference object: `{ \"ref\": \"k\" }`.\n";
            os << "- `{ref:...}` pulls from top-level `params`, and can be overridden by sweep overrides.\n";
            os << "- When resolving an op param, precedence is roughly:\n";
            os << "  1) sweep override for that param name (if present)\n";
            os << "  2) node.params literal/ref\n";
            os << "  3) plan-level params (used when node uses {ref:...})\n\n";

            os << "### Precomputed leaves (project-specific oddity)\n";
            os << "- `core.precomputed_num` / `core.precomputed_bool` require the named leaf to exist in the CompileEnv.\n";
            os << "  If the leaf is missing, compilation fails (not a runtime NaN/false fallback).\n";
            os << "  In Custom User Strategy (99), only the default environment is available, so avoid precomputed_* unless you\n";
            os << "  know that build/strategy injects those leaves.\n\n";

            os << "### Windows path and JSON escaping (common gotcha)\n";
            os << "- In JSON strings on Windows, backslashes must be escaped: `C:\\\\Dev\\\\plans\\\\my_plan.json`.\n";
            os << "  Forward slashes also work: `C:/Dev/plans/my_plan.json`.\n\n";

            os << "### Minimal example PlanSpec\n";
            os << R"JSON(
{
  "version": 1,
  "name": "SMA Cross (demo)",
  "trade_mode": "LongShort",
  "exec_settings": {
    "initial_equity": 10000,
    "fill_timing": "close",
    "costs": { "fee_bps": 0.0, "slippage_bps": 0.0 }
  },
  "params": { "fast": 10, "slow": 30 },
  "numeric_nodes": [
    { "id": "c", "op": "core.close" },
    { "id": "f", "op": "ts.sma", "args": ["c"], "params": { "n": { "ref": "fast" } } },
    { "id": "s", "op": "ts.sma", "args": ["c"], "params": { "n": { "ref": "slow" } } }
  ],
  "bool_nodes": [
    { "id": "enterL", "op": "cmp.cross_up", "args": ["f","s"] },
    { "id": "exitL",  "op": "cmp.cross_down", "args": ["f","s"] }
  ],
  "bindings": {
    "entry_long": "enterL",
    "exit_long":  "exitL",
    "entry_short": "",
    "exit_short":  "",
    "allow_trade": ""
  }
}
)JSON";
            os << "\n";

            os << "## PlanSpec sweep extension\n\n";
            os << "If `sweep` is present and `sweep.enabled=true`, SugarBot will iterate param combinations,\n";
            os << "compile+run each plan, and report Top-K.\n\n";
            os << "Sweep schema (v1):\n";
            os << "- `sweep.enabled` (bool, optional)\n";
            os << "- `sweep.top_k` (int, optional)\n";
            os << "- `sweep.max_evals` (int, optional)\n";
            os << "- `sweep.objective` (string: 'pnl' or 'pnl_dd', optional)\n";
            os << "- `sweep.dd_weight` (float, optional; used when objective='pnl_dd')\n";
            os << "- `sweep.engine` (string: 'exhaustive' | 'coarse_fine' | 'monte_carlo', optional; default='exhaustive')\n";
            os << "- `sweep.engine_config` (object, optional)\n";
            os << "  - `coarse_fine` object:\n";
            os << "    - `k_stage1`, `k_stage2` (int > 0)\n";
            os << "    - `depollute_stage1`, `dedupe_stage2` (bool)\n";
            os << "    - `suppression_hops`, `stage2_hops` (int > 0)\n";
            os << "    - `stage1_stride` (int >= 0; 0 means no coarse subset)\n";
            os << "  - `monte_carlo` object:\n";
            os << "    - `draws` (int > 0), `seed` (int >= 0), `dedupe` (bool), `top_k` (int > 0, optional)\n";
            os << "- `sweep.params` (object): map paramName -> range\n\n";
            os << "Range forms:\n";
            os << "- Discrete: `{ \"values\": [5,10,20], \"is_int\": true }`\n";
            os << "- Stepped : `{ \"min\": 5, \"max\": 50, \"step\": 5, \"is_int\": true }`\n\n";
        }

        std::string current_module;
        for (const auto* e : ordered) {
            if (e->spec.module != current_module) {
                current_module = e->spec.module;
                os << "## Module: `" << current_module << "`\n\n";
            }
            emit_one_op(os, *e);
            os << "\n";
        }
    }

    // ------------------ PARAM RESOLUTION (unchanged) ------------------

    namespace {
        const ParamValue* find_param(const ParamMap& m, const std::string& key) {
            auto it = m.find(key);
            if (it == m.end()) return nullptr;
            return &it->second;
        }

        bool to_resolved(const ParamValue& v, ResolvedParam& out) {
            if (std::holds_alternative<std::nullptr_t>(v)) { out = nullptr; return true; }
            if (std::holds_alternative<bool>(v)) { out = std::get<bool>(v); return true; }
            if (std::holds_alternative<std::int64_t>(v)) { out = std::get<std::int64_t>(v); return true; }
            if (std::holds_alternative<double>(v)) { out = std::get<double>(v); return true; }
            if (std::holds_alternative<std::string>(v)) { out = std::get<std::string>(v); return true; }
            return false;
        }

        bool check_type(const ResolvedParam& v, const std::string& t) {
            if (t == "bool") return std::holds_alternative<bool>(v);
            if (t == "string") return std::holds_alternative<std::string>(v);
            if (t == "int") return std::holds_alternative<std::int64_t>(v) || std::holds_alternative<double>(v);
            if (t == "float") return std::holds_alternative<double>(v) || std::holds_alternative<std::int64_t>(v);
            return false;
        }

        double as_number(const ResolvedParam& v) {
            if (std::holds_alternative<double>(v)) return std::get<double>(v);
            if (std::holds_alternative<std::int64_t>(v)) return static_cast<double>(std::get<std::int64_t>(v));
            return std::numeric_limits<double>::quiet_NaN();
        }
    }

    bool resolve_and_validate_params(const OpEntry& op,
        const ParamMap& node_params,
        const ParamMap& plan_params,
        const ParamMap* overrides,
        ResolvedParamMap& out,
        std::string& err)
    {
        out.clear();

        for (const auto& ps : op.spec.params) {
            const ParamValue* src = nullptr;
            if (overrides) src = find_param(*overrides, ps.name);
            if (!src) src = find_param(node_params, ps.name);

            ResolvedParam rv;
            bool has = false;

            if (src) {
                if (std::holds_alternative<ParamRef>(*src)) {
                    const auto& ref = std::get<ParamRef>(*src).name;
                    const ParamValue* pv = nullptr;
                    if (overrides) pv = find_param(*overrides, ref);
                    if (!pv) pv = find_param(plan_params, ref);
                    if (!pv) { err = "Param ref '" + ref + "' not found (needed for '" + ps.name + "')"; return false; }
                    if (std::holds_alternative<ParamRef>(*pv)) { err = "Param ref '" + ref + "' resolves to another ref (not allowed)"; return false; }
                    if (!to_resolved(*pv, rv)) { err = "Unsupported param value for ref '" + ref + "'"; return false; }
                    has = true;
                }
                else {
                    if (!to_resolved(*src, rv)) { err = "Unsupported param value type for '" + ps.name + "'"; return false; }
                    has = true;
                }
            }
            else if (ps.default_value.has_value()) {
                rv = *ps.default_value;
                has = true;
            }

            if (!has) {
                if (ps.required) { err = "Missing required param '" + ps.name + "'"; return false; }
                continue;
            }

            if (!check_type(rv, ps.type)) { err = "Param '" + ps.name + "' has wrong type (expected " + ps.type + ")"; return false; }

            if (is_numeric_type(ps.type)) {
                const double x = as_number(rv);
                if (ps.min_num && x < *ps.min_num) { err = "Param '" + ps.name + "' below min"; return false; }
                if (ps.max_num && x > *ps.max_num) { err = "Param '" + ps.name + "' above max"; return false; }
            }

            out[ps.name] = std::move(rv);
        }

        if (op.spec.name == "core.precomputed_num") {
            auto nm = as_str(out, "name");
            if (!nm || nm->empty()) { err = "core.precomputed_num requires param 'name'"; return false; }
        }
        if (op.spec.name == "core.precomputed_bool") {
            auto nm = as_str(out, "name");
            if (!nm || nm->empty()) { err = "core.precomputed_bool requires param 'name'"; return false; }
        }

        return true;
    }

    std::optional<double> as_f64(const ResolvedParamMap& m, const std::string& key) {
        auto it = m.find(key);
        if (it == m.end()) return std::nullopt;
        if (std::holds_alternative<double>(it->second)) return std::get<double>(it->second);
        if (std::holds_alternative<std::int64_t>(it->second)) return static_cast<double>(std::get<std::int64_t>(it->second));
        return std::nullopt;
    }

    std::optional<std::int64_t> as_i64(const ResolvedParamMap& m, const std::string& key) {
        auto it = m.find(key);
        if (it == m.end()) return std::nullopt;
        if (std::holds_alternative<std::int64_t>(it->second)) return std::get<std::int64_t>(it->second);
        if (std::holds_alternative<double>(it->second)) return static_cast<std::int64_t>(std::get<double>(it->second));
        return std::nullopt;
    }

    std::optional<bool> as_bool(const ResolvedParamMap& m, const std::string& key) {
        auto it = m.find(key);
        if (it == m.end()) return std::nullopt;
        if (std::holds_alternative<bool>(it->second)) return std::get<bool>(it->second);
        return std::nullopt;
    }

    std::optional<std::string> as_str(const ResolvedParamMap& m, const std::string& key) {
        auto it = m.find(key);
        if (it == m.end()) return std::nullopt;
        if (std::holds_alternative<std::string>(it->second)) return std::get<std::string>(it->second);
        return std::nullopt;
    }

} // namespace sugar
