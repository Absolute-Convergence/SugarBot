#include "sweep/plan_spec.h"

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

        bool parse_run_scope(const std::string& s, PlanRunScope& out) {
            if (s == "primary_series") {
                out = PlanRunScope::PrimarySeries;
                return true;
            }
            if (s == "per_entity_ranked") {
                out = PlanRunScope::PerEntityRanked;
                return true;
            }
            return false;
        }

        bool parse_objective_value_source(const std::string& s, ObjectiveValueSource& out) {
            if (s == "reported_metric") {
                out = ObjectiveValueSource::ReportedMetric;
                return true;
            }
            if (s == "numeric_node_output") {
                out = ObjectiveValueSource::NumericNodeOutput;
                return true;
            }
            return false;
        }

        bool parse_objective_direction(const std::string& s, ObjectiveDirection& out) {
            if (s == "maximize") {
                out = ObjectiveDirection::Maximize;
                return true;
            }
            if (s == "minimize") {
                out = ObjectiveDirection::Minimize;
                return true;
            }
            return false;
        }

        bool parse_numeric_node_summary_mode(const std::string& s, NumericNodeSummaryMode& out) {
            if (s == "last_finite") {
                out = NumericNodeSummaryMode::LastFiniteValue;
                return true;
            }
            return false;
        }

        bool parse_objective_value_spec(const Value& v, ObjectiveValueSpec& out, std::string& err) {
            if (!v.is_obj()) {
                err = "objective value must be an object";
                return false;
            }
            const auto& o = v.as_obj();

            auto* source = obj_get(o, "source");
            if (!source || !source->is_str()) {
                err = "objective value.source must be string";
                return false;
            }
            if (!parse_objective_value_source(source->as_str(), out.source)) {
                err = "objective value.source must be reported_metric or numeric_node_output";
                return false;
            }

            auto* name = obj_get(o, "name");
            if (!name || !name->is_str()) {
                err = "objective value.name must be string";
                return false;
            }
            out.name = name->as_str();
            if (out.name.empty()) {
                err = "objective value.name must not be empty";
                return false;
            }

            if (auto* summary = obj_get(o, "summary")) {
                if (!summary->is_str()) {
                    err = "objective value.summary must be string";
                    return false;
                }
                if (!parse_numeric_node_summary_mode(summary->as_str(), out.summary_mode)) {
                    err = "objective value.summary must be last_finite";
                    return false;
                }
            }

            return true;
        }

        RankingObjectiveSpec make_default_run_objective_spec() {
            RankingObjectiveSpec objective{};
            objective.primary.source = ObjectiveValueSource::ReportedMetric;
            objective.primary.name = "pnl";
            objective.direction = ObjectiveDirection::Maximize;
            return objective;
        }

        bool parse_compat_run_objective_spec(
            const Object& o,
            RankingObjectiveSpec& out,
            std::string& err) {
            const auto* ranking_metric = obj_get(o, "ranking_metric");
            const auto* ranking_dd_weight = obj_get(o, "ranking_dd_weight");
            if (!ranking_metric && !ranking_dd_weight) {
                return false;
            }

            if (!ranking_metric) {
                err = "run.ranking_dd_weight requires compatibility run.ranking_metric=pnl_dd";
                return false;
            }
            if (!ranking_metric->is_str()) {
                err = "run.ranking_metric must be string";
                return false;
            }

            const std::string metric = ranking_metric->as_str();
            if (metric == "pnl") {
                if (ranking_dd_weight) {
                    err = "run.ranking_dd_weight applies only when compatibility run.ranking_metric=pnl_dd";
                    return false;
                }
                out = make_default_run_objective_spec();
                return true;
            }

            if (metric == "pnl_dd") {
                if (!ranking_dd_weight || !ranking_dd_weight->is_num()) {
                    err = "run.ranking_dd_weight is required when compatibility run.ranking_metric=pnl_dd";
                    return false;
                }
                out = make_default_run_objective_spec();
                ObjectivePenaltySpec penalty{};
                penalty.value.source = ObjectiveValueSource::ReportedMetric;
                penalty.value.name = "max_drawdown";
                penalty.weight = ranking_dd_weight->number_as_f64();
                out.penalties.push_back(std::move(penalty));
                return true;
            }

            err = "run.ranking_metric must be pnl or pnl_dd";
            return false;
        }

        bool parse_objective_penalty_spec(const Value& v, ObjectivePenaltySpec& out, std::string& err) {
            if (!v.is_obj()) {
                err = "objective penalty must be an object";
                return false;
            }
            const auto& o = v.as_obj();
            auto* value = obj_get(o, "value");
            if (!value) {
                err = "objective penalty.value is required";
                return false;
            }
            if (!parse_objective_value_spec(*value, out.value, err)) {
                err = "objective penalty.value: " + err;
                return false;
            }
            auto* weight = obj_get(o, "weight");
            if (!weight || !weight->is_num()) {
                err = "objective penalty.weight must be number";
                return false;
            }
            out.weight = weight->number_as_f64();
            return true;
        }

        bool parse_objective_tie_break_spec(const Value& v, ObjectiveTieBreakSpec& out, std::string& err) {
            if (!v.is_obj()) {
                err = "objective tie_break must be an object";
                return false;
            }
            const auto& o = v.as_obj();
            auto* value = obj_get(o, "value");
            if (!value) {
                err = "objective tie_break.value is required";
                return false;
            }
            if (!parse_objective_value_spec(*value, out.value, err)) {
                err = "objective tie_break.value: " + err;
                return false;
            }
            auto* direction = obj_get(o, "direction");
            if (!direction || !direction->is_str()) {
                err = "objective tie_break.direction must be string";
                return false;
            }
            if (!parse_objective_direction(direction->as_str(), out.direction)) {
                err = "objective tie_break.direction must be maximize or minimize";
                return false;
            }
            return true;
        }

        bool parse_ranking_objective_object_spec(
            const Value& v,
            const char* field_name,
            RankingObjectiveSpec& out,
            std::string& err) {
            if (!v.is_obj()) {
                err = std::string(field_name) + " must be object";
                return false;
            }
            const auto& o = v.as_obj();

            auto* primary = obj_get(o, "value");
            if (!primary) {
                err = std::string(field_name) + ".value is required";
                return false;
            }
            if (!parse_objective_value_spec(*primary, out.primary, err)) {
                err = std::string(field_name) + ".value: " + err;
                return false;
            }

            auto* direction = obj_get(o, "direction");
            if (!direction || !direction->is_str()) {
                err = std::string(field_name) + ".direction must be string";
                return false;
            }
            if (!parse_objective_direction(direction->as_str(), out.direction)) {
                err = std::string(field_name) + ".direction must be maximize or minimize";
                return false;
            }

            if (auto* penalties = obj_get(o, "penalties")) {
                if (!penalties->is_arr()) {
                    err = std::string(field_name) + ".penalties must be array";
                    return false;
                }
                out.penalties.clear();
                for (const auto& item : penalties->as_arr()) {
                    ObjectivePenaltySpec penalty{};
                    if (!parse_objective_penalty_spec(item, penalty, err)) {
                        return false;
                    }
                    out.penalties.push_back(std::move(penalty));
                }
            }

            if (auto* tie_breaks = obj_get(o, "tie_breaks")) {
                if (!tie_breaks->is_arr()) {
                    err = std::string(field_name) + ".tie_breaks must be array";
                    return false;
                }
                out.tie_breaks.clear();
                for (const auto& item : tie_breaks->as_arr()) {
                    ObjectiveTieBreakSpec tie_break{};
                    if (!parse_objective_tie_break_spec(item, tie_break, err)) {
                        return false;
                    }
                    out.tie_breaks.push_back(std::move(tie_break));
                }
            }

            return true;
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
                if (x <= 0) { err = "sweep.top_k must be > 0"; return false; }
                out.top_k = static_cast<std::size_t>(x);
            }
            if (auto* p = obj_get(o, "max_evals")) {
                std::int64_t x = 0;
                if (!as_i64(*p, x)) { err = "sweep.max_evals must be number"; return false; }
                if (x <= 0) { err = "sweep.max_evals must be > 0"; return false; }
                out.max_evals = static_cast<std::size_t>(x);
            }
            if (auto* p = obj_get(o, "objective")) {
                if (p->is_str()) {
                    out.objective = p->as_str();
                }
                else if (p->is_obj()) {
                    RankingObjectiveSpec objective{};
                    if (!parse_ranking_objective_object_spec(*p, "sweep.objective", objective, err)) {
                        return false;
                    }
                    out.objective_spec = std::move(objective);
                }
                else {
                    err = "sweep.objective must be string or object";
                    return false;
                }
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

        bool parse_run_spec(const Value& v, PlanRunSpec& out, std::string& err) {
            if (!v.is_obj()) {
                err = "run must be object";
                return false;
            }
            const auto& o = v.as_obj();

            auto* scope = obj_get(o, "scope");
            if (!scope || !scope->is_str()) {
                err = "run.scope must be string";
                return false;
            }
            if (!parse_run_scope(scope->as_str(), out.scope)) {
                err = "run.scope must be primary_series or per_entity_ranked";
                return false;
            }

            if (auto* p = obj_get(o, "target_entity")) {
                if (!p->is_str()) {
                    err = "run.target_entity must be string";
                    return false;
                }
                out.target_entity = p->as_str();
            }

            if (auto* p = obj_get(o, "top_k")) {
                std::int64_t x = 0;
                if (!as_i64(*p, x)) {
                    err = "run.top_k must be number";
                    return false;
                }
                if (x <= 0) {
                    err = "run.top_k must be > 0";
                    return false;
                }
                out.top_k = static_cast<std::size_t>(x);
            }

            if (auto* p = obj_get(o, "objective")) {
                RankingObjectiveSpec objective{};
                if (!parse_ranking_objective_object_spec(*p, "run.objective", objective, err)) {
                    return false;
                }
                if (obj_get(o, "ranking_metric") || obj_get(o, "ranking_dd_weight")) {
                    err = "run.objective may not be combined with compatibility run.ranking_metric or run.ranking_dd_weight";
                    return false;
                }
                out.objective_spec = std::move(objective);
            } else {
                RankingObjectiveSpec compatibility_objective{};
                if (parse_compat_run_objective_spec(o, compatibility_objective, err)) {
                    out.objective_spec = std::move(compatibility_objective);
                } else if (!err.empty()) {
                    return false;
                }
            }

            return true;
        }
    }

    std::string_view plan_run_scope_name(PlanRunScope scope) {
        switch (scope) {
        case PlanRunScope::PrimarySeries:
            return "primary_series";
        case PlanRunScope::PerEntityRanked:
            return "per_entity_ranked";
        }
        return "unknown";
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

        if (auto* run = obj_get(o, "run")) {
            PlanRunSpec s{};
            std::string e;
            if (!parse_run_spec(*run, s, e)) { err = e; return false; }
            out.run = std::move(s);
        }

        return true;
    }

    bool load_plan_spec_file(const std::string& path, PlanSpec& out, std::string& err) {
        std::ifstream fp(path, std::ios::binary);
        if (!fp) { err = "Could not open PlanSpec file: " + path; return false; }
        std::ostringstream oss;
        oss << fp.rdbuf();
        const auto s = oss.str();
        PlanSpec parsed{};
        if (!parse_plan_spec_json(s, parsed, err)) {
            return false;
        }
        parsed.source_path = path;
        out = std::move(parsed);
        return true;
    }

    bool param_is_ref(const ParamValue& v) {
        return std::holds_alternative<ParamRef>(v);
    }

    std::optional<std::string> param_ref_name(const ParamValue& v) {
        if (!std::holds_alternative<ParamRef>(v)) return std::nullopt;
        return std::get<ParamRef>(v).name;
    }

} // namespace sugar
