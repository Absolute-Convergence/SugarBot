#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "sweep/plan_spec.h"

namespace sugar {

    enum class PlanDynamicExecutionValidationCode : std::uint8_t {
        EmptySubgraphId = 0,
        DuplicateSubgraphId = 1,
        EmptyRouteId = 2,
        DuplicateRouteId = 3,
        EmptyRouteCaseId = 4,
        DuplicateRouteCaseId = 5,
        EmptyOutputId = 6,
        DuplicateOutputId = 7,
        UnknownNodeRef = 8,
        UnknownBoolConditionRef = 9,
        UnknownSubgraphRef = 10,
        AmbiguousNodeOwnership = 11,
        EmptySubgraph = 12,
        EmptyRouteCases = 13,
        EmptyRouteCaseActivation = 14,
        MissingRouteFallback = 15,
        RouteCapExceeded = 16,
        ActiveSubgraphCapExceeded = 17,
        SelectedOutputCapExceeded = 18,
        MissingStrategyBindingOutput = 19,
        MissingOutputNodeOwnership = 20,
        CyclicSubgraphDependency = 21,
    };

    struct PlanDynamicExecutionValidationIssue {
        PlanDynamicExecutionValidationCode code{
            PlanDynamicExecutionValidationCode::UnknownNodeRef
        };
        std::string subject{};
        std::string detail{};
    };

    struct PlanDynamicExecutionValidationReport {
        bool execution_section_present{ false };
        bool dynamic_enabled{ false };
        bool valid{ true };
        std::vector<PlanDynamicExecutionValidationIssue> issues{};
    };

    std::string_view plan_dynamic_execution_validation_code_name(
        PlanDynamicExecutionValidationCode code);

    PlanDynamicExecutionValidationReport validate_plan_dynamic_execution(
        const PlanSpec& spec);

    bool plan_dynamic_execution_report_has_issue(
        const PlanDynamicExecutionValidationReport& report,
        PlanDynamicExecutionValidationCode code);

} // namespace sugar
