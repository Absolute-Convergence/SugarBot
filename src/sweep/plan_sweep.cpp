#include "sweep/plan_sweep.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <span>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ingest/data_view.h"
#include "sweep/metrics.h"
#include "engine/plan_compiler.h"
#include "plan_compiler_internal.h"
#include "ops/op_registry.h"
#include "op_registry_internal.h"
#include "ops/plan_spec_runtime_alias.h"
#include "ingest/run_pipeline.h"
#include "engine/strategy_executor.h"
#include "engine/program_evaluator_internal.h"
#include "sweep/sweep.h"

namespace sugar {

    namespace {
        const char* axis_kind_name(AxisKind k) {
            switch (k) {
            case AxisKind::Time: return "time";
            case AxisKind::Entity: return "entity";
            case AxisKind::Spatial: return "spatial";
            }
            return "unknown";
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

        void normalize_runtime_aliases_in_place(PlanSpec& spec) {
            auto normalize_node = [](NodeSpec& node) {
                node.op = normalize_plan_spec_runtime_op(node.op);
            };

            for (auto& node : spec.numeric_nodes) normalize_node(node);
            for (auto& node : spec.bool_nodes) normalize_node(node);
        }

        bool validate_planspec_runtime_requirements_for_dataview(
            const PlanSpec& spec,
            const DataView& dv,
            const std::string& default_series,
            const ParamMap* overrides,
            std::string& err) {
            const auto registry = make_default_registry();
            const auto idx = dv.index_schema();

            auto validate_node = [&](const NodeSpec& node, const char* kind) -> bool {
                const auto* op = registry.get(node.op);
                if (!op) {
                    err = std::string("node '") + node.id + "' (" + kind + "): unknown op '" + node.op + "'";
                    return false;
                }

                for (const auto axis : op->spec.required_axes) {
                    if (!idx) {
                        err = std::string("node '") + node.id + "' (" + kind + ", op '" + node.op +
                            "'): required axis '" + axis_kind_name(axis) + "' is missing (DataView has no index_schema)";
                        return false;
                    }
                    if (!idx->has_kind(axis)) {
                        err = std::string("node '") + node.id + "' (" + kind + ", op '" + node.op +
                            "'): required axis '" + axis_kind_name(axis) + "' is missing from DataView index_schema";
                        return false;
                    }
                }

                std::string topology_name;
                if (!resolve_bound_topology_name(*op, node.params, spec.params, overrides, topology_name, err)) {
                    err = std::string("node '") + node.id + "' (" + kind + ", op '" + node.op +
                        "'): " + err;
                    return false;
                }

                const bool requires_topology =
                    !op->spec.required_topology_families.empty() ||
                    !op->spec.required_topology_query_kinds.empty() ||
                    !topology_name.empty();
                if (!requires_topology) {
                    return true;
                }

                if (topology_name.empty()) {
                    err = std::string("node '") + node.id + "' (" + kind + ", op '" + node.op +
                        "'): requires a resolvable topology binding";
                    return false;
                }

                const TopologySpec* topology = dv.topology(topology_name);
                if (!topology) {
                    err = std::string("node '") + node.id + "' (" + kind + ", op '" + node.op +
                        "'): required topology '" + topology_name + "' is not available on DataView";
                    return false;
                }
                if (!topology_family_matches(op->spec.required_topology_families, topology->family)) {
                    err = std::string("node '") + node.id + "' (" + kind + ", op '" + node.op +
                        "'): topology '" + topology_name + "' has family '" +
                        std::string(topology_family_name(topology->family)) +
                        "', expected one of {" + join_topology_families(op->spec.required_topology_families) + "}";
                    return false;
                }
                for (const auto query_kind : op->spec.required_topology_query_kinds) {
                    if (!topology->supports_query_kind(query_kind)) {
                        err = std::string("node '") + node.id + "' (" + kind + ", op '" + node.op +
                            "'): topology '" + topology_name + "' does not support query kind '" +
                            std::string(topology_query_kind_name(query_kind)) +
                            "'; required kinds are {" + join_topology_query_kinds(op->spec.required_topology_query_kinds) + "}";
                        return false;
                    }
                }

                RuntimeLocalitySubject runtime_subject{};
                if (!resolve_runtime_locality_subject(
                        *op,
                        node.params,
                        spec.params,
                        overrides,
                        dv,
                        default_series,
                        runtime_subject,
                        err)) {
                    err = std::string("node '") + node.id + "' (" + kind + ", op '" + node.op +
                        "'): locality subject could not be resolved: " + err;
                    return false;
                }

                try {
                    auto runtime_query = dv.resolve_topology(runtime_subject, topology_name);
                    if (!runtime_query) {
                        err = std::string("node '") + node.id + "' (" + kind + ", op '" + node.op +
                            "'): topology '" + topology_name + "' did not resolve to a runtime query";
                        return false;
                    }
                }
                catch (const std::exception& ex) {
                    err = std::string("node '") + node.id + "' (" + kind + ", op '" + node.op +
                        "'): topology '" + topology_name + "' could not be resolved: " + ex.what();
                    return false;
                }
                return true;
            };

            for (const auto& node : spec.numeric_nodes) {
                if (!validate_node(node, "numeric")) return false;
            }
            for (const auto& node : spec.bool_nodes) {
                if (!validate_node(node, "bool")) return false;
            }

            return true;
        }

        const char* engine_name(PlanSweepEngineKind k) {
            switch (k) {
            case PlanSweepEngineKind::CoarseFine: return "coarse_fine";
            case PlanSweepEngineKind::MonteCarlo: return "monte_carlo";
            case PlanSweepEngineKind::Exhaustive:
            default: return "exhaustive";
            }
        }

        RankingObjectiveSpec resolve_sweep_objective(const PlanSweepSpec& sw) {
            if (sw.objective_spec.has_value()) {
                return *sw.objective_spec;
            }

            RankingObjectiveSpec objective{};
            objective.primary.source = ObjectiveValueSource::ReportedMetric;
            objective.primary.name = "pnl";
            objective.direction = ObjectiveDirection::Maximize;

            if (sw.objective == "pnl") {
                return objective;
            }

            ObjectivePenaltySpec penalty{};
            penalty.value.source = ObjectiveValueSource::ReportedMetric;
            penalty.value.name = "max_drawdown";
            penalty.weight = sw.dd_weight;
            objective.penalties.push_back(std::move(penalty));
            return objective;
        }

        bool ranking_objective_needs_numeric_outputs(const RankingObjectiveSpec& objective) {
            if (objective.primary.source == ObjectiveValueSource::NumericNodeOutput) {
                return true;
            }
            for (const auto& penalty : objective.penalties) {
                if (penalty.value.source == ObjectiveValueSource::NumericNodeOutput) {
                    return true;
                }
            }
            for (const auto& tie_break : objective.tie_breaks) {
                if (tie_break.value.source == ObjectiveValueSource::NumericNodeOutput) {
                    return true;
                }
            }
            return false;
        }

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
                const double summarized = summarize_numeric_output(slot, NumericNodeSummaryMode::LastFiniteValue);
                result_out.reported_values.push_back(NamedReportedValue{ output.node_id, summarized });
            }
        }

        ParamValue make_sweep_param_value(double value, bool is_int) {
            if (is_int) {
                return static_cast<std::int64_t>(std::llround(value));
            }
            return value;
        }

        std::string param_value_to_display_string(const ParamValue& value) {
            return std::visit([](const auto& x) -> std::string {
                using X = std::decay_t<decltype(x)>;
                if constexpr (std::is_same_v<X, std::nullptr_t>) {
                    return "null";
                }
                else if constexpr (std::is_same_v<X, bool>) {
                    return x ? "true" : "false";
                }
                else if constexpr (std::is_same_v<X, std::int64_t>) {
                    return std::to_string(x);
                }
                else if constexpr (std::is_same_v<X, double>) {
                    std::ostringstream oss;
                    oss << x;
                    return oss.str();
                }
                else if constexpr (std::is_same_v<X, std::string>) {
                    return x;
                }
                else {
                    return std::string("ref:") + x.name;
                }
            }, value);
        }

        std::vector<SweepParameterSetting> make_candidate_parameter_values(
            const PlanSpecSweepDims& dims,
            const std::vector<std::vector<std::size_t>>& points,
            std::size_t candidate_index) {
            std::vector<SweepParameterSetting> out;
            if (candidate_index >= points.size()) {
                return out;
            }

            const auto& point = points[candidate_index];
            out.reserve(dims.names.size());
            for (std::size_t i = 0; i < dims.names.size() && i < point.size(); ++i) {
                const auto value_index = point[i];
                if (i >= dims.values.size() || value_index >= dims.values[i].size()) {
                    continue;
                }
                SweepParameterSetting setting{};
                setting.name = dims.names[i];
                setting.value = make_sweep_param_value(dims.values[i][value_index], dims.is_int[i]);
                out.push_back(std::move(setting));
            }
            return out;
        }

        void populate_candidate_parameter_values(
            const PlanSpecSweepDims& dims,
            const std::vector<std::vector<std::size_t>>& points,
            std::vector<SweepRankedCandidate>& candidates) {
            for (auto& candidate : candidates) {
                candidate.parameter_values = make_candidate_parameter_values(dims, points, candidate.candidate_index);
            }
        }

        std::vector<SweepRankedCandidate> make_sweep_ranked_candidates(
            const std::vector<IndexedCandidate>& best,
            const PlanSpecSweepDims& dims,
            const std::vector<std::vector<std::size_t>>& points) {
            std::vector<SweepRankedCandidate> out;
            out.reserve(best.size());
            for (const auto& candidate : best) {
                SweepRankedCandidate ranked{};
                ranked.candidate_index = candidate.index;
                ranked.score = candidate.score;
                ranked.result = candidate.result;
                ranked.parameter_values = make_candidate_parameter_values(dims, points, candidate.index);
                out.push_back(std::move(ranked));
            }
            return out;
        }

        std::vector<SweepParameterRangeSummary> build_parameter_range_summaries(
            const PlanSweepSpec& sweep,
            const PlanSpecSweepDims& dims) {
            std::vector<SweepParameterRangeSummary> out;
            out.reserve(dims.names.size());
            for (std::size_t i = 0; i < dims.names.size(); ++i) {
                SweepParameterRangeSummary summary{};
                summary.name = dims.names[i];
                if (const auto it = sweep.params.find(summary.name); it != sweep.params.end()) {
                    summary.requested_range = it->second;
                }
                summary.expanded_count = (i < dims.values.size()) ? dims.values[i].size() : 0;
                out.push_back(std::move(summary));
            }
            return out;
        }

        void upsert_stage_summary(
            PlanSweepReport& report,
            SweepProgressStage stage,
            std::size_t done,
            std::size_t total) {
            auto it = std::find_if(
                report.stage_summaries.begin(),
                report.stage_summaries.end(),
                [stage](const SweepStageSummary& summary) {
                    return summary.stage == stage;
                });
            if (it == report.stage_summaries.end()) {
                report.stage_summaries.push_back(SweepStageSummary{ stage, done, total });
                return;
            }
            it->done = done;
            it->total = total;
        }

        std::string trim_copy(std::string s) {
            auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
            while (!s.empty() && is_space(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
            while (!s.empty() && is_space(static_cast<unsigned char>(s.back()))) s.pop_back();
            return s;
        }

        bool series_alias_and_col(const std::string& token,
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

        bool resolve_col_ref_token(const std::string& token,
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

        std::optional<ParamValue> find_param_value(const ParamMap& plan_params,
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

        bool resolve_param_to_string(const ParamValue& v,
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
                    // In multi-series mode, unresolved refs are allowed as direct column tokens.
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

        bool get_node_param(const NodeSpec& node,
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

            auto ensure_leaf_node_for_token = [&](const std::string& token, std::string& out_id) -> bool {
                std::string key;
                std::string e;
                if (!resolve_token_to_leaf_key(token, default_series, dv, key, e)) {
                    err = e;
                    return false;
                }
                if (auto it = leaf_key_to_node_id.find(key); it != leaf_key_to_node_id.end()) {
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
                else if ((node.op == "ms.diff" || node.op == "ms.ratio") && node.args.empty()) {
                    ParamValue pa{};
                    ParamValue pb{};
                    if (!get_node_param(src, "a", pa) || !get_node_param(src, "b", pb)) {
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

            // Build compile env leaves from all resolved DataView numeric columns.
            for (std::size_t si = 0; si < dv.bundle().series_keys.size() && si < dv.bundle().frames.size(); ++si) {
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
                    storage->numeric_leaf_values[leaf_key] = std::move(values);
                }
            }

            return true;
        }

        struct CompiledPlanRuntimeTemplate {
            CompiledPlanTemplate plan_template{};
            std::vector<NamedNumericOutput> numeric_outputs{};
        };

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

    } // namespace

    void print_plan_sweep_report(
        const PlanSweepReport& report,
        std::ostream& out)
    {
        auto print_requested_range = [&out](const SweepParameterRangeSummary& range) {
            out << "  " << range.name << ": ";
            if (!range.requested_range.values.empty()) {
                out << "[";
                for (std::size_t i = 0; i < range.requested_range.values.size(); ++i) {
                    if (i) out << ", ";
                    if (range.requested_range.is_int) {
                        out << static_cast<std::int64_t>(std::llround(range.requested_range.values[i]));
                    }
                    else {
                        out << range.requested_range.values[i];
                    }
                }
                out << "]";
            }
            else {
                out << range.requested_range.min
                    << " .. " << range.requested_range.max
                    << " step " << range.requested_range.step;
            }
            out << " (" << range.expanded_count << " values)\n";
        };

        auto print_candidate = [&out, &report](const SweepRankedCandidate& candidate, std::size_t rank) {
            out << "-- Rank " << rank << " | score=" << candidate.score << " --\n";
            out << "Params:\n";
            for (const auto& setting : candidate.parameter_values) {
                out << "  " << setting.name << ": "
                    << param_value_to_display_string(setting.value) << "\n";
            }
            out << "Run report:\n";
            print_run_report_summary(
                build_run_report(candidate.result, &report.resolved_objective),
                out,
                "  ");
            out << "\n";
        };

        out << "\n=== PlanSpec Sweep Results ===\n";
        out << "Engine: " << engine_name(report.requested_sweep.engine) << "\n";
        out << "Objective: " << describe_ranking_objective(report.resolved_objective) << "\n";
        out << "Top K: " << report.requested_sweep.top_k << "\n";
        out << "Max evals: " << report.requested_sweep.max_evals << "\n";
        out << "Param dims: " << report.parameter_count << "\n";
        out << "Total combos: ";
        if (report.total_combinations_overflow) {
            out << "(overflow)\n";
        }
        else {
            out << report.total_combinations << "\n";
        }
        out << "Candidate space: " << report.candidate_space_size;
        if (report.candidate_space_truncated) {
            out << " (truncated)";
        }
        out << "\n";
        out << "Evals run: " << report.evaluations_run << "\n";
        out << "Execution failures: " << report.execution_failures << "\n";
        out << "Compile failures: " << report.compile_failures << "\n";
        if (report.canceled) {
            out << "Canceled: true\n";
        }
        print_run_provenance_summary(report.run_provenance, out);
        out << "Candidate outcomes:\n"
            << "  completed: " << report.candidate_outcomes.completed << "\n"
            << "  completed_with_non_finite_metrics: " << report.candidate_outcomes.completed_with_non_finite_metrics << "\n"
            << "  failed: " << report.candidate_outcomes.failed << "\n"
            << "  canceled: " << report.candidate_outcomes.canceled << "\n"
            << "Objective rankability:\n"
            << "  not_evaluated: " << report.candidate_rankability.not_evaluated << "\n"
            << "  rankable: " << report.candidate_rankability.rankable << "\n"
            << "  missing_primary_value: " << report.candidate_rankability.missing_primary_value << "\n"
            << "  non_finite_primary_value: " << report.candidate_rankability.non_finite_primary_value << "\n"
            << "  missing_penalty_value: " << report.candidate_rankability.missing_penalty_value << "\n"
            << "  non_finite_penalty_value: " << report.candidate_rankability.non_finite_penalty_value << "\n";
        if (!report.last_execution_failure.empty()) {
            out << "Last execution failure: " << report.last_execution_failure << "\n";
        }
        if (!report.last_compile_error.empty()) {
            out << "Last compile error: " << report.last_compile_error << "\n";
        }

        if (!report.stage_summaries.empty()) {
            out << "Stages:\n";
            for (const auto& stage : report.stage_summaries) {
                out << "  " << sweep_progress_stage_name(stage.stage)
                    << ": " << stage.done << "/" << stage.total << "\n";
            }
        }

        if (!report.parameter_ranges.empty()) {
            out << "Sweep params:\n";
            for (const auto& range : report.parameter_ranges) {
                print_requested_range(range);
            }
        }

        switch (report.requested_sweep.engine) {
        case PlanSweepEngineKind::CoarseFine:
            out << "Engine settings:\n"
                << "  stage 1 K: " << report.requested_sweep.coarse_fine.k_stage1 << "\n"
                << "  stage 2 K: " << report.requested_sweep.coarse_fine.k_stage2 << "\n"
                << "  stage 1 stride: " << report.requested_sweep.coarse_fine.stage1_stride << "\n"
                << "  suppression hops: " << report.requested_sweep.coarse_fine.suppression_hops << "\n"
                << "  stage 2 hops: " << report.requested_sweep.coarse_fine.stage2_hops << "\n"
                << "  depollute stage 1: " << (report.requested_sweep.coarse_fine.depollute_stage1 ? "true" : "false") << "\n"
                << "  dedupe stage 2: " << (report.requested_sweep.coarse_fine.dedupe_stage2 ? "true" : "false") << "\n";
            break;
        case PlanSweepEngineKind::MonteCarlo:
            out << "Engine settings:\n"
                << "  draws: " << report.requested_sweep.monte_carlo.draws << "\n"
                << "  seed: " << report.requested_sweep.monte_carlo.seed << "\n"
                << "  dedupe: " << (report.requested_sweep.monte_carlo.dedupe ? "true" : "false") << "\n"
                << "  monte carlo top K: " << report.requested_sweep.monte_carlo.top_k << "\n";
            break;
        case PlanSweepEngineKind::Exhaustive:
            break;
        }

        out << "TopK: " << report.top_candidates.size() << "\n";
        if (!report.top_candidates.empty()) {
            out << "Ranked candidate reports:\n\n";
        }
        for (std::size_t i = 0; i < report.top_candidates.size(); ++i) {
            print_candidate(report.top_candidates[i], i + 1);
        }

        if (!report.top_candidates.empty()) {
            out << "=== Best Candidate Run Report (Rank 1) ===\n";
            print_run_report_summary(
                build_run_report(
                    report.top_candidates.front().result,
                    &report.resolved_objective),
                out);
        }
    }

    bool run_custom_plan_spec(
        const DataView& dv,
        const ColumnRef& primary,
        const PlanSpec& spec,
        const PlanSweepRuntimeOptions& opt,
        std::ostream& out,
        BacktestResult* result_out,
        PlanSweepReport* sweep_report_out,
        const RunProvenanceSummary* run_provenance)
    {
        if (!dv.has_col(primary)) {
            out << "PlanSpec input error: primary series/column is missing from DataView.\n";
            return false;
        }
        if (opt.cancel_requested && opt.cancel_requested()) {
            out << "PlanSpec execution canceled.\n";
            return false;
        }
        PlanSpec normalized_spec = spec;
        normalize_runtime_aliases_in_place(normalized_spec);
        const bool has_sweep =
            normalized_spec.sweep &&
            (normalized_spec.sweep->enabled || !normalized_spec.sweep->params.empty());
        const RankingObjectiveSpec sweep_objective =
            has_sweep ? resolve_sweep_objective(*normalized_spec.sweep) : RankingObjectiveSpec{};
        const bool collect_numeric_outputs =
            has_sweep && ranking_objective_needs_numeric_outputs(sweep_objective);
        if (sweep_report_out) {
            *sweep_report_out = PlanSweepReport{};
        }
        const std::string default_series = primary.series;
        std::string runtime_validation_err;
        if (!validate_planspec_runtime_requirements_for_dataview(
            normalized_spec, dv, default_series, nullptr, runtime_validation_err)) {
            out << "PlanSpec runtime validation failed: " << runtime_validation_err << "\n";
            return false;
        }
        const ColumnId primary_col = primary.col;

        CompiledPlanRuntimeTemplate runtime_template{};
        std::string compile_err;
        if (!build_compiled_plan_runtime_template(
            normalized_spec,
            dv,
            default_series,
            collect_numeric_outputs,
            runtime_template,
            compile_err)) {
            out << "PlanSpec compile failed: " << compile_err << "\n";
            return false;
        }
        if (opt.cancel_requested && opt.cancel_requested()) {
            out << "PlanSpec execution canceled.\n";
            return false;
        }

        auto run_one = [&](const ParamMap* overrides, BacktestResult& result_out, std::string& err_out) -> bool {
            if (!validate_planspec_runtime_requirements_for_dataview(
                normalized_spec, dv, default_series, overrides, err_out)) {
                return false;
            }
            return run_instantiated_plan_once(
                runtime_template,
                overrides,
                dv,
                default_series,
                primary_col,
                opt,
                result_out,
                err_out);
        };

        if (normalized_spec.sweep && (normalized_spec.sweep->enabled || !normalized_spec.sweep->params.empty())) {
            const PlanSweepSpec& sw = *normalized_spec.sweep;

            PlanSpecSweepDims dims;
            std::string sweep_err;
            if (!build_planspec_sweep_dims(sw, dims, sweep_err)) {
                out << "PlanSpec sweep error: " << sweep_err << "\n";
                return false;
            }

            const auto sweep_started = std::chrono::steady_clock::now();
            const std::size_t total_combos = dims.total_combinations();
            auto points = build_planspec_sweep_points(dims, sw.max_evals);

            PlanSweepReport sweep_report{};
            sweep_report.requested_sweep = sw;
            sweep_report.resolved_objective = sweep_objective;
            if (run_provenance != nullptr) {
                sweep_report.run_provenance = *run_provenance;
            }
            sweep_report.parameter_ranges = build_parameter_range_summaries(sw, dims);
            sweep_report.parameter_count = dims.names.size();
            sweep_report.total_combinations_overflow =
                (total_combos == std::numeric_limits<std::size_t>::max());
            if (!sweep_report.total_combinations_overflow) {
                sweep_report.total_combinations = total_combos;
            }
            sweep_report.candidate_space_size = points.size();
            sweep_report.candidate_space_truncated =
                sweep_report.total_combinations_overflow || points.size() < total_combos;

            std::size_t compile_failures = 0;
            std::string last_compile_error;
            std::size_t execution_failures = 0;
            std::string last_execution_failure;

            using IndexVec = std::vector<std::size_t>;

            auto eval_index = [&](std::size_t idx) -> BacktestResult {
                if (idx >= points.size()) {
                    BacktestResult bad{};
                    bad.pnl = -std::numeric_limits<double>::infinity();
                    bad.max_drawdown = std::numeric_limits<double>::infinity();
                    return bad;
                }

                const IndexVec& p = points[idx];
                ParamMap overrides{};

                for (std::size_t i = 0; i < dims.names.size(); ++i) {
                    const std::string& name = dims.names[i];
                    const double v = dims.values[i][p[i]];
                    if (dims.is_int[i]) overrides[name] = static_cast<std::int64_t>(std::llround(v));
                    else overrides[name] = v;
                }

                BacktestResult r{};
                std::string err_run;
                if (!run_one(&overrides, r, err_run)) {
                    const bool canceled =
                        (opt.cancel_requested && opt.cancel_requested()) ||
                        err_run.find("canceled") != std::string::npos;
                    if (canceled) {
                        increment_run_outcome_counts(
                            sweep_report.candidate_outcomes,
                            RunOutcomeState::Canceled);
                    }
                    else {
                        ++compile_failures;
                        ++execution_failures;
                        last_compile_error = err_run;
                        last_execution_failure = err_run;
                        increment_run_outcome_counts(
                            sweep_report.candidate_outcomes,
                            RunOutcomeState::Failed);
                    }
                    increment_run_rankability_counts(
                        sweep_report.candidate_rankability,
                        RunRankabilityState::NotEvaluated);
                    r.pnl = -std::numeric_limits<double>::infinity();
                    r.max_drawdown = std::numeric_limits<double>::infinity();
                    r.trades = 0;
                    r.trade_hash = 0;
                }
                else {
                    if (!err_run.empty()) last_compile_error = err_run;
                    const RunReport report =
                        build_run_report(r, &sweep_objective, run_provenance);
                    increment_run_outcome_counts(
                        sweep_report.candidate_outcomes,
                        report.outcome);
                    increment_run_rankability_counts(
                        sweep_report.candidate_rankability,
                        report.objective.rankability);
                }
                return r;
                };

            auto score_fn = [&](const BacktestResult& r) -> double {
                return evaluate_ranking_objective(sweep_objective, r);
                };

            auto ostream_progress_cb = make_ostream_progress_cb(
                out,
                std::chrono::milliseconds(opt.ostream_progress_throttle_ms));
            auto progress_cb = [&](const SweepProgress& progress) {
                SweepProgress enriched = progress;
                populate_candidate_parameter_values(dims, points, enriched.current_best);
                upsert_stage_summary(
                    sweep_report,
                    enriched.stage,
                    enriched.stage_done,
                    enriched.stage_total);
                if (!enriched.current_best.empty()) {
                    sweep_report.top_candidates = enriched.current_best;
                }
                if (ostream_progress_cb) {
                    ostream_progress_cb(enriched);
                }
                if (opt.progress_cb) {
                    opt.progress_cb(enriched);
                }
            };
            std::vector<IndexedCandidate> best;
            std::size_t eval_count = 0;

            switch (sw.engine) {
            case PlanSweepEngineKind::Exhaustive: {
                auto eval_token = [&](const SweepCandidateToken& token) -> BacktestResult {
                    return eval_index(token.id);
                    };

                DenseIndexSweepSpace space(points.size());
                FunctionTrialEvaluator evaluator(eval_token);
                FunctionObjective objective(score_fn);
                ExhaustiveIndexTopKPolicy policy;
                SweepSessionConfig session_cfg{};
                session_cfg.top_k = sw.top_k;
                session_cfg.progress_cb = progress_cb;
                session_cfg.cancel_cb = opt.cancel_requested;

                auto session = run_sweep_session(
                    space,
                    evaluator,
                    objective,
                    policy,
                    session_cfg);

                best = std::move(session.best_first);
                eval_count = session.eval_count;
                upsert_stage_summary(
                    sweep_report,
                    SweepProgressStage::CandidateEvaluation,
                    session.eval_count,
                    session.total_candidates);
                break;
            }

            case PlanSweepEngineKind::CoarseFine:
            case PlanSweepEngineKind::MonteCarlo: {
                SweepIndexConfig cfg{};
                SweepIndexProblem problem{};
                problem.param_count = points.size();
                problem.eval = [&](std::size_t idx) -> BacktestResult {
                    return eval_index(idx);
                    };

                if (sw.engine == PlanSweepEngineKind::CoarseFine) {
                    if (total_combos == std::numeric_limits<std::size_t>::max() || points.size() != total_combos) {
                        out << "PlanSpec sweep error: coarse_fine requires full cartesian grid "
                            << "(set max_evals to total combinations).\n";
                        return false;
                    }

                    const std::size_t nd = dims.values.size();
                    std::vector<std::size_t> radices;
                    radices.reserve(nd);
                    for (const auto& v : dims.values) radices.push_back(v.size());

                    auto unpack = [radices, nd](std::size_t idx, std::vector<std::size_t>& coords) {
                        coords.assign(nd, 0);
                        for (std::size_t d = 0; d < nd; ++d) {
                            const auto r = radices[d];
                            if (r == 0) continue;
                            coords[d] = idx % r;
                            idx /= r;
                        }
                        };

                    auto pack = [radices, nd](const std::vector<std::size_t>& coords) -> std::size_t {
                        std::size_t mult = 1;
                        std::size_t out_idx = 0;
                        for (std::size_t d = 0; d < nd; ++d) {
                            out_idx += coords[d] * mult;
                            mult *= radices[d];
                        }
                        return out_idx;
                        };

                    problem.neighbors = [unpack, pack, radices, nd](std::size_t idx) -> std::vector<std::size_t> {
                        std::vector<std::size_t> coords;
                        unpack(idx, coords);

                        std::vector<std::size_t> out;
                        out.reserve(1 + 2 * nd);
                        out.push_back(idx);

                        for (std::size_t d = 0; d < nd; ++d) {
                            if (coords[d] > 0) {
                                auto c = coords;
                                --c[d];
                                out.push_back(pack(c));
                            }
                            if (coords[d] + 1 < radices[d]) {
                                auto c = coords;
                                ++c[d];
                                out.push_back(pack(c));
                            }
                        }
                        return out;
                        };

                    cfg.kind = SweepIndexKind::CoarseFine;
                    cfg.coarse_fine.k_stage1 = sw.coarse_fine.k_stage1;
                    cfg.coarse_fine.k_stage2 = sw.coarse_fine.k_stage2;
                    cfg.coarse_fine.opt.depollute_stage1 = sw.coarse_fine.depollute_stage1;
                    cfg.coarse_fine.opt.dedupe_stage2 = sw.coarse_fine.dedupe_stage2;
                    cfg.coarse_fine.opt.suppression_hops = sw.coarse_fine.suppression_hops;
                    cfg.coarse_fine.opt.stage2_hops = sw.coarse_fine.stage2_hops;

                    if (sw.coarse_fine.stage1_stride > 1) {
                        auto stage1_indices = std::make_shared<std::vector<std::size_t>>();
                        stage1_indices->reserve(points.size() / sw.coarse_fine.stage1_stride + 1);
                        for (std::size_t i = 0; i < points.size(); i += sw.coarse_fine.stage1_stride) {
                            stage1_indices->push_back(i);
                        }
                        cfg.coarse_fine.stage1_indices = stage1_indices;
                    }
                }
                else {
                    cfg.kind = SweepIndexKind::MonteCarlo;
                    cfg.monte_carlo.draws = sw.monte_carlo.draws;
                    cfg.monte_carlo.seed = sw.monte_carlo.seed;
                    cfg.monte_carlo.dedupe = sw.monte_carlo.dedupe;
                    cfg.monte_carlo.top_k = (sw.monte_carlo.top_k > 0) ? sw.monte_carlo.top_k : sw.top_k;
                }

                auto sweep = make_sweep_index(cfg);
                const auto run = sweep->run(problem, score_fn, progress_cb, opt.cancel_requested);
                best = run.has_two_stage ? run.two_stage.stage2_best_first : run.best_first;
                eval_count = run.eval_count;
                if (run.has_two_stage) {
                    upsert_stage_summary(
                        sweep_report,
                        SweepProgressStage::CoarseSearch,
                        run.two_stage.stage1_evaluated,
                        run.two_stage.stage1_evaluated);
                    upsert_stage_summary(
                        sweep_report,
                        SweepProgressStage::LocalRefinement,
                        run.two_stage.stage2_evaluated,
                        run.two_stage.stage2_evaluated);
                }
                else if (sw.engine == PlanSweepEngineKind::MonteCarlo) {
                    upsert_stage_summary(
                        sweep_report,
                        SweepProgressStage::MonteCarloSampling,
                        run.eval_count,
                        cfg.monte_carlo.draws);
                }
                break;
            }
            }

            sweep_report.compile_failures = compile_failures;
            sweep_report.execution_failures = execution_failures;
            sweep_report.last_execution_failure = last_execution_failure;
            sweep_report.last_compile_error = last_compile_error;
            sweep_report.evaluations_run = eval_count;

            if (opt.cancel_requested && opt.cancel_requested()) {
                sweep_report.canceled = true;
                if (sweep_report_out) {
                    *sweep_report_out = sweep_report;
                }
                out << "PlanSpec execution canceled.\n";
                return false;
            }

            std::sort(best.begin(), best.end(),
                [&](const IndexedCandidate& a, const IndexedCandidate& b) {
                    if (a.score != b.score) return a.score > b.score;
                    if (ranking_objective_prefers_left(sweep_objective, a.result, b.result)) return true;
                    if (ranking_objective_prefers_left(sweep_objective, b.result, a.result)) return false;
                    return a.index < b.index;
                });

            if (best.size() > sw.top_k) {
                best.resize(sw.top_k);
            }

            const auto after_engine = std::chrono::steady_clock::now();
            const double ranking_elapsed =
                std::chrono::duration<double>(after_engine - sweep_started).count();
            const auto ranked_candidates = make_sweep_ranked_candidates(best, dims, points);
            progress_cb(make_sweep_progress(
                eval_count,
                points.size(),
                ranking_elapsed,
                0.0,
                SweepProgressStage::Ranking,
                1,
                1,
                ranked_candidates));

            sweep_report.top_candidates = ranked_candidates;
            progress_cb(make_sweep_progress(
                eval_count,
                points.size(),
                ranking_elapsed,
                0.0,
                SweepProgressStage::ReportAssembly,
                1,
                1,
                sweep_report.top_candidates));

            if (!sweep_report.top_candidates.empty() && result_out) {
                *result_out = sweep_report.top_candidates.front().result;
            }
            if (sweep_report_out) {
                *sweep_report_out = sweep_report;
            }
            print_plan_sweep_report(sweep_report, out);
            return true;
        }

        BacktestResult r;
        std::string e;
        if (!run_one(nullptr, r, e)) {
            if (opt.cancel_requested && opt.cancel_requested()) {
                out << "PlanSpec execution canceled.\n";
            }
            else {
                out << "PlanSpec compile failed: " << e << "\n";
            }
            return false;
        }
        if (result_out) {
            *result_out = r;
        }
        print_run_report_summary(
            build_run_report(r, nullptr, run_provenance),
            out);
        return true;
    }

} // namespace sugar
