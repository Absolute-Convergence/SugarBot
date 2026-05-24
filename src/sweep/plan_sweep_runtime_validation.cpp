#include "plan_sweep_runtime_validation.h"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <string>
#include <vector>

#include "op_registry_internal.h"
#include "ops/op_registry.h"
#include "ops/plan_spec_runtime_alias.h"

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

        bool topology_has_time_valid_edges(const TopologySpec& spec) {
            return std::any_of(spec.static_edges.begin(),
                spec.static_edges.end(),
                [](const StaticGraphEdge& edge) {
                    return edge.valid_from.has_value() || edge.valid_to.has_value();
                });
        }

        std::string join_topology_families(
            const std::vector<TopologyFamily>& families) {
            std::string out;
            for (std::size_t i = 0; i < families.size(); ++i) {
                if (i) out += ", ";
                out += std::string(topology_family_name(families[i]));
            }
            return out;
        }

        std::string join_topology_query_kinds(
            const std::vector<TopologyQueryKind>& queries) {
            std::string out;
            for (std::size_t i = 0; i < queries.size(); ++i) {
                if (i) out += ", ";
                out += std::string(topology_query_kind_name(queries[i]));
            }
            return out;
        }
    } // namespace

    void normalize_plan_sweep_runtime_aliases(PlanSpec& spec) {
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
            if (topology_has_time_valid_edges(*topology) &&
                (!idx || !idx->has_kind(AxisKind::Time))) {
                err = std::string("node '") + node.id + "' (" + kind + ", op '" + node.op +
                    "'): topology '" + topology_name +
                    "' has time-valid edges; add layout time_col/time_kind so topology queries can bind graph time";
                return false;
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

} // namespace sugar
