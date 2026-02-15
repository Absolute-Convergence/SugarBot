#include "plan_spec.h"

#include <fstream>
#include <sstream>

namespace sugar {

    namespace {
        using namespace sugar::json_min;

        std::string type_name(const Value& v) {
            if (v.is_null()) return "null";
            if (v.is_bool()) return "bool";
            if (v.is_i64()) return "int";
            if (v.is_f64()) return "float";
            if (v.is_str()) return "string";
            if (v.is_arr()) return "array";
            if (v.is_obj()) return "object";
            return "unknown";
        }

        bool as_i64(const Value& v, std::int64_t& out) {
            if (!v.is_num()) return false;
            out = v.number_as_i64();
            return true;
        }

        bool parse_trade_mode(const std::string& s, TradeMode& m) {
            if (s == "LongOnly" || s == "long_only" || s == "LONG_ONLY") { m = TradeMode::LongOnly; return true; }
            if (s == "LongShort" || s == "long_short" || s == "LONG_SHORT") { m = TradeMode::LongShort; return true; }
            return false;
        }

        bool parse_sweep_engine_kind(const std::string& s, PlanSweepEngineKind& out) {
            if (s == "exhaustive") { out = PlanSweepEngineKind::Exhaustive; return true; }
            if (s == "coarse_fine") { out = PlanSweepEngineKind::CoarseFine; return true; }
            if (s == "monte_carlo") { out = PlanSweepEngineKind::MonteCarlo; return true; }
            return false;
        }

        bool parse_exec_settings(const Value& v, ExecSettings& out, std::string& err) {
            if (!v.is_obj()) { err = "exec_settings must be an object"; return false; }
            const auto& o = v.as_obj();

            if (auto* p = obj_get(o, "initial_equity")) {
                if (!p->is_num()) { err = "exec_settings.initial_equity must be number"; return false; }
                out.initial_equity = p->number_as_f64();
            }

            if (auto* p = obj_get(o, "eval_mode")) {
                if (!p->is_str()) { err = "exec_settings.eval_mode must be string"; return false; }
                const auto& s = p->as_str();
                if (s == "SameBar" || s == "same") out.eval_mode = BarEvalMode::SameBar;
                else if (s == "NextBar" || s == "next") out.eval_mode = BarEvalMode::NextBar;
                else { err = "exec_settings.eval_mode must be SameBar or NextBar"; return false; }
            }

            if (auto* p = obj_get(o, "fill_timing")) {
                if (!p->is_str()) { err = "exec_settings.fill_timing must be string"; return false; }
                const auto& s = p->as_str();
                if (s == "SameBarClose" || s == "close") out.fill_timing = FillTiming::SameBarClose;
                else if (s == "NextBarOpen" || s == "open") out.fill_timing = FillTiming::NextBarOpen;
                else { err = "exec_settings.fill_timing must be SameBarClose or NextBarOpen"; return false; }
            }

            if (auto* p = obj_get(o, "costs")) {
                if (!p->is_obj()) { err = "exec_settings.costs must be object"; return false; }
                const auto& co = p->as_obj();
                if (auto* fee = obj_get(co, "fee_bps")) {
                    if (!fee->is_num()) { err = "exec_settings.costs.fee_bps must be number"; return false; }
                    out.costs.fee_bps = fee->number_as_f64();
                }
                if (auto* sl = obj_get(co, "slippage_bps")) {
                    if (!sl->is_num()) { err = "exec_settings.costs.slippage_bps must be number"; return false; }
                    out.costs.slippage_bps = sl->number_as_f64();
                }
            }

            auto parse_bool_field = [&](const char* key, bool& dst) -> bool {
                if (auto* p = obj_get(o, key)) {
                    if (!p->is_bool()) { err = std::string("exec_settings.") + key + " must be bool"; return false; }
                    dst = p->as_bool();
                }
                return true;
                };

            if (!parse_bool_field("exits_first", out.exits_first)) return false;
            if (!parse_bool_field("allow_same_bar_reversal", out.allow_same_bar_reversal)) return false;
            if (!parse_bool_field("close_out_at_end", out.close_out_at_end)) return false;

            return true;
        }

        bool parse_param_value(const Value& v, ParamValue& out, std::string& err) {
            if (v.is_obj()) {
                const auto& o = v.as_obj();
                if (auto* r = obj_get(o, "ref")) {
                    if (!r->is_str()) { err = "Param ref {ref:...} must be string"; return false; }
                    out = ParamRef{ r->as_str() };
                    return true;
                }
                err = "Param values may not be objects (except {ref:...})";
                return false;
            }
            if (v.is_null()) { out = nullptr; return true; }
            if (v.is_bool()) { out = v.as_bool(); return true; }
            if (v.is_i64()) { out = v.as_i64(); return true; }
            if (v.is_f64()) { out = v.as_f64(); return true; }
            if (v.is_str()) { out = v.as_str(); return true; }
            err = "Unsupported param value type: " + type_name(v);
            return false;
        }

        bool parse_param_map(const Value& v, ParamMap& out, std::string& err) {
            if (!v.is_obj()) { err = "params must be object"; return false; }
            for (const auto& kv : v.as_obj()) {
                ParamValue pv;
                if (!parse_param_value(kv.second, pv, err)) {
                    err = "params." + kv.first + ": " + err;
                    return false;
                }
                out[kv.first] = std::move(pv);
            }
            return true;
        }

        bool parse_nodes(const Value& v, std::vector<NodeSpec>& out, std::string& err) {
            if (!v.is_arr()) { err = "nodes must be an array"; return false; }
            for (const auto& it : v.as_arr()) {
                if (!it.is_obj()) { err = "node must be object"; return false; }
                const auto& o = it.as_obj();
                NodeSpec n;

                auto* idv = obj_get(o, "id");
                auto* opv = obj_get(o, "op");
                if (!idv || !opv || !idv->is_str() || !opv->is_str()) {
                    err = "node required string fields 'id' and 'op'";
                    return false;
                }
                n.id = idv->as_str();
                n.op = opv->as_str();

                if (auto* av = obj_get(o, "args")) {
                    if (!av->is_arr()) { err = "node.args must be array"; return false; }
                    for (const auto& a : av->as_arr()) {
                        if (!a.is_str()) { err = "node.args entries must be strings"; return false; }
                        n.args.push_back(a.as_str());
                    }
                }

                if (auto* pv = obj_get(o, "params")) {
                    if (!parse_param_map(*pv, n.params, err)) {
                        err = "node.params: " + err;
                        return false;
                    }
                }

                out.push_back(std::move(n));
            }
            return true;
        }

        bool parse_bindings(const Value& v, PlanBindings& b, std::string& err) {
            if (!v.is_obj()) { err = "bindings must be object"; return false; }
            const auto& o = v.as_obj();
            auto get_str = [&](const char* key, std::string& dst) -> bool {
                if (auto* p = obj_get(o, key)) {
                    if (!p->is_str()) { err = std::string("bindings.") + key + " must be string"; return false; }
                    dst = p->as_str();
                }
                return true;
                };

            if (!get_str("entry_long", b.entry_long)) return false;
            if (!get_str("exit_long", b.exit_long)) return false;
            if (!get_str("entry_short", b.entry_short)) return false;
            if (!get_str("exit_short", b.exit_short)) return false;
            if (!get_str("allow_trade", b.allow_trade)) return false;
            return true;
        }

        bool parse_sweep_param_range(const Value& v, SweepParamRange& out, std::string& err) {
            if (!v.is_obj()) { err = "sweep param range must be object"; return false; }
            const auto& o = v.as_obj();

            // Optional: is_int
            if (auto* p = obj_get(o, "is_int")) {
                if (!p->is_bool()) { err = "is_int must be bool"; return false; }
                out.is_int = p->as_bool();
            }

            // Discrete values mode
            if (auto* pv = obj_get(o, "values")) {
                if (!pv->is_arr()) { err = "values must be array"; return false; }
                out.values.clear();
                for (const auto& it : pv->as_arr()) {
                    if (!it.is_num()) { err = "values entries must be numbers"; return false; }
                    out.values.push_back(it.number_as_f64());
                }
                return true;
            }

            // Range mode: min/max/step
            auto* vmin = obj_get(o, "min");
            auto* vmax = obj_get(o, "max");
            if (!vmin || !vmax || !vmin->is_num() || !vmax->is_num()) {
                err = "range mode requires numeric min and max (or provide values[])";
                return false;
            }
            out.min = vmin->number_as_f64();
            out.max = vmax->number_as_f64();
            out.step = 1.0;
            if (auto* vs = obj_get(o, "step")) {
                if (!vs->is_num()) { err = "step must be number"; return false; }
                out.step = vs->number_as_f64();
            }
            if (out.step <= 0.0) { err = "step must be > 0"; return false; }
            if (out.max < out.min) { err = "max must be >= min"; return false; }
            return true;
        }

        bool parse_sweep_spec(const Value& v, PlanSweepSpec& out, std::string& err) {
            if (!v.is_obj()) { err = "sweep must be object"; return false; }
            const auto& o = v.as_obj();

            if (auto* p = obj_get(o, "enabled")) {
                if (!p->is_bool()) { err = "sweep.enabled must be bool"; return false; }
                out.enabled = p->as_bool();
            }
            if (auto* p = obj_get(o, "top_k")) {
                std::int64_t x = 0;
                if (!as_i64(*p, x)) { err = "sweep.top_k must be number"; return false; }
                out.top_k = static_cast<std::size_t>(x);
            }
            if (auto* p = obj_get(o, "max_evals")) {
                std::int64_t x = 0;
                if (!as_i64(*p, x)) { err = "sweep.max_evals must be number"; return false; }
                out.max_evals = static_cast<std::size_t>(x);
            }
            if (auto* p = obj_get(o, "objective")) {
                if (!p->is_str()) { err = "sweep.objective must be string"; return false; }
                out.objective = p->as_str();
            }
            if (auto* p = obj_get(o, "dd_weight")) {
                if (!p->is_num()) { err = "sweep.dd_weight must be number"; return false; }
                out.dd_weight = p->number_as_f64();
            }
            if (auto* p = obj_get(o, "engine")) {
                if (!p->is_str()) { err = "sweep.engine must be string"; return false; }
                if (!parse_sweep_engine_kind(p->as_str(), out.engine)) {
                    err = "sweep.engine must be one of: exhaustive, coarse_fine, monte_carlo";
                    return false;
                }
            }

            if (auto* p = obj_get(o, "engine_config")) {
                if (!p->is_obj()) { err = "sweep.engine_config must be object"; return false; }
                const auto& ec = p->as_obj();

                if (auto* cf = obj_get(ec, "coarse_fine")) {
                    if (!cf->is_obj()) { err = "sweep.engine_config.coarse_fine must be object"; return false; }
                    const auto& cfo = cf->as_obj();

                    if (auto* x = obj_get(cfo, "k_stage1")) {
                        std::int64_t n = 0;
                        if (!as_i64(*x, n)) { err = "sweep.engine_config.coarse_fine.k_stage1 must be number"; return false; }
                        if (n <= 0) { err = "sweep.engine_config.coarse_fine.k_stage1 must be > 0"; return false; }
                        out.coarse_fine.k_stage1 = static_cast<std::size_t>(n);
                    }
                    if (auto* x = obj_get(cfo, "k_stage2")) {
                        std::int64_t n = 0;
                        if (!as_i64(*x, n)) { err = "sweep.engine_config.coarse_fine.k_stage2 must be number"; return false; }
                        if (n <= 0) { err = "sweep.engine_config.coarse_fine.k_stage2 must be > 0"; return false; }
                        out.coarse_fine.k_stage2 = static_cast<std::size_t>(n);
                    }
                    if (auto* x = obj_get(cfo, "depollute_stage1")) {
                        if (!x->is_bool()) { err = "sweep.engine_config.coarse_fine.depollute_stage1 must be bool"; return false; }
                        out.coarse_fine.depollute_stage1 = x->as_bool();
                    }
                    if (auto* x = obj_get(cfo, "dedupe_stage2")) {
                        if (!x->is_bool()) { err = "sweep.engine_config.coarse_fine.dedupe_stage2 must be bool"; return false; }
                        out.coarse_fine.dedupe_stage2 = x->as_bool();
                    }
                    if (auto* x = obj_get(cfo, "suppression_hops")) {
                        std::int64_t n = 0;
                        if (!as_i64(*x, n)) { err = "sweep.engine_config.coarse_fine.suppression_hops must be number"; return false; }
                        if (n <= 0) { err = "sweep.engine_config.coarse_fine.suppression_hops must be > 0"; return false; }
                        out.coarse_fine.suppression_hops = static_cast<std::size_t>(n);
                    }
                    if (auto* x = obj_get(cfo, "stage2_hops")) {
                        std::int64_t n = 0;
                        if (!as_i64(*x, n)) { err = "sweep.engine_config.coarse_fine.stage2_hops must be number"; return false; }
                        if (n <= 0) { err = "sweep.engine_config.coarse_fine.stage2_hops must be > 0"; return false; }
                        out.coarse_fine.stage2_hops = static_cast<std::size_t>(n);
                    }
                    if (auto* x = obj_get(cfo, "stage1_stride")) {
                        std::int64_t n = 0;
                        if (!as_i64(*x, n)) { err = "sweep.engine_config.coarse_fine.stage1_stride must be number"; return false; }
                        if (n < 0) { err = "sweep.engine_config.coarse_fine.stage1_stride must be >= 0"; return false; }
                        out.coarse_fine.stage1_stride = static_cast<std::size_t>(n);
                    }
                }

                if (auto* mc = obj_get(ec, "monte_carlo")) {
                    if (!mc->is_obj()) { err = "sweep.engine_config.monte_carlo must be object"; return false; }
                    const auto& mco = mc->as_obj();

                    if (auto* x = obj_get(mco, "draws")) {
                        std::int64_t n = 0;
                        if (!as_i64(*x, n)) { err = "sweep.engine_config.monte_carlo.draws must be number"; return false; }
                        if (n <= 0) { err = "sweep.engine_config.monte_carlo.draws must be > 0"; return false; }
                        out.monte_carlo.draws = static_cast<std::size_t>(n);
                    }
                    if (auto* x = obj_get(mco, "seed")) {
                        std::int64_t n = 0;
                        if (!as_i64(*x, n)) { err = "sweep.engine_config.monte_carlo.seed must be number"; return false; }
                        if (n < 0) { err = "sweep.engine_config.monte_carlo.seed must be >= 0"; return false; }
                        out.monte_carlo.seed = static_cast<std::uint64_t>(n);
                    }
                    if (auto* x = obj_get(mco, "dedupe")) {
                        if (!x->is_bool()) { err = "sweep.engine_config.monte_carlo.dedupe must be bool"; return false; }
                        out.monte_carlo.dedupe = x->as_bool();
                    }
                    if (auto* x = obj_get(mco, "top_k")) {
                        std::int64_t n = 0;
                        if (!as_i64(*x, n)) { err = "sweep.engine_config.monte_carlo.top_k must be number"; return false; }
                        if (n <= 0) { err = "sweep.engine_config.monte_carlo.top_k must be > 0"; return false; }
                        out.monte_carlo.top_k = static_cast<std::size_t>(n);
                    }
                }
            }

            if (auto* p = obj_get(o, "params")) {
                if (!p->is_obj()) { err = "sweep.params must be object"; return false; }
                out.params.clear();
                for (const auto& kv : p->as_obj()) {
                    SweepParamRange r;
                    std::string e;
                    if (!parse_sweep_param_range(kv.second, r, e)) {
                        err = "sweep.params." + kv.first + ": " + e;
                        return false;
                    }
                    out.params[kv.first] = std::move(r);
                }
            }

            return true;
        }
    }

    bool parse_plan_spec_json(std::string_view json, PlanSpec& out, std::string& err) {
        using namespace sugar::json_min;
        Value root;
        ParseError perr;
        if (!parse(json, root, perr)) {
            err = perr.message + " (line " + std::to_string(perr.line) + ", col " + std::to_string(perr.col) + ")";
            return false;
        }
        if (!root.is_obj()) { err = "PlanSpec root must be object"; return false; }

        const auto& o = root.as_obj();
        if (auto* v = obj_get(o, "version")) {
            std::int64_t vi = 0;
            if (!as_i64(*v, vi)) { err = "version must be number"; return false; }
            out.version = static_cast<int>(vi);
        }
        if (auto* n = obj_get(o, "name")) {
            if (!n->is_str()) { err = "name must be string"; return false; }
            out.name = n->as_str();
        }
        if (auto* r = obj_get(o, "required")) {
            if (!r->is_arr()) { err = "required must be array"; return false; }
            out.required.clear();
            for (const auto& it : r->as_arr()) {
                if (!it.is_str()) { err = "required entries must be string"; return false; }
                out.required.push_back(it.as_str());
            }
        }

        if (auto* tm = obj_get(o, "trade_mode")) {
            if (!tm->is_str()) { err = "trade_mode must be string"; return false; }
            TradeMode m{};
            if (!parse_trade_mode(tm->as_str(), m)) { err = "trade_mode must be LongOnly or LongShort"; return false; }
            out.trade_mode = m;
        }

        if (auto* ex = obj_get(o, "exec_settings")) {
            std::string e;
            if (!parse_exec_settings(*ex, out.exec, e)) { err = e; return false; }
        }

        if (auto* p = obj_get(o, "params")) {
            std::string e;
            if (!parse_param_map(*p, out.params, e)) { err = e; return false; }
        }

        if (auto* nn = obj_get(o, "numeric_nodes")) {
            std::string e;
            if (!parse_nodes(*nn, out.numeric_nodes, e)) { err = "numeric_nodes: " + e; return false; }
        }

        if (auto* bn = obj_get(o, "bool_nodes")) {
            std::string e;
            if (!parse_nodes(*bn, out.bool_nodes, e)) { err = "bool_nodes: " + e; return false; }
        }

        if (auto* b = obj_get(o, "bindings")) {
            std::string e;
            if (!parse_bindings(*b, out.bind, e)) { err = e; return false; }
        }
        else {
            err = "bindings object is required";
            return false;
        }

        if (auto* sw = obj_get(o, "sweep")) {
            PlanSweepSpec s;
            std::string e;
            if (!parse_sweep_spec(*sw, s, e)) { err = e; return false; }
            out.sweep = std::move(s);
        }

        return true;
    }

    bool load_plan_spec_file(const std::string& path, PlanSpec& out, std::string& err) {
        std::ifstream fp(path, std::ios::binary);
        if (!fp) { err = "Could not open PlanSpec file: " + path; return false; }
        std::ostringstream oss;
        oss << fp.rdbuf();
        const auto s = oss.str();
        return parse_plan_spec_json(s, out, err);
    }

    bool param_is_ref(const ParamValue& v) {
        return std::holds_alternative<ParamRef>(v);
    }

    std::optional<std::string> param_ref_name(const ParamValue& v) {
        if (!std::holds_alternative<ParamRef>(v)) return std::nullopt;
        return std::get<ParamRef>(v).name;
    }

} // namespace sugar
