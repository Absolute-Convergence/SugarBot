#include "sweep/dynamic_execution.h"

#include <algorithm>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace sugar {

    namespace {
        using IssueCode = PlanDynamicExecutionValidationCode;

        void add_issue(
            PlanDynamicExecutionValidationReport& report,
            IssueCode code,
            std::string subject,
            std::string detail) {
            report.issues.push_back(PlanDynamicExecutionValidationIssue{
                code,
                std::move(subject),
                std::move(detail),
            });
        }

        bool contains(
            const std::unordered_set<std::string>& values,
            const std::string& value) {
            return values.find(value) != values.end();
        }

        std::unordered_set<std::string> collect_node_ids(
            const std::vector<NodeSpec>& nodes) {
            std::unordered_set<std::string> ids;
            for (const auto& node : nodes) {
                if (!node.id.empty()) {
                    ids.insert(node.id);
                }
            }
            return ids;
        }

        bool node_exists(
            const std::unordered_set<std::string>& numeric_ids,
            const std::unordered_set<std::string>& bool_ids,
            const std::string& id) {
            return contains(numeric_ids, id) || contains(bool_ids, id);
        }

        void validate_unique_outputs(
            const PlanExecutionSpec& execution,
            PlanDynamicExecutionValidationReport& report,
            std::unordered_map<std::string, std::string>& output_nodes) {
            for (const auto& output : execution.outputs) {
                if (output.id.empty()) {
                    add_issue(report,
                        IssueCode::EmptyOutputId,
                        "execution.outputs",
                        "output ids must not be empty");
                    continue;
                }
                const auto [it, inserted] = output_nodes.emplace(output.id, output.node);
                if (!inserted) {
                    add_issue(report,
                        IssueCode::DuplicateOutputId,
                        output.id,
                        "execution output ids must be unique");
                }
            }
        }

        void validate_strategy_binding_outputs(
            const PlanBindings& bindings,
            const std::unordered_map<std::string, std::string>& output_nodes,
            PlanDynamicExecutionValidationReport& report) {
            const std::pair<const char*, const std::string*> roles[] = {
                { "entry_long", &bindings.entry_long },
                { "exit_long", &bindings.exit_long },
                { "entry_short", &bindings.entry_short },
                { "exit_short", &bindings.exit_short },
                { "allow_trade", &bindings.allow_trade },
            };

            for (const auto& role : roles) {
                if (!role.second || role.second->empty()) {
                    continue;
                }
                if (output_nodes.find(role.first) == output_nodes.end()) {
                    add_issue(report,
                        IssueCode::MissingStrategyBindingOutput,
                        role.first,
                        "declared_dynamic execution must expose an output for each active strategy binding");
                }
            }
        }

        void validate_subgraph_cycles(
            const std::vector<PlanExecutionSubgraphSpec>& subgraphs,
            const std::unordered_set<std::string>& known_subgraphs,
            PlanDynamicExecutionValidationReport& report) {
            std::unordered_map<std::string, std::vector<std::string>> dependencies;
            for (const auto& subgraph : subgraphs) {
                if (!subgraph.id.empty()) {
                    dependencies[subgraph.id] = subgraph.depends_on;
                }
            }

            std::unordered_map<std::string, int> state;
            std::function<void(const std::string&)> visit = [&](const std::string& id) {
                state[id] = 1;
                const auto deps = dependencies.find(id);
                if (deps != dependencies.end()) {
                    for (const auto& dep : deps->second) {
                        if (!contains(known_subgraphs, dep)) {
                            continue;
                        }
                        const int dep_state = state[dep];
                        if (dep_state == 1) {
                            add_issue(report,
                                IssueCode::CyclicSubgraphDependency,
                                id,
                                "subgraph dependency cycle reaches '" + dep + "'");
                            continue;
                        }
                        if (dep_state == 0) {
                            visit(dep);
                        }
                    }
                }
                state[id] = 2;
            };

            for (const auto& subgraph : subgraphs) {
                if (!subgraph.id.empty() && state[subgraph.id] == 0) {
                    visit(subgraph.id);
                }
            }
        }
    } // namespace

    std::string_view plan_dynamic_execution_validation_code_name(
        PlanDynamicExecutionValidationCode code) {
        switch (code) {
        case IssueCode::EmptySubgraphId:
            return "empty_subgraph_id";
        case IssueCode::DuplicateSubgraphId:
            return "duplicate_subgraph_id";
        case IssueCode::EmptyRouteId:
            return "empty_route_id";
        case IssueCode::DuplicateRouteId:
            return "duplicate_route_id";
        case IssueCode::EmptyRouteCaseId:
            return "empty_route_case_id";
        case IssueCode::DuplicateRouteCaseId:
            return "duplicate_route_case_id";
        case IssueCode::EmptyOutputId:
            return "empty_output_id";
        case IssueCode::DuplicateOutputId:
            return "duplicate_output_id";
        case IssueCode::UnknownNodeRef:
            return "unknown_node_ref";
        case IssueCode::UnknownBoolConditionRef:
            return "unknown_bool_condition_ref";
        case IssueCode::UnknownSubgraphRef:
            return "unknown_subgraph_ref";
        case IssueCode::AmbiguousNodeOwnership:
            return "ambiguous_node_ownership";
        case IssueCode::EmptySubgraph:
            return "empty_subgraph";
        case IssueCode::EmptyRouteCases:
            return "empty_route_cases";
        case IssueCode::EmptyRouteCaseActivation:
            return "empty_route_case_activation";
        case IssueCode::MissingRouteFallback:
            return "missing_route_fallback";
        case IssueCode::RouteCapExceeded:
            return "route_cap_exceeded";
        case IssueCode::ActiveSubgraphCapExceeded:
            return "active_subgraph_cap_exceeded";
        case IssueCode::SelectedOutputCapExceeded:
            return "selected_output_cap_exceeded";
        case IssueCode::MissingStrategyBindingOutput:
            return "missing_strategy_binding_output";
        case IssueCode::MissingOutputNodeOwnership:
            return "missing_output_node_ownership";
        case IssueCode::CyclicSubgraphDependency:
            return "cyclic_subgraph_dependency";
        }
        return "unknown";
    }

    PlanDynamicExecutionValidationReport validate_plan_dynamic_execution(
        const PlanSpec& spec) {
        PlanDynamicExecutionValidationReport report{};
        report.execution_section_present = spec.execution.has_value();
        if (!spec.execution ||
            spec.execution->mode == PlanExecutionMode::Static) {
            return report;
        }

        report.dynamic_enabled = true;
        const auto& execution = *spec.execution;
        const auto numeric_ids = collect_node_ids(spec.numeric_nodes);
        const auto bool_ids = collect_node_ids(spec.bool_nodes);

        std::unordered_set<std::string> subgraph_ids;
        std::unordered_map<std::string, std::string> node_owner;
        for (const auto& subgraph : execution.subgraphs) {
            if (subgraph.id.empty()) {
                add_issue(report,
                    IssueCode::EmptySubgraphId,
                    "execution.subgraphs",
                    "subgraph ids must not be empty");
                continue;
            }
            if (!subgraph_ids.insert(subgraph.id).second) {
                add_issue(report,
                    IssueCode::DuplicateSubgraphId,
                    subgraph.id,
                    "subgraph ids must be unique");
            }
            if (subgraph.nodes.empty()) {
                add_issue(report,
                    IssueCode::EmptySubgraph,
                    subgraph.id,
                    "subgraphs must declare at least one PlanSpec node");
            }
            for (const auto& node_id : subgraph.nodes) {
                if (!node_exists(numeric_ids, bool_ids, node_id)) {
                    add_issue(report,
                        IssueCode::UnknownNodeRef,
                        subgraph.id,
                        "subgraph references unknown node '" + node_id + "'");
                    continue;
                }
                const auto [owner, inserted] = node_owner.emplace(node_id, subgraph.id);
                if (!inserted && owner->second != subgraph.id) {
                    add_issue(report,
                        IssueCode::AmbiguousNodeOwnership,
                        node_id,
                        "node appears in both subgraph '" + owner->second +
                            "' and subgraph '" + subgraph.id + "'");
                }
            }
        }

        for (const auto& subgraph : execution.subgraphs) {
            for (const auto& dep : subgraph.depends_on) {
                if (!contains(subgraph_ids, dep)) {
                    add_issue(report,
                        IssueCode::UnknownSubgraphRef,
                        subgraph.id,
                        "subgraph dependency references unknown subgraph '" + dep + "'");
                }
            }
        }
        validate_subgraph_cycles(execution.subgraphs, subgraph_ids, report);

        if (execution.routes.size() > execution.bounds.max_routes) {
            add_issue(report,
                IssueCode::RouteCapExceeded,
                "execution.routes",
                "route count exceeds execution.bounds.max_routes");
        }

        std::unordered_set<std::string> route_ids;
        for (const auto& route : execution.routes) {
            if (route.id.empty()) {
                add_issue(report,
                    IssueCode::EmptyRouteId,
                    "execution.routes",
                    "route ids must not be empty");
            } else if (!route_ids.insert(route.id).second) {
                add_issue(report,
                    IssueCode::DuplicateRouteId,
                    route.id,
                    "route ids must be unique");
            }

            if (!contains(bool_ids, route.condition)) {
                add_issue(report,
                    IssueCode::UnknownBoolConditionRef,
                    route.id,
                    "route condition must reference a bool node; got '" +
                        route.condition + "'");
            }
            if (route.cases.empty()) {
                add_issue(report,
                    IssueCode::EmptyRouteCases,
                    route.id,
                    "routes must declare at least one case");
            }
            if (route.fallback_subgraphs.empty()) {
                add_issue(report,
                    IssueCode::MissingRouteFallback,
                    route.id,
                    "routes must declare fallback subgraphs for deterministic execution");
            }
            if (route.fallback_subgraphs.size() > execution.bounds.max_active_subgraphs) {
                add_issue(report,
                    IssueCode::ActiveSubgraphCapExceeded,
                    route.id,
                    "route fallback exceeds execution.bounds.max_active_subgraphs");
            }
            for (const auto& subgraph_id : route.fallback_subgraphs) {
                if (!contains(subgraph_ids, subgraph_id)) {
                    add_issue(report,
                        IssueCode::UnknownSubgraphRef,
                        route.id,
                        "route fallback references unknown subgraph '" +
                            subgraph_id + "'");
                }
            }

            std::unordered_set<std::string> case_ids;
            for (const auto& route_case : route.cases) {
                if (route_case.id.empty()) {
                    add_issue(report,
                        IssueCode::EmptyRouteCaseId,
                        route.id,
                        "route case ids must not be empty");
                } else if (!case_ids.insert(route_case.id).second) {
                    add_issue(report,
                        IssueCode::DuplicateRouteCaseId,
                        route_case.id,
                        "route case ids must be unique within a route");
                }
                if (route_case.activate_subgraphs.empty()) {
                    add_issue(report,
                        IssueCode::EmptyRouteCaseActivation,
                        route_case.id,
                        "route cases must activate at least one subgraph");
                }
                if (route_case.activate_subgraphs.size() >
                    execution.bounds.max_active_subgraphs) {
                    add_issue(report,
                        IssueCode::ActiveSubgraphCapExceeded,
                        route_case.id,
                        "route case activates more subgraphs than execution.bounds.max_active_subgraphs");
                }
                for (const auto& subgraph_id : route_case.activate_subgraphs) {
                    if (!contains(subgraph_ids, subgraph_id)) {
                        add_issue(report,
                            IssueCode::UnknownSubgraphRef,
                            route_case.id,
                            "route case references unknown subgraph '" +
                                subgraph_id + "'");
                    }
                }
            }
        }

        if (execution.outputs.size() > execution.bounds.max_selected_outputs) {
            add_issue(report,
                IssueCode::SelectedOutputCapExceeded,
                "execution.outputs",
                "output count exceeds execution.bounds.max_selected_outputs");
        }

        std::unordered_map<std::string, std::string> output_nodes;
        validate_unique_outputs(execution, report, output_nodes);
        for (const auto& output : output_nodes) {
            if (!node_exists(numeric_ids, bool_ids, output.second)) {
                add_issue(report,
                    IssueCode::UnknownNodeRef,
                    output.first,
                    "execution output references unknown node '" +
                        output.second + "'");
                continue;
            }
            if (node_owner.find(output.second) == node_owner.end()) {
                add_issue(report,
                    IssueCode::MissingOutputNodeOwnership,
                    output.first,
                    "execution output node '" + output.second +
                        "' is not owned by any declared subgraph");
            }
        }
        validate_strategy_binding_outputs(spec.bind, output_nodes, report);

        report.valid = report.issues.empty();
        return report;
    }

    bool plan_dynamic_execution_report_has_issue(
        const PlanDynamicExecutionValidationReport& report,
        PlanDynamicExecutionValidationCode code) {
        return std::any_of(report.issues.begin(), report.issues.end(),
            [&](const auto& issue) {
                return issue.code == code;
            });
    }

} // namespace sugar
