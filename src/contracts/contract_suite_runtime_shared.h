#pragma once

#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "contract_suite_internal.h"
#include "engine/program_evaluator.h"
#include "ingest/run_manifest.h"
#include "sweep/plan_capability.h"

namespace sugar::contract_suite_internal {

    std::shared_ptr<IndexSchema> make_spatial_only_index_schema();
    std::shared_ptr<IndexSchema> make_time_spatial_index_schema();

    bool make_single_series_dataview_with_index_schema(
        std::span<const double> close,
        const std::shared_ptr<IndexSchema>& index_schema,
        std::shared_ptr<align::AlignedBundle>& bundle_out,
        ColumnRef& primary_out,
        TimeKey start_time = static_cast<TimeKey>(0),
        std::span<const double> spatial_x = {},
        std::span<const double> spatial_y = {});

    bool make_two_series_dataview_with_index_schema(
        std::span<const double> primary_close,
        std::span<const double> secondary_close,
        const std::shared_ptr<IndexSchema>& index_schema,
        std::shared_ptr<align::AlignedBundle>& bundle_out,
        ColumnRef& primary_out,
        TimeKey start_time,
        std::span<const double> primary_x,
        std::span<const double> primary_y,
        std::span<const double> secondary_x,
        std::span<const double> secondary_y);

    bool compile_and_eval_plan_signals(
        const PlanSpec& spec,
        const DataView& dv,
        const ColumnRef& primary,
        ProgramSignalOutputs& out,
        std::string& err);

    const PlanNodeCapability* find_plan_node(
        const PlanCapabilityReport& report,
        std::string_view node_id);

    bool has_plan_reason(
        const PlanNodeCapability* node,
        PlanCapabilityReasonCode code,
        std::optional<AxisKind> axis = std::nullopt);

    bool has_plan_reason_detail(
        const PlanNodeCapability* node,
        PlanCapabilityReasonCode code,
        std::string_view detail_substring);

    const PlanBindingCapability* find_plan_binding(
        const PlanCapabilityReport& report,
        std::string_view binding_name);

    bool has_binding_reason(
        const PlanBindingCapability* binding,
        PlanCapabilityReasonCode code);

    bool any_plan_node_has_reason(
        const PlanCapabilityReport& report,
        PlanCapabilityReasonCode code);

    const TopologyUsageSnapshot* find_topology_usage(
        const RunManifest& manifest,
        std::string_view subject_id,
        std::string_view topology_name);

    template <class Fn>
    bool expect_topology_resolution_failure(
        ContractAsserts& t,
        const char* action_label,
        std::string_view expected_substring,
        Fn&& fn) {
        try {
            (void)fn();
            return t.expect(false, std::string(action_label) + " rejects explicitly");
        }
        catch (const std::exception& ex) {
            const std::string msg = ex.what();
            const bool contains = msg.find(expected_substring) != std::string::npos;
            return t.expect(contains, std::string(action_label) + " preserves explicit failure reason");
        }
    }

    PlanSpec make_spatial_radius_mean_plan(
        std::string name,
        double radius,
        double threshold,
        std::string topology_name,
        std::optional<std::string> subject_series = std::nullopt);

    PlanSpec make_spatiotemporal_radius_mean_plan(
        std::string name,
        std::int64_t last_n,
        double radius,
        double threshold,
        std::string topology_name);

    void add_standard_exit_binding(PlanSpec& spec);
    PlanSpec make_multi_entity_runner_contract_plan();

    const CapabilityOutcomeSnapshot* find_capability_outcome(
        const RunManifest& manifest,
        std::string_view domain,
        std::string_view subject);

} // namespace sugar::contract_suite_internal
