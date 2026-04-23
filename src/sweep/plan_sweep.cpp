#include "sweep/plan_sweep.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
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

        double compute_score(const BacktestResult& r, const PlanSweepSpec& sw) {
            if (sw.objective == "pnl") return r.pnl;
            // default: pnl_dd
            return r.pnl - sw.dd_weight * r.max_drawdown;
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
        };

        bool build_compiled_plan_runtime_template(
            const PlanSpec& spec,
            const DataView& dv,
            const std::string& default_series,
            CompiledPlanRuntimeTemplate& out,
            std::string& err_out) {
            PlanSpec compile_spec{};
            CompileEnv env{};
            if (!build_compiler_compatible_spec(spec, nullptr, dv, default_series, compile_spec, env, err_out)) {
                return false;
            }

            const auto registry = make_default_registry();
            if (!compile_plan_template(compile_spec, registry, env, out.plan_template, err_out)) {
                return false;
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
            return exec.run(
                dv,
                primary,
                compiled.plan,
                compiled.exec,
                result_out,
                err_out,
                opt.cancel_requested);
        }

    } // namespace

    bool run_custom_plan_spec(
        const DataView& dv,
        const ColumnRef& primary,
        const PlanSpec& spec,
        const PlanSweepRuntimeOptions& opt,
        std::ostream& out,
        BacktestResult* result_out)
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

            const std::size_t total_combos = dims.total_combinations();
            auto points = build_planspec_sweep_points(dims, sw.max_evals);

            std::size_t compile_failures = 0;
            std::string last_compile_error;

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
                    ++compile_failures;
                    last_compile_error = err_run;
                    r.pnl = -std::numeric_limits<double>::infinity();
                    r.max_drawdown = std::numeric_limits<double>::infinity();
                    r.trades = 0;
                    r.trade_hash = 0;
                }
                else {
                    if (!err_run.empty()) last_compile_error = err_run;
                }
                return r;
                };

            auto score_fn = [&](const BacktestResult& r) -> double {
                return compute_score(r, sw);
                };

            auto ostream_progress_cb = make_ostream_progress_cb(out);
            auto progress_cb = [&](const SweepProgress& progress) {
                if (ostream_progress_cb) {
                    ostream_progress_cb(progress);
                }
                if (opt.progress_cb) {
                    opt.progress_cb(progress);
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
                break;
            }
            }

            if (opt.cancel_requested && opt.cancel_requested()) {
                out << "PlanSpec execution canceled.\n";
                return false;
            }

            if (best.size() > sw.top_k) {
                best.resize(sw.top_k);
            }

            out << "\n";

            out << "\n=== PlanSpec Sweep Results ===\n";
            out << "Engine: " << engine_name(sw.engine) << "\n";
            out << "Objective: " << sw.objective;
            if (sw.objective != "pnl") out << " (dd_weight=" << sw.dd_weight << ")";
            out << "\n";
            out << "Param dims: " << dims.names.size() << "\n";
            out << "Total combos: ";
            if (total_combos == std::numeric_limits<std::size_t>::max()) out << "(overflow)\n";
            else out << total_combos << "\n";
            out << "Evals run: " << eval_count << " (max " << sw.max_evals << ")\n";
            out << "Compile failures: " << compile_failures << "\n";
            if (!last_compile_error.empty()) {
                out << "Last compile error: " << last_compile_error << "\n";
            }
            out << "TopK: " << best.size() << "\n\n";

            for (std::size_t i = 0; i < best.size(); ++i) {
                out << "-- Rank " << (i + 1) << " | score=" << best[i].score << " --\n";
                out << "Params:\n";

                if (best[i].index >= points.size()) continue;
                const auto& p = points[best[i].index];

                for (std::size_t d = 0; d < dims.names.size(); ++d) {
                    const auto& name = dims.names[d];
                    const double v = dims.values[d][p[d]];
                    out << "  " << name << ": ";
                    if (dims.is_int[d]) out << static_cast<std::int64_t>(std::llround(v));
                    else out << v;
                    out << "\n";
                }

                out << "Result:\n";
                print_backtest_summary(best[i].result, out, "  ");
                out << "\n";
            }

            if (!best.empty()) {
                if (result_out) {
                    *result_out = best.front().result;
                }
                out << "=== Best Result (Rank 1) ===\n";
                print_backtest_summary(best.front().result, out);
            }
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
        print_backtest_summary(r, out);
        return true;
    }

} // namespace sugar
