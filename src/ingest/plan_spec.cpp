#include "sweep/plan_spec.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <sstream>
#include <string_view>
#include <utility>

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
            if (s == "successive_halving") { out = PlanSweepEngineKind::SuccessiveHalving; return true; }
            if (s == "bayesian_posterior" || s == "bayesian" || s == "posterior") {
                out = PlanSweepEngineKind::BayesianPosterior;
                return true;
            }
            return false;
        }

        bool parse_posterior_surrogate_kind(
            const std::string& s,
            PlanSweepPosteriorSurrogateKind& out) {
            if (s == "gaussian_process") {
                out = PlanSweepPosteriorSurrogateKind::GaussianProcess;
                return true;
            }
            return false;
        }

        bool parse_posterior_acquisition_kind(
            const std::string& s,
            PlanSweepPosteriorAcquisitionKind& out) {
            if (s == "expected_improvement") {
                out = PlanSweepPosteriorAcquisitionKind::ExpectedImprovement;
                return true;
            }
            if (s == "probability_of_improvement") {
                out = PlanSweepPosteriorAcquisitionKind::ProbabilityOfImprovement;
                return true;
            }
            if (s == "upper_confidence_bound") {
                out = PlanSweepPosteriorAcquisitionKind::UpperConfidenceBound;
                return true;
            }
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

        bool parse_execution_mode(const std::string& s, PlanExecutionMode& out) {
            if (s == "static") {
                out = PlanExecutionMode::Static;
                return true;
            }
            if (s == "declared_dynamic") {
                out = PlanExecutionMode::DeclaredDynamic;
                return true;
            }
            return false;
        }

        bool parse_string_array_field(
            const Value& v,
            std::vector<std::string>& out,
            const std::string& field_name,
            std::string& err) {
            if (!v.is_arr()) {
                err = field_name + " must be array";
                return false;
            }
            out.clear();
            for (const auto& item : v.as_arr()) {
                if (!item.is_str()) {
                    err = field_name + " entries must be strings";
                    return false;
                }
                out.push_back(item.as_str());
            }
            return true;
        }

        bool parse_positive_size_field(
            const Value& v,
            std::size_t& out,
            const std::string& field_name,
            std::string& err) {
            std::int64_t n = 0;
            if (!as_i64(v, n)) {
                err = field_name + " must be number";
                return false;
            }
            if (n <= 0) {
                err = field_name + " must be > 0";
                return false;
            }
            out = static_cast<std::size_t>(n);
            return true;
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

        bool parse_result_diversity_config(
            const Value& v,
            const std::string& field_name,
            PlanSweepResultDiversityConfig& out,
            std::string& err) {
            if (!v.is_obj()) {
                err = field_name + " must be object";
                return false;
            }
            const auto& o = v.as_obj();
            out.enabled = true;

            if (auto* x = obj_get(o, "enabled")) {
                if (!x->is_bool()) { err = field_name + ".enabled must be bool"; return false; }
                out.enabled = x->as_bool();
            }
            if (auto* x = obj_get(o, "dedupe_trade_hash")) {
                if (!x->is_bool()) { err = field_name + ".dedupe_trade_hash must be bool"; return false; }
                out.dedupe_trade_hash = x->as_bool();
            }
            if (auto* x = obj_get(o, "basin_aware")) {
                if (!x->is_bool()) { err = field_name + ".basin_aware must be bool"; return false; }
                out.basin_aware = x->as_bool();
            }
            if (auto* x = obj_get(o, "oversample_multiplier")) {
                std::int64_t n = 0;
                if (!as_i64(*x, n)) { err = field_name + ".oversample_multiplier must be number"; return false; }
                if (n <= 0) { err = field_name + ".oversample_multiplier must be > 0"; return false; }
                out.oversample_multiplier = static_cast<std::size_t>(n);
            }
            if (auto* x = obj_get(o, "max_per_basin")) {
                std::int64_t n = 0;
                if (!as_i64(*x, n)) { err = field_name + ".max_per_basin must be number"; return false; }
                if (n <= 0) { err = field_name + ".max_per_basin must be > 0"; return false; }
                out.max_per_basin = static_cast<std::size_t>(n);
            }
            if (auto* x = obj_get(o, "param_bucket_size")) {
                std::int64_t n = 0;
                if (!as_i64(*x, n)) { err = field_name + ".param_bucket_size must be number"; return false; }
                if (n <= 0) { err = field_name + ".param_bucket_size must be > 0"; return false; }
                out.param_bucket_size = static_cast<std::size_t>(n);
            }
            if (auto* x = obj_get(o, "min_param_distance")) {
                std::int64_t n = 0;
                if (!as_i64(*x, n)) { err = field_name + ".min_param_distance must be number"; return false; }
                if (n < 0) { err = field_name + ".min_param_distance must be >= 0"; return false; }
                out.min_param_distance = static_cast<std::size_t>(n);
            }
            if (auto* x = obj_get(o, "min_trades")) {
                std::int64_t n = 0;
                if (!as_i64(*x, n)) { err = field_name + ".min_trades must be number"; return false; }
                if (n < 0) { err = field_name + ".min_trades must be >= 0"; return false; }
                out.min_trades = static_cast<std::size_t>(n);
            }
            return true;
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

        bool parse_objective_vector_component_spec(
            const Value& v,
            ObjectiveVectorComponentSpec& out,
            std::string& err) {
            if (!v.is_obj()) {
                err = "objective vector component must be an object";
                return false;
            }
            const auto& o = v.as_obj();
            if (auto* label = obj_get(o, "label")) {
                if (!label->is_str()) {
                    err = "objective vector component.label must be string";
                    return false;
                }
                out.label = label->as_str();
                if (out.label.empty()) {
                    err = "objective vector component.label must not be empty";
                    return false;
                }
            }
            auto* value = obj_get(o, "value");
            if (!value) {
                err = "objective vector component.value is required";
                return false;
            }
            if (!parse_objective_value_spec(*value, out.value, err)) {
                err = "objective vector component.value: " + err;
                return false;
            }
            auto* direction = obj_get(o, "direction");
            if (!direction || !direction->is_str()) {
                err = "objective vector component.direction must be string";
                return false;
            }
            if (!parse_objective_direction(direction->as_str(), out.direction)) {
                err = "objective vector component.direction must be maximize or minimize";
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
                if (std::string_view(field_name) == "run.objective") {
                    err += "; for ranked pnl shorthand use compatibility run.ranking_metric=\"pnl\", "
                        "or write object-form run.objective with value and direction. "
                        "The string shortcut is only accepted by sweep.objective";
                }
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
            const std::array<std::string_view, 5> valid_keys = {
                "entry_long",
                "exit_long",
                "entry_short",
                "exit_short",
                "allow_trade",
            };
            auto is_valid_key = [&](const std::string& key) {
                const std::string_view key_view(key.data(), key.size());
                return std::find(valid_keys.begin(), valid_keys.end(), key_view) != valid_keys.end();
                };
            for (const auto& kv : o) {
                const auto& key = kv.first;
                if (is_valid_key(key)) continue;
                err = "bindings." + key + " is not supported; valid binding keys are "
                    "entry_long, exit_long, entry_short, exit_short, allow_trade";
                return false;
            }

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
            bool has_explicit_is_int = false;
            if (auto* p = obj_get(o, "is_int")) {
                if (!p->is_bool()) { err = "is_int must be bool"; return false; }
                out.is_int = p->as_bool();
                has_explicit_is_int = true;
            }

            // Discrete values mode
            if (auto* pv = obj_get(o, "values")) {
                if (!pv->is_arr()) { err = "values must be array"; return false; }
                out.values.clear();
                bool has_fractional_literal = false;
                for (const auto& it : pv->as_arr()) {
                    if (!it.is_num()) { err = "values entries must be numbers"; return false; }
                    has_fractional_literal = has_fractional_literal || it.is_f64();
                    out.values.push_back(it.number_as_f64());
                }
                if (!has_explicit_is_int && has_fractional_literal) {
                    out.is_int = false;
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
            bool has_fractional_literal = vmin->is_f64() || vmax->is_f64();
            if (auto* vs = obj_get(o, "step")) {
                if (!vs->is_num()) { err = "step must be number"; return false; }
                has_fractional_literal = has_fractional_literal || vs->is_f64();
                out.step = vs->number_as_f64();
            }
            if (!has_explicit_is_int && has_fractional_literal) {
                out.is_int = false;
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
                    err = "sweep.engine must be one of: exhaustive, coarse_fine, monte_carlo, successive_halving, bayesian_posterior";
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
                    if (auto* x = obj_get(cfo, "coarse_step_multiplier")) {
                        std::int64_t n = 0;
                        if (!as_i64(*x, n)) { err = "sweep.engine_config.coarse_fine.coarse_step_multiplier must be number"; return false; }
                        if (n < 0) { err = "sweep.engine_config.coarse_fine.coarse_step_multiplier must be >= 0"; return false; }
                        out.coarse_fine.coarse_step_multiplier = static_cast<std::size_t>(n);
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
                        out.monte_carlo.draws_explicit = true;
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

                if (auto* sh = obj_get(ec, "successive_halving")) {
                    if (!sh->is_obj()) { err = "sweep.engine_config.successive_halving must be object"; return false; }
                    const auto& sho = sh->as_obj();

                    if (auto* x = obj_get(sho, "rounds")) {
                        std::int64_t n = 0;
                        if (!as_i64(*x, n)) { err = "sweep.engine_config.successive_halving.rounds must be number"; return false; }
                        if (n <= 0) { err = "sweep.engine_config.successive_halving.rounds must be > 0"; return false; }
                        out.successive_halving.rounds = static_cast<std::size_t>(n);
                    }
                    if (auto* x = obj_get(sho, "reduction_factor")) {
                        std::int64_t n = 0;
                        if (!as_i64(*x, n)) { err = "sweep.engine_config.successive_halving.reduction_factor must be number"; return false; }
                        if (n <= 1) { err = "sweep.engine_config.successive_halving.reduction_factor must be > 1"; return false; }
                        out.successive_halving.reduction_factor = static_cast<std::size_t>(n);
                    }
                    if (auto* x = obj_get(sho, "initial_candidates")) {
                        std::int64_t n = 0;
                        if (!as_i64(*x, n)) { err = "sweep.engine_config.successive_halving.initial_candidates must be number"; return false; }
                        if (n < 0) { err = "sweep.engine_config.successive_halving.initial_candidates must be >= 0"; return false; }
                        out.successive_halving.initial_candidates = static_cast<std::size_t>(n);
                    }
                    if (auto* x = obj_get(sho, "initial_stride")) {
                        std::int64_t n = 0;
                        if (!as_i64(*x, n)) { err = "sweep.engine_config.successive_halving.initial_stride must be number"; return false; }
                        if (n < 0) { err = "sweep.engine_config.successive_halving.initial_stride must be >= 0"; return false; }
                        out.successive_halving.initial_stride = static_cast<std::size_t>(n);
                    }
                    if (auto* x = obj_get(sho, "neighborhood_hops")) {
                        std::int64_t n = 0;
                        if (!as_i64(*x, n)) { err = "sweep.engine_config.successive_halving.neighborhood_hops must be number"; return false; }
                        if (n <= 0) { err = "sweep.engine_config.successive_halving.neighborhood_hops must be > 0"; return false; }
                        out.successive_halving.neighborhood_hops = static_cast<std::size_t>(n);
                    }
                    if (auto* x = obj_get(sho, "dedupe_candidates")) {
                        if (!x->is_bool()) { err = "sweep.engine_config.successive_halving.dedupe_candidates must be bool"; return false; }
                        out.successive_halving.dedupe_candidates = x->as_bool();
                    }
                    if (auto* x = obj_get(sho, "top_k")) {
                        std::int64_t n = 0;
                        if (!as_i64(*x, n)) { err = "sweep.engine_config.successive_halving.top_k must be number"; return false; }
                        if (n <= 0) { err = "sweep.engine_config.successive_halving.top_k must be > 0"; return false; }
                        out.successive_halving.top_k = static_cast<std::size_t>(n);
                    }
                }

                if (auto* bp = obj_get(ec, "bayesian_posterior")) {
                    if (!bp->is_obj()) { err = "sweep.engine_config.bayesian_posterior must be object"; return false; }
                    const auto& bpo = bp->as_obj();

                    if (auto* x = obj_get(bpo, "surrogate")) {
                        if (!x->is_str()) { err = "sweep.engine_config.bayesian_posterior.surrogate must be string"; return false; }
                        if (!parse_posterior_surrogate_kind(x->as_str(), out.bayesian_posterior.surrogate)) {
                            err = "sweep.engine_config.bayesian_posterior.surrogate must be gaussian_process";
                            return false;
                        }
                    }
                    if (auto* x = obj_get(bpo, "acquisition")) {
                        if (!x->is_str()) { err = "sweep.engine_config.bayesian_posterior.acquisition must be string"; return false; }
                        if (!parse_posterior_acquisition_kind(x->as_str(), out.bayesian_posterior.acquisition)) {
                            err = "sweep.engine_config.bayesian_posterior.acquisition must be one of: expected_improvement, probability_of_improvement, upper_confidence_bound";
                            return false;
                        }
                    }
                    if (auto* x = obj_get(bpo, "initial_design_size")) {
                        std::int64_t n = 0;
                        if (!as_i64(*x, n)) { err = "sweep.engine_config.bayesian_posterior.initial_design_size must be number"; return false; }
                        if (n < 0) { err = "sweep.engine_config.bayesian_posterior.initial_design_size must be >= 0"; return false; }
                        out.bayesian_posterior.initial_design_size = static_cast<std::size_t>(n);
                    }
                    if (auto* x = obj_get(bpo, "iterations")) {
                        std::int64_t n = 0;
                        if (!as_i64(*x, n)) { err = "sweep.engine_config.bayesian_posterior.iterations must be number"; return false; }
                        if (n < 0) { err = "sweep.engine_config.bayesian_posterior.iterations must be >= 0"; return false; }
                        out.bayesian_posterior.iterations = static_cast<std::size_t>(n);
                    }
                    if (auto* x = obj_get(bpo, "seed")) {
                        std::int64_t n = 0;
                        if (!as_i64(*x, n)) { err = "sweep.engine_config.bayesian_posterior.seed must be number"; return false; }
                        if (n < 0) { err = "sweep.engine_config.bayesian_posterior.seed must be >= 0"; return false; }
                        out.bayesian_posterior.seed = static_cast<std::uint64_t>(n);
                    }
                    if (auto* x = obj_get(bpo, "exploration_weight")) {
                        if (!x->is_num()) { err = "sweep.engine_config.bayesian_posterior.exploration_weight must be number"; return false; }
                        const double value = x->number_as_f64();
                        if (value < 0.0) {
                            err = "sweep.engine_config.bayesian_posterior.exploration_weight must be >= 0";
                            return false;
                        }
                        out.bayesian_posterior.exploration_weight = value;
                    }
                    if (auto* x = obj_get(bpo, "normalize_inputs")) {
                        if (!x->is_bool()) { err = "sweep.engine_config.bayesian_posterior.normalize_inputs must be bool"; return false; }
                        out.bayesian_posterior.normalize_inputs = x->as_bool();
                    }
                    if (auto* x = obj_get(bpo, "top_k")) {
                        std::int64_t n = 0;
                        if (!as_i64(*x, n)) { err = "sweep.engine_config.bayesian_posterior.top_k must be number"; return false; }
                        if (n <= 0) { err = "sweep.engine_config.bayesian_posterior.top_k must be > 0"; return false; }
                        out.bayesian_posterior.top_k = static_cast<std::size_t>(n);
                    }
                }
            }

            if (auto* p = obj_get(o, "result_diversity")) {
                if (!parse_result_diversity_config(
                        *p,
                        "sweep.result_diversity",
                        out.result_diversity,
                        err)) {
                    return false;
                }
            }

            if (auto* p = obj_get(o, "pareto")) {
                if (!p->is_obj()) { err = "sweep.pareto must be object"; return false; }
                const auto& po = p->as_obj();
                out.pareto.enabled = true;

                if (auto* x = obj_get(po, "enabled")) {
                    if (!x->is_bool()) { err = "sweep.pareto.enabled must be bool"; return false; }
                    out.pareto.enabled = x->as_bool();
                }
                if (auto* x = obj_get(po, "use_result_diversity")) {
                    if (!x->is_bool()) { err = "sweep.pareto.use_result_diversity must be bool"; return false; }
                    out.pareto.use_result_diversity = x->as_bool();
                }
                if (auto* x = obj_get(po, "output_depollution")) {
                    PlanSweepResultDiversityConfig config{};
                    if (!parse_result_diversity_config(
                            *x,
                            "sweep.pareto.output_depollution",
                            config,
                            err)) {
                        return false;
                    }
                    out.pareto.output_depollution = std::move(config);
                }
                if (auto* x = obj_get(po, "retained_candidate_limit")) {
                    std::int64_t n = 0;
                    if (!as_i64(*x, n)) { err = "sweep.pareto.retained_candidate_limit must be number"; return false; }
                    if (n <= 0) { err = "sweep.pareto.retained_candidate_limit must be > 0"; return false; }
                    out.pareto.retained_candidate_limit = static_cast<std::size_t>(n);
                    out.pareto.front_row_limit = static_cast<std::size_t>(n);
                }
                if (auto* x = obj_get(po, "front_row_limit")) {
                    std::int64_t n = 0;
                    if (!as_i64(*x, n)) { err = "sweep.pareto.front_row_limit must be number"; return false; }
                    if (n < 0) { err = "sweep.pareto.front_row_limit must be >= 0"; return false; }
                    out.pareto.front_row_limit = static_cast<std::size_t>(n);
                    out.pareto.retained_candidate_limit =
                        out.pareto.front_row_limit == 0
                        ? out.pareto.retained_candidate_limit
                        : out.pareto.front_row_limit;
                }
                if (auto* x = obj_get(po, "primary_sort_index")) {
                    std::int64_t n = 0;
                    if (!as_i64(*x, n)) { err = "sweep.pareto.primary_sort_index must be number"; return false; }
                    if (n < 0) { err = "sweep.pareto.primary_sort_index must be >= 0"; return false; }
                    out.pareto.primary_sort_index = static_cast<std::size_t>(n);
                }
                if (auto* objectives = obj_get(po, "objectives")) {
                    if (!objectives->is_arr()) { err = "sweep.pareto.objectives must be array"; return false; }
                    out.pareto.objectives.clear();
                    for (const auto& item : objectives->as_arr()) {
                        ObjectiveVectorComponentSpec objective{};
                        if (!parse_objective_vector_component_spec(item, objective, err)) {
                            err = "sweep.pareto.objectives: " + err;
                            return false;
                        }
                        out.pareto.objectives.push_back(std::move(objective));
                    }
                }
                if (out.pareto.enabled) {
                    if (out.pareto.objectives.empty()) {
                        err = "sweep.pareto.objectives must contain at least one objective when pareto is enabled";
                        return false;
                    }
                    if (out.pareto.primary_sort_index >= out.pareto.objectives.size()) {
                        err = "sweep.pareto.primary_sort_index must reference an objective";
                        return false;
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

            if (out.engine == PlanSweepEngineKind::MonteCarlo && !out.monte_carlo.draws_explicit) {
                out.monte_carlo.draws = out.max_evals;
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

        bool parse_execution_bounds_spec(
            const Value& v,
            PlanExecutionBoundsSpec& out,
            std::string& err) {
            if (!v.is_obj()) {
                err = "execution.bounds must be object";
                return false;
            }
            const auto& o = v.as_obj();
            if (auto* p = obj_get(o, "max_routes")) {
                if (!parse_positive_size_field(*p, out.max_routes, "execution.bounds.max_routes", err)) return false;
            }
            if (auto* p = obj_get(o, "max_active_subgraphs")) {
                if (!parse_positive_size_field(*p, out.max_active_subgraphs, "execution.bounds.max_active_subgraphs", err)) return false;
            }
            if (auto* p = obj_get(o, "max_selected_outputs")) {
                if (!parse_positive_size_field(*p, out.max_selected_outputs, "execution.bounds.max_selected_outputs", err)) return false;
            }
            if (auto* p = obj_get(o, "decision_sample_limit")) {
                if (!parse_positive_size_field(*p, out.decision_sample_limit, "execution.bounds.decision_sample_limit", err)) return false;
            }
            return true;
        }

        bool parse_execution_subgraphs(
            const Value& v,
            std::vector<PlanExecutionSubgraphSpec>& out,
            std::string& err) {
            if (!v.is_arr()) {
                err = "execution.subgraphs must be array";
                return false;
            }
            out.clear();
            for (const auto& item : v.as_arr()) {
                if (!item.is_obj()) {
                    err = "execution.subgraphs entries must be objects";
                    return false;
                }
                const auto& o = item.as_obj();
                PlanExecutionSubgraphSpec subgraph{};
                auto* id = obj_get(o, "id");
                if (!id || !id->is_str()) {
                    err = "execution.subgraphs[].id must be string";
                    return false;
                }
                subgraph.id = id->as_str();

                auto* nodes = obj_get(o, "nodes");
                if (!nodes) {
                    err = "execution.subgraphs[].nodes is required";
                    return false;
                }
                if (!parse_string_array_field(*nodes, subgraph.nodes, "execution.subgraphs[].nodes", err)) {
                    return false;
                }

                if (auto* depends = obj_get(o, "depends_on")) {
                    if (!parse_string_array_field(*depends, subgraph.depends_on, "execution.subgraphs[].depends_on", err)) {
                        return false;
                    }
                }

                out.push_back(std::move(subgraph));
            }
            return true;
        }

        bool parse_execution_route_cases(
            const Value& v,
            std::vector<PlanExecutionRouteCaseSpec>& out,
            std::string& err) {
            if (!v.is_arr()) {
                err = "execution.routes[].cases must be array";
                return false;
            }
            out.clear();
            for (const auto& item : v.as_arr()) {
                if (!item.is_obj()) {
                    err = "execution.routes[].cases entries must be objects";
                    return false;
                }
                const auto& o = item.as_obj();
                PlanExecutionRouteCaseSpec route_case{};
                auto* id = obj_get(o, "id");
                if (!id || !id->is_str()) {
                    err = "execution.routes[].cases[].id must be string";
                    return false;
                }
                route_case.id = id->as_str();

                auto* when = obj_get(o, "when");
                if (!when || !when->is_bool()) {
                    err = "execution.routes[].cases[].when must be bool";
                    return false;
                }
                route_case.when = when->as_bool();

                auto* activate = obj_get(o, "activate");
                if (!activate) {
                    err = "execution.routes[].cases[].activate is required";
                    return false;
                }
                if (!parse_string_array_field(*activate, route_case.activate_subgraphs, "execution.routes[].cases[].activate", err)) {
                    return false;
                }

                out.push_back(std::move(route_case));
            }
            return true;
        }

        bool parse_execution_routes(
            const Value& v,
            std::vector<PlanExecutionRouteSpec>& out,
            std::string& err) {
            if (!v.is_arr()) {
                err = "execution.routes must be array";
                return false;
            }
            out.clear();
            for (const auto& item : v.as_arr()) {
                if (!item.is_obj()) {
                    err = "execution.routes entries must be objects";
                    return false;
                }
                const auto& o = item.as_obj();
                PlanExecutionRouteSpec route{};
                auto* id = obj_get(o, "id");
                if (!id || !id->is_str()) {
                    err = "execution.routes[].id must be string";
                    return false;
                }
                route.id = id->as_str();

                auto* condition = obj_get(o, "condition");
                if (!condition || !condition->is_str()) {
                    err = "execution.routes[].condition must be string";
                    return false;
                }
                route.condition = condition->as_str();

                auto* cases = obj_get(o, "cases");
                if (!cases) {
                    err = "execution.routes[].cases is required";
                    return false;
                }
                if (!parse_execution_route_cases(*cases, route.cases, err)) {
                    return false;
                }

                auto* fallback = obj_get(o, "fallback");
                if (!fallback) {
                    err = "execution.routes[].fallback is required";
                    return false;
                }
                if (!parse_string_array_field(*fallback, route.fallback_subgraphs, "execution.routes[].fallback", err)) {
                    return false;
                }

                out.push_back(std::move(route));
            }
            return true;
        }

        bool parse_execution_outputs(
            const Value& v,
            std::vector<PlanExecutionOutputSpec>& out,
            std::string& err) {
            if (!v.is_obj()) {
                err = "execution.outputs must be object";
                return false;
            }
            out.clear();
            for (const auto& kv : v.as_obj()) {
                if (!kv.second.is_str()) {
                    err = "execution.outputs." + kv.first + " must be string";
                    return false;
                }
                out.push_back(PlanExecutionOutputSpec{ kv.first, kv.second.as_str() });
            }
            return true;
        }

        bool parse_execution_spec(
            const Value& v,
            PlanExecutionSpec& out,
            std::string& err) {
            if (!v.is_obj()) {
                err = "execution must be object";
                return false;
            }
            const auto& o = v.as_obj();

            if (auto* mode = obj_get(o, "mode")) {
                if (!mode->is_str()) {
                    err = "execution.mode must be string";
                    return false;
                }
                if (!parse_execution_mode(mode->as_str(), out.mode)) {
                    err = "execution.mode must be static or declared_dynamic";
                    return false;
                }
            }

            if (auto* subgraphs = obj_get(o, "subgraphs")) {
                if (!parse_execution_subgraphs(*subgraphs, out.subgraphs, err)) {
                    return false;
                }
            }
            if (auto* routes = obj_get(o, "routes")) {
                if (!parse_execution_routes(*routes, out.routes, err)) {
                    return false;
                }
            }
            if (auto* outputs = obj_get(o, "outputs")) {
                if (!parse_execution_outputs(*outputs, out.outputs, err)) {
                    return false;
                }
            }
            if (auto* bounds = obj_get(o, "bounds")) {
                if (!parse_execution_bounds_spec(*bounds, out.bounds, err)) {
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

    std::string_view plan_execution_mode_name(PlanExecutionMode mode) {
        switch (mode) {
        case PlanExecutionMode::Static:
            return "static";
        case PlanExecutionMode::DeclaredDynamic:
            return "declared_dynamic";
        }
        return "unknown";
    }

    std::string_view plan_sweep_engine_kind_name(PlanSweepEngineKind kind) {
        switch (kind) {
        case PlanSweepEngineKind::Exhaustive:
            return "exhaustive";
        case PlanSweepEngineKind::CoarseFine:
            return "coarse_fine";
        case PlanSweepEngineKind::MonteCarlo:
            return "monte_carlo";
        case PlanSweepEngineKind::SuccessiveHalving:
            return "successive_halving";
        case PlanSweepEngineKind::BayesianPosterior:
            return "bayesian_posterior";
        }
        return "unknown";
    }

    std::string_view plan_sweep_posterior_surrogate_kind_name(
        PlanSweepPosteriorSurrogateKind kind) {
        switch (kind) {
        case PlanSweepPosteriorSurrogateKind::GaussianProcess:
            return "gaussian_process";
        }
        return "unknown";
    }

    std::string_view plan_sweep_posterior_acquisition_kind_name(
        PlanSweepPosteriorAcquisitionKind kind) {
        switch (kind) {
        case PlanSweepPosteriorAcquisitionKind::ExpectedImprovement:
            return "expected_improvement";
        case PlanSweepPosteriorAcquisitionKind::ProbabilityOfImprovement:
            return "probability_of_improvement";
        case PlanSweepPosteriorAcquisitionKind::UpperConfidenceBound:
            return "upper_confidence_bound";
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

        if (auto* execution = obj_get(o, "execution")) {
            PlanExecutionSpec s{};
            std::string e;
            if (!parse_execution_spec(*execution, s, e)) { err = e; return false; }
            out.execution = std::move(s);
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
