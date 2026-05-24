#include "sweep/plan_capability.h"

#include <string>
#include <string_view>

namespace sugar {

    namespace {

        std::string append_detail(std::string base, const std::string& detail) {
            if (!detail.empty()) {
                base += ": " + detail;
            }
            return base;
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
        case PlanCapabilityReasonCode::AmbiguousRuntimeDefaultSeries: return "ambiguous_runtime_default_series";
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
        case OpAxisScope::TopologyLocal: return "topology_local";
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
            return append_detail("requires multiple aligned series", reason.detail);
        case PlanCapabilityReasonCode::ParameterValidationFailed:
            return append_detail("parameter validation failed", reason.detail);
        case PlanCapabilityReasonCode::MissingNodeId:
            return "missing node id";
        case PlanCapabilityReasonCode::DuplicateNodeId:
            return append_detail("duplicate node id", reason.detail);
        case PlanCapabilityReasonCode::UnknownNumericNode:
            if (reason.detail.find(" is a bool node;") != std::string::npos) {
                return reason.detail;
            }
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
        case PlanCapabilityReasonCode::AmbiguousRuntimeDefaultSeries:
            return append_detail("ambiguous runtime default series", reason.detail);
        }
        return "unknown reason";
    }

} // namespace sugar
