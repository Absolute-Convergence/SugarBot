#include "op_modules.h"

#include <algorithm>
#include <cstdint>

namespace sugar::op_modules {

void register_spatial_ops(OperatorRegistry& r) {
    {
        OpEntry e;
        e.spec = OpSpec{
            "spatial.identity",
            OpType::Numeric,
            {ValueType::Num},
            ValueType::Num,
            {},
            "spatial",
            false,
            "child_warmup",
            OpAxisScope::SpatialLocal,
            {AxisKind::Spatial}
        };
        e.warmup_num = [](const std::vector<std::size_t>& cw, const ResolvedParamMap&) {
            return cw.at(0);
        };
        r.add(std::move(e));
    }

    {
        OpEntry e;
        OpParamSpec radius; radius.name = "radius"; radius.type = "float"; radius.required = true; radius.min_num = 0.0;
        OpParamSpec include_self; include_self.name = "include_self"; include_self.type = "bool"; include_self.required = false; include_self.default_value = true;
        OpParamSpec subject_series; subject_series.name = "subject_series"; subject_series.type = "string"; subject_series.required = false;
        OpParamSpec topology; topology.name = "topology"; topology.type = "string"; topology.required = false;
        e.spec = OpSpec{
            "spatial.radius_mean",
            OpType::Numeric,
            {ValueType::Num},
            ValueType::Num,
            {radius, include_self, subject_series, topology},
            "spatial",
            false,
            "child_warmup",
            OpAxisScope::SpatialLocal,
            {AxisKind::Spatial}
        };
        e.warmup_num = [](const std::vector<std::size_t>& cw, const ResolvedParamMap&) {
            return cw.at(0);
        };
        e.spec.required_topology_families = { TopologyFamily::SpatialMetric2D };
        e.spec.required_topology_query_kinds = { TopologyQueryKind::MetricRadius };
        e.spec.default_topology_name = std::string(builtin_spatial_topology_name());
        e.spec.topology_binding_param = "topology";
        e.spec.locality_subject_binding_param = "subject_series";
        r.add(std::move(e));
    }

    {
        OpEntry e;
        OpParamSpec last_n; last_n.name = "last_n"; last_n.type = "int"; last_n.required = true; last_n.min_num = 1;
        OpParamSpec radius; radius.name = "radius"; radius.type = "float"; radius.required = true; radius.min_num = 0.0;
        OpParamSpec subject_series; subject_series.name = "subject_series"; subject_series.type = "string"; subject_series.required = false;
        OpParamSpec topology; topology.name = "topology"; topology.type = "string"; topology.required = false;
        e.spec = OpSpec{
            "st.radius_mean",
            OpType::Numeric,
            {ValueType::Num},
            ValueType::Num,
            {last_n, radius, subject_series, topology},
            "st",
            false,
            "child_warmup + (last_n-1)",
            OpAxisScope::SpatioTemporal,
            {AxisKind::Time, AxisKind::Spatial}
        };
        e.warmup_num = [](const std::vector<std::size_t>& cw, const ResolvedParamMap& pm) {
            const auto last_n_val = static_cast<std::size_t>(std::max<std::int64_t>(1, as_i64(pm, "last_n").value_or(1)));
            return cw.at(0) + (last_n_val - 1);
        };
        e.spec.required_topology_families = { TopologyFamily::SpatioTemporalComposite };
        e.spec.required_topology_query_kinds = { TopologyQueryKind::Intersection };
        e.spec.default_topology_name = std::string(builtin_spatiotemporal_topology_name());
        e.spec.topology_binding_param = "topology";
        e.spec.locality_subject_binding_param = "subject_series";
        r.add(std::move(e));
    }
}

} // namespace sugar::op_modules
