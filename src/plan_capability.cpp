#include "plan_capability.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <sstream>
#include <unordered_map>
#include <vector>

#include "plan_spec_runtime_alias.h"

namespace sugar {

    namespace {

        struct RuntimeDiagnosisContext {
            const ResolvedRuntimeInput* runtime{ nullptr };
            std::string default_series{};
        };

        std::string trim_copy(std::string s) {
            auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
            while (!s.empty() && is_space(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
            while (!s.empty() && is_space(static_cast<unsigned char>(s.back()))) s.pop_back();
            return s;
        }

        std::string append_detail(std::string base, const std::string& detail) {
            if (!detail.empty()) {
                base += ": " + detail;
            }
            return base;
        }

        void add_axis_scope(std::vector<OpAxisScope>& scopes, OpAxisScope scope) {
            if (std::find(scopes.begin(), scopes.end(), scope) == scopes.end()) {
                scopes.push_back(scope);
            }
        }

        void add_topology_family(std::vector<TopologyFamily>& families, TopologyFamily family) {
            if (std::find(families.begin(), families.end(), family) == families.end()) {
                families.push_back(family);
            }
        }

        void add_topology_name(std::vector<std::string>& names, std::string_view name) {
            if (name.empty()) return;
            if (std::find(names.begin(), names.end(), name) == names.end()) {
                names.emplace_back(name);
            }
        }

        void add_reason(std::vector<PlanCapabilityReason>& reasons,
            PlanCapabilityReasonCode code,
            AxisKind axis = AxisKind::Time,
            bool has_axis = false,
            std::string detail = {}) {
            const auto it = std::find_if(reasons.begin(), reasons.end(),
                [&](const PlanCapabilityReason& reason) {
                    return reason.code == code &&
                        reason.axis == axis &&
                        reason.has_axis == has_axis &&
                        reason.detail == detail;
                });
            if (it == reasons.end()) {
                reasons.push_back(PlanCapabilityReason{ code, axis, has_axis, std::move(detail) });
            }
        }

        bool facts_have_axis(const ObservationWorldFacts& facts, AxisKind axis) {
            switch (axis) {
            case AxisKind::Time:
                return facts.has_time_axis;
            case AxisKind::Entity:
                return facts.has_entity_axis;
            case AxisKind::Spatial:
                return facts.has_spatial_axis;
            }
            return false;
        }

        const TopologySpec* builtin_topology_from_facts_name(
            const ObservationWorldFacts& facts,
            std::string_view name) {
            if (name == builtin_temporal_topology_name()) {
                if (!facts.has_time_axis) return nullptr;
                static const TopologySpec kSpec = make_builtin_temporal_topology_spec();
                return &kSpec;
            }
            if (name == builtin_spatial_topology_name()) {
                if (!facts.has_spatial_axis) return nullptr;
                static const TopologySpec kSpec = make_builtin_spatial_topology_spec();
                return &kSpec;
            }
            if (name == builtin_spatiotemporal_topology_name()) {
                if (!facts.has_time_axis || !facts.has_spatial_axis) return nullptr;
                static const TopologySpec kSpec = make_builtin_spatiotemporal_topology_spec();
                return &kSpec;
            }
            return nullptr;
        }

        const TopologySpec* find_fact_topology(
            const ObservationWorldFacts& facts,
            std::string_view name) {
            if (const auto* builtin = builtin_topology_from_facts_name(facts, name)) {
                return builtin;
            }

            const auto it = std::find_if(facts.available_topologies.begin(),
                facts.available_topologies.end(),
                [&](const TopologySpec& spec) {
                    return spec.name == name;
                });
            return it == facts.available_topologies.end() ? nullptr : &*it;
        }

        bool facts_have_topology_family(
            const ObservationWorldFacts& facts,
            TopologyFamily family) {
            switch (family) {
            case TopologyFamily::TemporalWindow:
                if (facts.has_time_axis) return true;
                break;
            case TopologyFamily::SpatialMetric2D:
                if (facts.has_spatial_axis) return true;
                break;
            case TopologyFamily::SpatioTemporalComposite:
                if (facts.has_time_axis && facts.has_spatial_axis) return true;
                break;
            default:
                break;
            }

            return std::any_of(facts.available_topologies.begin(),
                facts.available_topologies.end(),
                [&](const TopologySpec& spec) {
                    return spec.family == family;
                });
        }

        std::vector<std::string> list_fact_topology_names(const ObservationWorldFacts& facts) {
            std::vector<std::string> names;
            if (facts.has_time_axis) {
                add_topology_name(names, builtin_temporal_topology_name());
            }
            if (facts.has_spatial_axis) {
                add_topology_name(names, builtin_spatial_topology_name());
            }
            if (facts.has_time_axis && facts.has_spatial_axis) {
                add_topology_name(names, builtin_spatiotemporal_topology_name());
            }
            for (const auto& spec : facts.available_topologies) {
                add_topology_name(names, spec.name);
            }
            return names;
        }

        bool topology_family_matches(
            const std::vector<TopologyFamily>& required,
            TopologyFamily actual) {
            if (required.empty()) return true;
            return std::find(required.begin(), required.end(), actual) != required.end();
        }

        std::string join_strings(const std::vector<std::string>& values) {
            std::string out;
            for (std::size_t i = 0; i < values.size(); ++i) {
                if (i) out += ", ";
                out += values[i];
            }
            return out;
        }

        std::string join_topology_families(const std::vector<TopologyFamily>& families) {
            std::string out;
            for (std::size_t i = 0; i < families.size(); ++i) {
                if (i) out += ", ";
                out += std::string(topology_family_name(families[i]));
            }
            return out;
        }

        std::string topology_provision_hint(
            const ObservationWorldFacts& facts,
            const std::vector<TopologyFamily>& families,
            std::string_view named_topology) {
            std::vector<std::string> hints;
            for (const auto family : families) {
                switch (family) {
                case TopologyFamily::SpatialMetric2D:
                case TopologyFamily::SpatioTemporalComposite:
                    if (!facts.has_spatial_axis) {
                        hints.emplace_back(
                            observation_capability_reason_text(
                                ObservationCapabilityReasonCode::SpatialSchemaCanBeProvided));
                    }
                    break;
                default:
                    break;
                }
            }

            if (hints.empty() && !named_topology.empty()) {
                hints.emplace_back("supply topology '" + std::string(named_topology) + "'");
            }

            std::sort(hints.begin(), hints.end());
            hints.erase(std::unique(hints.begin(), hints.end()), hints.end());
            return join_strings(hints);
        }

        bool op_mentions_topology_or_locality(const OpEntry& op) {
            return !op.spec.required_topology_families.empty() ||
                !op.spec.required_topology_query_kinds.empty() ||
                !op.spec.default_topology_name.empty() ||
                !op.spec.topology_binding_param.empty() ||
                !op.spec.locality_subject_binding_param.empty();
        }

        std::string infer_default_series(const ResolvedRuntimeInput& runtime) {
            if (!runtime.primary_ref.series.empty()) return runtime.primary_ref.series;
            if (runtime.aligned_bundle && !runtime.aligned_bundle->series_keys.empty()) {
                return runtime.aligned_bundle->series_keys.front();
            }
            return {};
        }

        bool resolve_param_to_string_like_runtime(const ParamValue& v,
            const ParamMap& plan_params,
            std::string& out,
            std::string& err) {
            if (std::holds_alternative<std::string>(v)) {
                out = std::get<std::string>(v);
                return true;
            }
            if (std::holds_alternative<ParamRef>(v)) {
                const std::string& ref = std::get<ParamRef>(v).name;
                auto it = plan_params.find(ref);
                if (it == plan_params.end()) {
                    out = ref;
                    return true;
                }
                if (std::holds_alternative<std::string>(it->second)) {
                    out = std::get<std::string>(it->second);
                    return true;
                }
                err = "Param ref '" + ref + "' did not resolve to string";
                return false;
            }
            err = "Expected string or {ref:...}";
            return false;
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

        bool has_series_key(const align::AlignedBundle& bundle, std::string_view series) {
            return bundle.find_series_index(series) != static_cast<std::size_t>(-1);
        }

        bool validate_runtime_token(const std::string& token,
            const RuntimeDiagnosisContext& runtime_ctx,
            std::vector<PlanCapabilityReason>& reasons) {
            if (runtime_ctx.runtime == nullptr || !runtime_ctx.runtime->aligned_bundle) {
                return true;
            }

            const auto& bundle = *runtime_ctx.runtime->aligned_bundle;
            if (!bundle.schema) {
                add_reason(reasons,
                    PlanCapabilityReasonCode::MissingRuntimeColumn,
                    AxisKind::Time,
                    false,
                    "runtime schema is missing");
                return false;
            }

            std::string series;
            std::string col_name;
            if (!series_alias_and_col(token, runtime_ctx.default_series, series, col_name)) {
                add_reason(reasons,
                    PlanCapabilityReasonCode::InvalidRefToken,
                    AxisKind::Time,
                    false,
                    "invalid column ref token '" + token + "'");
                return false;
            }

            if (!has_series_key(bundle, series)) {
                add_reason(reasons,
                    PlanCapabilityReasonCode::MissingRuntimeSeries,
                    AxisKind::Time,
                    false,
                    series);
                return false;
            }

            ColumnId col{};
            if (!bundle.schema->columns().find(col_name, col)) {
                add_reason(reasons,
                    PlanCapabilityReasonCode::MissingRuntimeColumn,
                    AxisKind::Time,
                    false,
                    col_name);
                return false;
            }

            const DataView dv(bundle);
            if (!dv.has_col(ColumnRef{ series, col })) {
                add_reason(reasons,
                    PlanCapabilityReasonCode::MissingRuntimeColumn,
                    AxisKind::Time,
                    false,
                    series + ":" + col_name);
                return false;
            }

            return true;
        }

        bool validate_runtime_token_param(const NodeSpec& node,
            const char* param_name,
            const ParamMap& plan_params,
            const RuntimeDiagnosisContext& runtime_ctx,
            std::vector<PlanCapabilityReason>& reasons) {
            const auto it = node.params.find(param_name);
            if (it == node.params.end()) {
                add_reason(reasons,
                    PlanCapabilityReasonCode::ParameterValidationFailed,
                    AxisKind::Time,
                    false,
                    "missing param '" + std::string(param_name) + "'");
                return false;
            }

            std::string token;
            std::string err;
            if (!resolve_param_to_string_like_runtime(it->second, plan_params, token, err)) {
                add_reason(reasons,
                    PlanCapabilityReasonCode::ParameterValidationFailed,
                    AxisKind::Time,
                    false,
                    "param '" + std::string(param_name) + "': " + err);
                return false;
            }
            token = trim_copy(std::move(token));
            if (token.empty()) {
                add_reason(reasons,
                    PlanCapabilityReasonCode::InvalidRefToken,
                    AxisKind::Time,
                    false,
                    "param '" + std::string(param_name) + "' resolved to empty token");
                return false;
            }

            return validate_runtime_token(token, runtime_ctx, reasons);
        }

        bool is_runtime_binding_form(const PlanNodeCapability& node, const NodeSpec& spec_node) {
            if (node.op_name == "core.ref") return true;
            if ((node.op_name == "ms.diff" || node.op_name == "ms.ratio" || node.op_name == "ms.zscore") &&
                spec_node.args.empty()) {
                return true;
            }
            return false;
        }

        void validate_runtime_binding_form(const NodeSpec& node,
            const ParamMap& plan_params,
            const RuntimeDiagnosisContext& runtime_ctx,
            std::vector<PlanCapabilityReason>& reasons) {
            const std::string canonical_op = normalize_plan_spec_runtime_op(node.op);
            if (canonical_op == "core.ref") {
                validate_runtime_token_param(node, "ref", plan_params, runtime_ctx, reasons);
                return;
            }
            if (canonical_op == "ms.diff" || canonical_op == "ms.ratio") {
                validate_runtime_token_param(node, "a", plan_params, runtime_ctx, reasons);
                validate_runtime_token_param(node, "b", plan_params, runtime_ctx, reasons);
                return;
            }
            if (canonical_op == "ms.zscore") {
                validate_runtime_token_param(node, "a", plan_params, runtime_ctx, reasons);
                return;
            }
        }

        void validate_standard_params(const NodeSpec& node,
            const OpEntry& op,
            const ParamMap& plan_params,
            std::vector<PlanCapabilityReason>& reasons) {
            ResolvedParamMap resolved{};
            std::string err;
            if (!resolve_and_validate_params(op, node.params, plan_params, nullptr, resolved, err)) {
                add_reason(reasons,
                    PlanCapabilityReasonCode::ParameterValidationFailed,
                    AxisKind::Time,
                    false,
                    err);
            }
        }

        void validate_node_args(const NodeSpec& node,
            const OpEntry& op,
            const std::unordered_map<std::string, std::size_t>& numeric_lookup,
            const std::unordered_map<std::string, std::size_t>& bool_lookup,
            std::vector<PlanCapabilityReason>& reasons) {
            if (node.args.size() != op.spec.inputs.size()) {
                std::ostringstream oss;
                oss << "expected " << op.spec.inputs.size() << " args, got " << node.args.size();
                add_reason(reasons,
                    PlanCapabilityReasonCode::WrongArgumentCount,
                    AxisKind::Time,
                    false,
                    oss.str());
                return;
            }

            for (std::size_t i = 0; i < node.args.size(); ++i) {
                const auto& arg = node.args[i];
                if (op.spec.inputs[i] == ValueType::Bool) {
                    if (bool_lookup.find(arg) == bool_lookup.end()) {
                        add_reason(reasons,
                            PlanCapabilityReasonCode::UnknownBoolNode,
                            AxisKind::Time,
                            false,
                            arg);
                    }
                }
                else {
                    if (numeric_lookup.find(arg) == numeric_lookup.end()) {
                        add_reason(reasons,
                            PlanCapabilityReasonCode::UnknownNumericNode,
                            AxisKind::Time,
                            false,
                            arg);
                    }
                }
            }
        }

        void validate_topology_requirements(const NodeSpec& node,
            const OpEntry& op,
            const ObservationWorldFacts& facts,
            const ParamMap& plan_params,
            const RuntimeDiagnosisContext& runtime_ctx,
            PlanNodeCapability& out,
            std::vector<std::string>& used_topology_names,
            std::vector<PlanCapabilityReason>& reasons) {
            std::string topology_name;
            std::string topology_err;
            const bool has_explicit_topology_binding =
                resolve_bound_topology_name(op, node.params, plan_params, nullptr, topology_name, topology_err);
            if (!has_explicit_topology_binding) {
                return;
            }

            if (topology_name.empty()) {
                topology_name = op.spec.default_topology_name;
            }

            if (!topology_name.empty()) {
                out.resolved_topology_name = topology_name;
                add_topology_name(used_topology_names, topology_name);
            }

            for (const auto family : op.spec.required_topology_families) {
                if (facts_have_topology_family(facts, family)) continue;

                std::string detail = std::string(topology_family_name(family));
                const std::string hint = topology_provision_hint(facts, { family }, topology_name);
                if (!hint.empty()) {
                    detail += " (" + hint + ")";
                }
                add_reason(reasons,
                    PlanCapabilityReasonCode::MissingRequiredTopologyFamily,
                    AxisKind::Time,
                    false,
                    detail);
            }

            if (topology_name.empty()) {
                return;
            }

            const TopologySpec* topology = nullptr;
            if (runtime_ctx.runtime && runtime_ctx.runtime->aligned_bundle) {
                const DataView dv(*runtime_ctx.runtime->aligned_bundle);
                topology = dv.topology(topology_name);
            }
            else {
                topology = find_fact_topology(facts, topology_name);
            }

            if (!topology) {
                std::string detail = "'" + topology_name + "'";
                const auto available = list_fact_topology_names(facts);
                if (!available.empty()) {
                    detail += " (available: " + join_strings(available) + ")";
                }
                add_reason(reasons,
                    PlanCapabilityReasonCode::MissingNamedTopology,
                    AxisKind::Time,
                    false,
                    detail);
                return;
            }

            if (!topology_family_matches(op.spec.required_topology_families, topology->family)) {
                std::string detail = "topology '" + topology->name + "' has family '" +
                    std::string(topology_family_name(topology->family)) +
                    "'; expected {" + join_topology_families(op.spec.required_topology_families) + "}";
                add_reason(reasons,
                    PlanCapabilityReasonCode::IncompatibleTopologyFamily,
                    AxisKind::Time,
                    false,
                    detail);
            }

            for (const auto kind : op.spec.required_topology_query_kinds) {
                if (topology->supports_query_kind(kind)) continue;
                std::string detail = "topology '" + topology->name +
                    "' is missing query kind '" +
                    std::string(topology_query_kind_name(kind)) +
                    "'";
                add_reason(reasons,
                    PlanCapabilityReasonCode::MissingRequiredTopologyQueryKind,
                    AxisKind::Time,
                    false,
                    detail);
            }

            if (runtime_ctx.runtime == nullptr ||
                !runtime_ctx.runtime->aligned_bundle ||
                runtime_ctx.default_series.empty()) {
                return;
            }

            const DataView dv(*runtime_ctx.runtime->aligned_bundle);
            RuntimeLocalitySubject runtime_subject{};
            if (!resolve_runtime_locality_subject(
                    op,
                    node.params,
                    plan_params,
                    nullptr,
                    dv,
                    runtime_ctx.default_series,
                    runtime_subject,
                    topology_err)) {
                add_reason(reasons,
                    PlanCapabilityReasonCode::LocalitySubjectResolutionFailed,
                    AxisKind::Time,
                    false,
                    topology_err);
                return;
            }
            out.runtime_locality_subject = runtime_subject;
            out.has_runtime_locality_subject = true;

            try {
                (void)dv.resolve_topology(runtime_subject, topology_name);
            }
            catch (const std::exception& ex) {
                add_reason(reasons,
                    PlanCapabilityReasonCode::TopologyResolutionFailed,
                    AxisKind::Time,
                    false,
                    ex.what());
            }
        }

        void analyze_node(const NodeSpec& node,
            PlanNodeKind kind,
            const OperatorRegistry& registry,
            const ObservationWorldFacts& facts,
            const ParamMap& plan_params,
            const RuntimeDiagnosisContext& runtime_ctx,
            const std::unordered_map<std::string, std::size_t>& numeric_lookup,
            const std::unordered_map<std::string, std::size_t>& bool_lookup,
            PlanNodeCapability& out,
            std::vector<OpAxisScope>& used_axis_scopes,
            std::vector<TopologyFamily>& used_topology_families,
            std::vector<std::string>& used_topology_names) {
            const auto* op = registry.get(out.op_name);
            if (!op) {
                add_reason(out.reasons, PlanCapabilityReasonCode::UnknownOperator);
                out.status = PlanCapabilityStatus::Invalid;
                return;
            }

            out.axis_scope = op->spec.axis_scope;
            out.required_axes = op->spec.required_axes;
            out.required_topology_families = op->spec.required_topology_families;
            out.required_topology_query_kinds = op->spec.required_topology_query_kinds;
            out.default_topology_name = op->spec.default_topology_name;
            out.topology_binding_param = op->spec.topology_binding_param;
            out.default_locality_subject_kind = op->spec.default_locality_subject_kind;
            out.default_locality_subject_value = op->spec.default_locality_subject_value;
            out.bound_locality_subject_kind = op->spec.bound_locality_subject_kind;
            out.locality_subject_binding_param = op->spec.locality_subject_binding_param;
            add_axis_scope(used_axis_scopes, out.axis_scope);
            for (const auto family : out.required_topology_families) {
                add_topology_family(used_topology_families, family);
            }

            if (op_mentions_topology_or_locality(*op)) {
                ResolvedLocalitySubject subject{};
                std::string subject_err;
                if (resolve_bound_locality_subject(*op, node.params, plan_params, nullptr, subject, subject_err)) {
                    out.locality_subject = std::move(subject);
                    out.has_locality_subject = true;
                }
            }

            if ((kind == PlanNodeKind::Numeric && op->spec.op_type != OpType::Numeric) ||
                (kind == PlanNodeKind::Bool && op->spec.op_type != OpType::Bool)) {
                add_reason(out.reasons,
                    PlanCapabilityReasonCode::OperatorTypeMismatch,
                    AxisKind::Time,
                    false,
                    std::string("op '") + out.op_name + "' is not valid for " +
                        std::string(kind == PlanNodeKind::Numeric ? "numeric" : "bool") + " nodes");
            }

            for (const auto axis : out.required_axes) {
                if (!facts_have_axis(facts, axis)) {
                    add_reason(out.reasons, PlanCapabilityReasonCode::MissingRequiredAxis, axis, true);
                }
            }

            if (out.axis_scope == OpAxisScope::CrossSeries && facts.series_count < 2) {
                add_reason(out.reasons, PlanCapabilityReasonCode::InsufficientSeriesCount);
            }

            validate_topology_requirements(
                node,
                *op,
                facts,
                plan_params,
                runtime_ctx,
                out,
                used_topology_names,
                out.reasons);

            const bool runtime_binding_form = is_runtime_binding_form(out, node) && node.args.empty();
            if (runtime_binding_form) {
                if (out.op_name != "core.ref") {
                    validate_standard_params(node, *op, plan_params, out.reasons);
                }
                validate_runtime_binding_form(node, plan_params, runtime_ctx, out.reasons);
            }
            else {
                validate_standard_params(node, *op, plan_params, out.reasons);
                validate_node_args(node, *op, numeric_lookup, bool_lookup, out.reasons);
            }

            out.status = out.reasons.empty() ?
                PlanCapabilityStatus::Valid :
                PlanCapabilityStatus::Invalid;
        }

        void collect_node_id_maps(const std::vector<PlanNodeCapability>& nodes,
            std::vector<std::size_t>& numeric_node_indexes,
            std::vector<std::size_t>& bool_node_indexes,
            std::unordered_map<std::string, std::vector<std::size_t>>& numeric_id_map,
            std::unordered_map<std::string, std::vector<std::size_t>>& bool_id_map) {
            numeric_node_indexes.clear();
            bool_node_indexes.clear();
            numeric_id_map.clear();
            bool_id_map.clear();

            for (std::size_t i = 0; i < nodes.size(); ++i) {
                const auto& node = nodes[i];
                if (node.node_kind == PlanNodeKind::Numeric) {
                    numeric_node_indexes.push_back(i);
                    if (!node.node_id.empty()) numeric_id_map[node.node_id].push_back(i);
                }
                else {
                    bool_node_indexes.push_back(i);
                    if (!node.node_id.empty()) bool_id_map[node.node_id].push_back(i);
                }
            }
        }

        void add_id_diagnostics(std::vector<PlanNodeCapability>& nodes,
            const std::vector<std::size_t>& indexes,
            const std::unordered_map<std::string, std::vector<std::size_t>>& id_map) {
            for (const auto idx : indexes) {
                if (nodes[idx].node_id.empty()) {
                    add_reason(nodes[idx].reasons, PlanCapabilityReasonCode::MissingNodeId);
                }
            }

            for (const auto& [id, members] : id_map) {
                if (members.size() < 2) continue;
                for (const auto idx : members) {
                    add_reason(nodes[idx].reasons, PlanCapabilityReasonCode::DuplicateNodeId, AxisKind::Time, false, id);
                }
            }
        }

        std::unordered_map<std::string, std::size_t> build_unique_lookup(
            const std::unordered_map<std::string, std::vector<std::size_t>>& id_map) {
            std::unordered_map<std::string, std::size_t> out;
            for (const auto& [id, members] : id_map) {
                if (members.size() == 1) {
                    out.emplace(id, members.front());
                }
            }
            return out;
        }

        void append_binding(std::vector<PlanBindingCapability>& bindings,
            std::size_t& incompatible_bindings,
            std::string binding_name,
            const std::string& target_id,
            const std::unordered_map<std::string, std::size_t>& bool_lookup) {
            if (target_id.empty()) return;

            PlanBindingCapability binding{};
            binding.binding_name = std::move(binding_name);
            binding.target_id = target_id;
            if (bool_lookup.find(target_id) == bool_lookup.end()) {
                binding.status = PlanCapabilityStatus::Invalid;
                add_reason(binding.reasons,
                    PlanCapabilityReasonCode::InvalidBindingTarget,
                    AxisKind::Time,
                    false,
                    target_id);
                ++incompatible_bindings;
            }
            bindings.push_back(std::move(binding));
        }

        PlanCapabilityReport diagnose_plan_capabilities_impl(
            const PlanSpec& spec,
            const OperatorRegistry& registry,
            const ObservationWorldFacts& facts,
            const RuntimeDiagnosisContext& runtime_ctx) {
            PlanCapabilityReport out{};
            out.facts = facts;
            out.nodes.reserve(spec.numeric_nodes.size() + spec.bool_nodes.size());

            for (const auto& node : spec.numeric_nodes) {
                PlanNodeCapability item{};
                item.node_id = node.id;
                item.original_op_name = node.op;
                item.op_name = normalize_plan_spec_runtime_op(node.op);
                item.node_kind = PlanNodeKind::Numeric;
                out.nodes.push_back(std::move(item));
            }
            for (const auto& node : spec.bool_nodes) {
                PlanNodeCapability item{};
                item.node_id = node.id;
                item.original_op_name = node.op;
                item.op_name = normalize_plan_spec_runtime_op(node.op);
                item.node_kind = PlanNodeKind::Bool;
                out.nodes.push_back(std::move(item));
            }

            std::vector<std::size_t> numeric_indexes;
            std::vector<std::size_t> bool_indexes;
            std::unordered_map<std::string, std::vector<std::size_t>> numeric_id_map;
            std::unordered_map<std::string, std::vector<std::size_t>> bool_id_map;
            collect_node_id_maps(out.nodes, numeric_indexes, bool_indexes, numeric_id_map, bool_id_map);
            add_id_diagnostics(out.nodes, numeric_indexes, numeric_id_map);
            add_id_diagnostics(out.nodes, bool_indexes, bool_id_map);

            const auto numeric_lookup = build_unique_lookup(numeric_id_map);
            const auto bool_lookup = build_unique_lookup(bool_id_map);

            for (std::size_t i = 0; i < spec.numeric_nodes.size(); ++i) {
                analyze_node(spec.numeric_nodes[i],
                    PlanNodeKind::Numeric,
                    registry,
                    facts,
                    spec.params,
                    runtime_ctx,
                    numeric_lookup,
                    bool_lookup,
                    out.nodes[i],
                    out.used_axis_scopes,
                    out.used_topology_families,
                    out.used_topology_names);
                if (out.nodes[i].status == PlanCapabilityStatus::Invalid) {
                    ++out.incompatible_nodes;
                }
            }

            const std::size_t bool_offset = spec.numeric_nodes.size();
            for (std::size_t i = 0; i < spec.bool_nodes.size(); ++i) {
                analyze_node(spec.bool_nodes[i],
                    PlanNodeKind::Bool,
                    registry,
                    facts,
                    spec.params,
                    runtime_ctx,
                    numeric_lookup,
                    bool_lookup,
                    out.nodes[bool_offset + i],
                    out.used_axis_scopes,
                    out.used_topology_families,
                    out.used_topology_names);
                if (out.nodes[bool_offset + i].status == PlanCapabilityStatus::Invalid) {
                    ++out.incompatible_nodes;
                }
            }

            append_binding(out.bindings, out.incompatible_bindings, "entry_long", spec.bind.entry_long, bool_lookup);
            append_binding(out.bindings, out.incompatible_bindings, "exit_long", spec.bind.exit_long, bool_lookup);
            append_binding(out.bindings, out.incompatible_bindings, "entry_short", spec.bind.entry_short, bool_lookup);
            append_binding(out.bindings, out.incompatible_bindings, "exit_short", spec.bind.exit_short, bool_lookup);
            append_binding(out.bindings, out.incompatible_bindings, "allow_trade", spec.bind.allow_trade, bool_lookup);

            out.total_nodes = out.nodes.size();
            out.status = (out.incompatible_nodes == 0 && out.incompatible_bindings == 0) ?
                PlanCapabilityStatus::Valid :
                PlanCapabilityStatus::Invalid;
            return out;
        }

    } // namespace

    std::string_view plan_capability_status_name(PlanCapabilityStatus status) {
        switch (status) {
        case PlanCapabilityStatus::Valid: return "valid";
        case PlanCapabilityStatus::Invalid: return "invalid";
        }
        return "unknown";
    }

    std::string_view plan_node_kind_name(PlanNodeKind kind) {
        switch (kind) {
        case PlanNodeKind::Numeric: return "numeric";
        case PlanNodeKind::Bool: return "bool";
        }
        return "unknown";
    }

    std::string_view plan_capability_reason_code_name(PlanCapabilityReasonCode code) {
        switch (code) {
        case PlanCapabilityReasonCode::UnknownOperator: return "unknown_operator";
        case PlanCapabilityReasonCode::OperatorTypeMismatch: return "operator_type_mismatch";
        case PlanCapabilityReasonCode::MissingRequiredAxis: return "missing_required_axis";
        case PlanCapabilityReasonCode::MissingRequiredTopologyFamily: return "missing_required_topology_family";
        case PlanCapabilityReasonCode::MissingNamedTopology: return "missing_named_topology";
        case PlanCapabilityReasonCode::MissingRequiredTopologyQueryKind: return "missing_required_topology_query_kind";
        case PlanCapabilityReasonCode::IncompatibleTopologyFamily: return "incompatible_topology_family";
        case PlanCapabilityReasonCode::TopologyResolutionFailed: return "topology_resolution_failed";
        case PlanCapabilityReasonCode::LocalitySubjectResolutionFailed: return "locality_subject_resolution_failed";
        case PlanCapabilityReasonCode::InsufficientSeriesCount: return "insufficient_series_count";
        case PlanCapabilityReasonCode::ParameterValidationFailed: return "parameter_validation_failed";
        case PlanCapabilityReasonCode::MissingNodeId: return "missing_node_id";
        case PlanCapabilityReasonCode::DuplicateNodeId: return "duplicate_node_id";
        case PlanCapabilityReasonCode::UnknownNumericNode: return "unknown_numeric_node";
        case PlanCapabilityReasonCode::UnknownBoolNode: return "unknown_bool_node";
        case PlanCapabilityReasonCode::WrongArgumentCount: return "wrong_argument_count";
        case PlanCapabilityReasonCode::InvalidRefToken: return "invalid_ref_token";
        case PlanCapabilityReasonCode::MissingRuntimeSeries: return "missing_runtime_series";
        case PlanCapabilityReasonCode::MissingRuntimeColumn: return "missing_runtime_column";
        case PlanCapabilityReasonCode::InvalidBindingTarget: return "invalid_binding_target";
        }
        return "unknown";
    }

    std::string_view plan_axis_scope_name(OpAxisScope scope) {
        switch (scope) {
        case OpAxisScope::RowWise: return "row_wise";
        case OpAxisScope::TimeSeries: return "temporal_local";
        case OpAxisScope::CrossSeries: return "cross_series";
        case OpAxisScope::SpatialLocal: return "spatial_local";
        case OpAxisScope::SpatioTemporal: return "spatiotemporal";
        }
        return "unknown";
    }

    std::string_view plan_axis_kind_name(AxisKind axis) {
        switch (axis) {
        case AxisKind::Time: return "time";
        case AxisKind::Entity: return "entity";
        case AxisKind::Spatial: return "spatial";
        }
        return "unknown";
    }

    std::string plan_capability_reason_text(const PlanCapabilityReason& reason) {
        switch (reason.code) {
        case PlanCapabilityReasonCode::UnknownOperator:
            return append_detail("unknown operator", reason.detail);
        case PlanCapabilityReasonCode::OperatorTypeMismatch:
            return append_detail("operator type mismatch", reason.detail);
        case PlanCapabilityReasonCode::MissingRequiredAxis:
            return std::string("missing required axis '") + std::string(plan_axis_kind_name(reason.axis)) + "'";
        case PlanCapabilityReasonCode::MissingRequiredTopologyFamily:
            return append_detail("missing required topology family", reason.detail);
        case PlanCapabilityReasonCode::MissingNamedTopology:
            return append_detail("missing named topology", reason.detail);
        case PlanCapabilityReasonCode::MissingRequiredTopologyQueryKind:
            return append_detail("missing required topology query kind", reason.detail);
        case PlanCapabilityReasonCode::IncompatibleTopologyFamily:
            return append_detail("incompatible topology family", reason.detail);
        case PlanCapabilityReasonCode::TopologyResolutionFailed:
            return append_detail("topology resolution failed", reason.detail);
        case PlanCapabilityReasonCode::LocalitySubjectResolutionFailed:
            return append_detail("locality subject resolution failed", reason.detail);
        case PlanCapabilityReasonCode::InsufficientSeriesCount:
            return "requires multiple aligned series";
        case PlanCapabilityReasonCode::ParameterValidationFailed:
            return append_detail("parameter validation failed", reason.detail);
        case PlanCapabilityReasonCode::MissingNodeId:
            return "missing node id";
        case PlanCapabilityReasonCode::DuplicateNodeId:
            return append_detail("duplicate node id", reason.detail);
        case PlanCapabilityReasonCode::UnknownNumericNode:
            return append_detail("unknown numeric node", reason.detail);
        case PlanCapabilityReasonCode::UnknownBoolNode:
            return append_detail("unknown bool node", reason.detail);
        case PlanCapabilityReasonCode::WrongArgumentCount:
            return append_detail("wrong argument count", reason.detail);
        case PlanCapabilityReasonCode::InvalidRefToken:
            return append_detail("invalid runtime ref token", reason.detail);
        case PlanCapabilityReasonCode::MissingRuntimeSeries:
            return append_detail("missing runtime series", reason.detail);
        case PlanCapabilityReasonCode::MissingRuntimeColumn:
            return append_detail("missing runtime column", reason.detail);
        case PlanCapabilityReasonCode::InvalidBindingTarget:
            return append_detail("invalid strategy binding target", reason.detail);
        }
        return "unknown reason";
    }

    PlanCapabilityReport diagnose_plan_capabilities(
        const PlanSpec& spec,
        const OperatorRegistry& registry,
        const ObservationWorldFacts& facts) {
        return diagnose_plan_capabilities_impl(spec, registry, facts, RuntimeDiagnosisContext{});
    }

    PlanCapabilityReport diagnose_plan_capabilities(
        const PlanSpec& spec,
        const ObservationWorldFacts& facts) {
        return diagnose_plan_capabilities(spec, make_default_registry(), facts);
    }

    PlanCapabilityReport diagnose_plan_capabilities(
        const PlanSpec& spec,
        const OperatorRegistry& registry,
        const ResolvedRuntimeInput& runtime) {
        RuntimeDiagnosisContext runtime_ctx{};
        runtime_ctx.runtime = &runtime;
        runtime_ctx.default_series = infer_default_series(runtime);
        return diagnose_plan_capabilities_impl(
            spec,
            registry,
            make_observation_world_facts(runtime),
            runtime_ctx);
    }

    PlanCapabilityReport diagnose_plan_capabilities(
        const PlanSpec& spec,
        const ResolvedRuntimeInput& runtime) {
        return diagnose_plan_capabilities(spec, make_default_registry(), runtime);
    }

} // namespace sugar
