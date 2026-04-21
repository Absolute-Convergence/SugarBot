#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "ingest/observation_capability.h"
#include "ops/op_registry.h"

namespace sugar {

    enum class PlanCapabilityStatus {
        Valid = 0,
        Invalid = 1,
    };

    enum class PlanNodeKind {
        Numeric = 0,
        Bool = 1,
    };

    enum class PlanCapabilityReasonCode {
        UnknownOperator = 0,
        OperatorTypeMismatch = 1,
        MissingRequiredAxis = 2,
        MissingRequiredTopologyFamily = 3,
        MissingNamedTopology = 4,
        MissingRequiredTopologyQueryKind = 5,
        IncompatibleTopologyFamily = 6,
        TopologyResolutionFailed = 7,
        LocalitySubjectResolutionFailed = 8,
        InsufficientSeriesCount = 9,
        ParameterValidationFailed = 10,
        MissingNodeId = 11,
        DuplicateNodeId = 12,
        UnknownNumericNode = 13,
        UnknownBoolNode = 14,
        WrongArgumentCount = 15,
        InvalidRefToken = 16,
        MissingRuntimeSeries = 17,
        MissingRuntimeColumn = 18,
        InvalidBindingTarget = 19,
        AmbiguousRuntimeDefaultSeries = 20,
    };

    struct PlanCapabilityReason {
        PlanCapabilityReasonCode code{ PlanCapabilityReasonCode::UnknownOperator };
        AxisKind axis{ AxisKind::Time };
        bool has_axis{ false };
        std::string detail{};
    };

    struct PlanNodeCapability {
        std::string node_id{};
        std::string original_op_name{};
        std::string op_name{};
        PlanNodeKind node_kind{ PlanNodeKind::Numeric };
        OpAxisScope axis_scope{ OpAxisScope::RowWise };
        std::vector<AxisKind> required_axes{};
        std::vector<TopologyFamily> required_topology_families{};
        std::vector<TopologyQueryKind> required_topology_query_kinds{};
        std::string default_topology_name{};
        std::string topology_binding_param{};
        std::string resolved_topology_name{};
        LocalitySubjectKind default_locality_subject_kind{ LocalitySubjectKind::PrimarySeries };
        std::string default_locality_subject_value{};
        LocalitySubjectKind bound_locality_subject_kind{ LocalitySubjectKind::SeriesKey };
        std::string locality_subject_binding_param{};
        ResolvedLocalitySubject locality_subject{};
        bool has_locality_subject{ false };
        RuntimeLocalitySubject runtime_locality_subject{};
        bool has_runtime_locality_subject{ false };
        PlanCapabilityStatus status{ PlanCapabilityStatus::Valid };
        std::vector<PlanCapabilityReason> reasons{};
    };

    struct PlanBindingCapability {
        std::string binding_name{};
        std::string target_id{};
        PlanCapabilityStatus status{ PlanCapabilityStatus::Valid };
        std::vector<PlanCapabilityReason> reasons{};
    };

    struct PlanCapabilityReport {
        ObservationWorldFacts facts{};
        PlanCapabilityStatus status{ PlanCapabilityStatus::Valid };
        std::size_t total_nodes{ 0 };
        std::size_t incompatible_nodes{ 0 };
        std::size_t incompatible_bindings{ 0 };
        std::vector<OpAxisScope> used_axis_scopes{};
        std::vector<TopologyFamily> used_topology_families{};
        std::vector<std::string> used_topology_names{};
        std::vector<PlanNodeCapability> nodes{};
        std::vector<PlanBindingCapability> bindings{};
    };

    std::string_view plan_capability_status_name(PlanCapabilityStatus status);
    std::string_view plan_node_kind_name(PlanNodeKind kind);
    std::string_view plan_capability_reason_code_name(PlanCapabilityReasonCode code);
    std::string_view plan_axis_scope_name(OpAxisScope scope);
    std::string_view plan_axis_kind_name(AxisKind axis);
    std::string plan_capability_reason_text(const PlanCapabilityReason& reason);

    PlanCapabilityReport diagnose_plan_capabilities(
        const PlanSpec& spec,
        const OperatorRegistry& registry,
        const ObservationWorldFacts& facts);

    PlanCapabilityReport diagnose_plan_capabilities(
        const PlanSpec& spec,
        const ObservationWorldFacts& facts);

    PlanCapabilityReport diagnose_plan_capabilities(
        const PlanSpec& spec,
        const OperatorRegistry& registry,
        const ResolvedRuntimeInput& runtime);

    PlanCapabilityReport diagnose_plan_capabilities(
        const PlanSpec& spec,
        const ResolvedRuntimeInput& runtime);

} // namespace sugar
