#include "op_modules.h"

#include <string>
#include <utility>
#include <vector>

namespace sugar::op_modules {

namespace {

    OpParamSpec make_topology_param() {
        OpParamSpec param{};
        param.name = "topology";
        param.type = "string";
        param.required = true;
        return param;
    }

    OpParamSpec make_direction_param() {
        OpParamSpec param{};
        param.name = "direction";
        param.type = "string";
        param.required = false;
        param.default_value = std::string("outgoing");
        return param;
    }

    OpParamSpec make_include_anchor_param() {
        OpParamSpec param{};
        param.name = "include_anchor";
        param.type = "bool";
        param.required = false;
        param.default_value = false;
        return param;
    }

    OpParamSpec make_max_neighbors_param() {
        OpParamSpec param{};
        param.name = "max_neighbors";
        param.type = "int";
        param.required = false;
        param.min_num = 0.0;
        return param;
    }

    OpParamSpec make_graph_version_param() {
        OpParamSpec param{};
        param.name = "version";
        param.type = "string";
        param.required = false;
        return param;
    }

    OpParamSpec make_hierarchy_traversal_param() {
        OpParamSpec param{};
        param.name = "traversal";
        param.type = "string";
        param.required = false;
        return param;
    }

    OpParamSpec make_hierarchy_max_depth_param() {
        OpParamSpec param{};
        param.name = "max_depth";
        param.type = "int";
        param.required = false;
        param.min_num = 0.0;
        return param;
    }

    OpParamSpec make_subject_series_param() {
        OpParamSpec param{};
        param.name = "subject_series";
        param.type = "string";
        param.required = false;
        return param;
    }

    std::vector<OpParamSpec> make_common_params() {
        return {
            make_topology_param(),
            make_direction_param(),
            make_include_anchor_param(),
            make_max_neighbors_param(),
            make_graph_version_param(),
            make_hierarchy_traversal_param(),
            make_hierarchy_max_depth_param(),
            make_subject_series_param(),
        };
    }

    void configure_common_topology_binding(OpEntry& e, TopologyQueryKind query_kind) {
        e.spec.required_topology_query_kinds = { query_kind };
        e.spec.topology_binding_param = "topology";
        e.spec.default_locality_subject_kind = LocalitySubjectKind::ObservationDomain;
        e.spec.locality_subject_binding_param = "subject_series";
    }

} // namespace

void register_topology_ops(OperatorRegistry& r) {
    const auto register_neighbor_aggregate = [&](std::string name) {
        OpEntry e;
        e.spec = OpSpec{
            std::move(name),
            OpType::Numeric,
            { ValueType::Num },
            ValueType::Num,
            make_common_params(),
            "topology",
            false,
            "child_warmup",
            OpAxisScope::TopologyLocal,
            {}
        };
        e.warmup_num = [](const std::vector<std::size_t>& cw, const ResolvedParamMap&) {
            return cw.at(0);
        };
        configure_common_topology_binding(e, TopologyQueryKind::NeighborSet);
        r.add(std::move(e));
    };

    register_neighbor_aggregate("topology.neighbor_mean");
    register_neighbor_aggregate("topology.neighbor_min");
    register_neighbor_aggregate("topology.neighbor_max");

    {
        OpEntry e;
        e.spec = OpSpec{
            "topology.weighted_sum",
            OpType::Numeric,
            { ValueType::Num },
            ValueType::Num,
            make_common_params(),
            "topology",
            false,
            "child_warmup",
            OpAxisScope::TopologyLocal,
            {}
        };
        e.warmup_num = [](const std::vector<std::size_t>& cw, const ResolvedParamMap&) {
            return cw.at(0);
        };
        configure_common_topology_binding(e, TopologyQueryKind::WeightedNeighborSet);
        r.add(std::move(e));
    }

    {
        OpEntry e;
        e.spec = OpSpec{
            "topology.degree",
            OpType::Numeric,
            {},
            ValueType::Num,
            make_common_params(),
            "topology",
            false,
            "0",
            OpAxisScope::TopologyLocal,
            {}
        };
        e.warmup_num = [](const std::vector<std::size_t>&, const ResolvedParamMap&) {
            return std::size_t{ 0 };
        };
        configure_common_topology_binding(e, TopologyQueryKind::NeighborSet);
        r.add(std::move(e));
    }
}

} // namespace sugar::op_modules
