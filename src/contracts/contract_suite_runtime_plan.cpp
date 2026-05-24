#include "contract_suite_internal.h"
#include "contract_suite_runtime_shared.h"
#include "strategy_plan_internal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "op_registry_internal.h"
#include "ops/op_registry.h"
#include "sweep/panel_runner.h"
#include "sweep/plan_capability.h"
#include "engine/plan_compiler.h"
#include "sweep/plan_sweep.h"
#include "engine/program_evaluator.h"
#include "ingest/run_manifest.h"

namespace sugar::contract_suite_internal {


    bool test_plan_capability_fact_contracts(ContractAsserts& t) {
        t.out << "[plan_capability_facts] fact-only plan/operator capability diagnosis\n";

        {
            PlanSpec spec{};
            spec.numeric_nodes = {
                NodeSpec{ "c", "core.close", {}, {} },
                NodeSpec{ "sma", "ts.sma", { "c" }, { {"n", static_cast<std::int64_t>(3)} } },
            };

            ObservationWorldFacts facts{};
            facts.series_count = 1;
            facts.row_count = 3;
            facts.has_time_axis = false;
            facts.has_spatial_axis = true;
            facts.has_entity_axis = false;

            const auto report = diagnose_plan_capabilities(spec, facts);
            const auto* close_node = find_plan_node(report, "c");
            const auto* sma_node = find_plan_node(report, "sma");

            t.expect(report.status == PlanCapabilityStatus::Invalid,
                "spatial-only facts make temporal-local plan invalid");
            t.expect(report.incompatible_nodes == 1,
                "spatial-only facts count one incompatible temporal-local node");
            t.expect(close_node && close_node->status == PlanCapabilityStatus::Valid,
                "row-wise core.close remains valid on spatial-only facts");
            t.expect(sma_node && sma_node->status == PlanCapabilityStatus::Invalid,
                "ts.sma is invalid on spatial-only facts");
            t.expect(has_plan_reason(sma_node, PlanCapabilityReasonCode::MissingRequiredAxis, AxisKind::Time),
                "ts.sma failure cites missing time axis");
        }

        {
            PlanSpec spec{};
            spec.numeric_nodes = {
                NodeSpec{ "c", "core.close", {}, {} },
                NodeSpec{ "mean", "spatial.radius_mean", { "c" }, {
                    {"radius", 1.0}
                } },
            };

            ObservationWorldFacts facts{};
            facts.series_count = 1;
            facts.row_count = 4;
            facts.has_time_axis = true;
            facts.has_spatial_axis = false;
            facts.has_entity_axis = false;

            const auto report = diagnose_plan_capabilities(spec, facts);
            const auto* mean_node = find_plan_node(report, "mean");

            t.expect(report.status == PlanCapabilityStatus::Invalid,
                "temporal-only facts make spatial locality plan invalid");
            t.expect(mean_node && mean_node->status == PlanCapabilityStatus::Invalid,
                "spatial locality node is invalid when spatial topology is absent");
            t.expect(mean_node && mean_node->resolved_topology_name == builtin_spatial_topology_name(),
                "spatial locality node resolves built-in default topology name in capability report");
            t.expect(std::find(report.used_topology_names.begin(),
                    report.used_topology_names.end(),
                    std::string(builtin_spatial_topology_name())) != report.used_topology_names.end(),
                "plan capability report records used topology names");
            t.expect(has_plan_reason(mean_node, PlanCapabilityReasonCode::MissingRequiredAxis, AxisKind::Spatial),
                "spatial locality failure still cites missing spatial axis");
            t.expect(has_plan_reason(mean_node, PlanCapabilityReasonCode::MissingRequiredTopologyFamily),
                "spatial locality failure cites missing required topology family");
            t.expect(has_plan_reason_detail(mean_node,
                    PlanCapabilityReasonCode::MissingRequiredTopologyFamily,
                    "provide spatial neighborhood/index schema"),
                "spatial locality topology-family failure carries providable spatial hint");
            t.expect(has_plan_reason(mean_node, PlanCapabilityReasonCode::MissingNamedTopology),
                "spatial locality failure cites missing named topology when built-in spatial topology is unavailable");

            RunManifest manifest{};
            append_plan_capability_snapshot(report, manifest);
            const auto* usage = find_topology_usage(manifest, "mean", builtin_spatial_topology_name());
            t.expect(usage != nullptr,
                "plan capability snapshot records topology usage for unresolved built-in locality");
            if (usage) {
                t.expect(!usage->catalog_present,
                    "plan capability snapshot preserves missing-catalog topology usage state");
                t.expect(!usage->runtime_resolved,
                    "plan capability snapshot preserves unresolved topology state");
            }
            const auto* outcome = find_capability_outcome(manifest, "plan_node", "mean");
            t.expect(outcome != nullptr,
                "plan capability snapshot records plan-node outcome");
            if (outcome) {
                t.expect(outcome->status == "invalid",
                    "plan capability snapshot preserves plan-node invalid status");
            }
        }

        {
            PlanSpec parsed{};
            std::string err;
            const bool ok = parse_plan_spec_json(
                "{\n"
                "  \"version\": 1,\n"
                "  \"numeric_nodes\": [],\n"
                "  \"bool_nodes\": [],\n"
                "  \"bindings\": { \"entry\": \"entry\" }\n"
                "}\n",
                parsed,
                err);
            t.expect(!ok &&
                    err.find("bindings.entry is not supported") != std::string::npos &&
                    err.find("entry_long") != std::string::npos,
                "PlanSpec parser rejects unknown strategy binding keys with valid-key guidance");
        }

        {
            PlanSpec spec{};
            spec.numeric_nodes = {
                NodeSpec{ "left", "core.close", {}, {} },
                NodeSpec{ "right", "core.close", {}, {} },
                NodeSpec{ "spread", "diff", { "left", "right" }, {} },
            };
            spec.bool_nodes = {
                NodeSpec{ "above", "cmp.gte", { "left", "right" }, {} },
                NodeSpec{ "entry", "bool.and", { "above", "above" }, {} },
            };
            spec.bind.entry_long = "entry";

            ObservationWorldFacts facts{};
            facts.series_count = 1;
            facts.row_count = 10;
            facts.has_time_axis = true;
            facts.has_spatial_axis = false;
            facts.has_entity_axis = false;

            const auto report = diagnose_plan_capabilities(spec, facts);
            const auto* spread_node = find_plan_node(report, "spread");
            const auto* above_node = find_plan_node(report, "above");
            const auto* entry_node = find_plan_node(report, "entry");

            t.expect(report.status == PlanCapabilityStatus::Invalid,
                "single-series facts make cross-series plan invalid");
            t.expect(spread_node && spread_node->op_name == "ms.diff",
                "plan capability diagnosis normalizes runtime op aliases");
            t.expect(above_node && above_node->op_name == "cmp.ge" &&
                    entry_node && entry_node->op_name == "logic.and",
                "plan capability diagnosis normalizes comparison and boolean aliases");
            t.expect(spread_node && spread_node->status == PlanCapabilityStatus::Invalid,
                "cross-series node is invalid on single-series facts");
            t.expect(has_plan_reason(spread_node, PlanCapabilityReasonCode::InsufficientSeriesCount),
                "cross-series node failure cites multiple-series requirement");
        }

        {
            PlanSpec spec{};
            spec.numeric_nodes = {
                NodeSpec{ "", "core.close", {}, {} },
                NodeSpec{ "dup", "core.close", {}, {} },
                NodeSpec{ "dup", "core.close", {}, {} },
            };

            ObservationWorldFacts facts{};
            facts.series_count = 1;
            facts.row_count = 3;
            facts.has_time_axis = true;

            const auto report = diagnose_plan_capabilities(spec, facts);
            const auto* dup = find_plan_node(report, "dup");

            t.expect(report.status == PlanCapabilityStatus::Invalid,
                "missing or duplicate node ids make plan invalid");
            t.expect(any_plan_node_has_reason(report, PlanCapabilityReasonCode::MissingNodeId),
                "missing-id node is reported explicitly");
            t.expect(dup && has_plan_reason(dup, PlanCapabilityReasonCode::DuplicateNodeId),
                "duplicate node id is reported explicitly");
        }

        {
            PlanSpec spec{};
            spec.numeric_nodes = {
                NodeSpec{ "c", "core.close", {}, {} },
                NodeSpec{ "mean", "st.radius_mean", { "c" }, {
                    {"last_n", static_cast<std::int64_t>(3)},
                    {"radius", 1.5}
                } },
            };

            ObservationWorldFacts facts{};
            facts.series_count = 2;
            facts.row_count = 5;
            facts.has_time_axis = true;
            facts.has_spatial_axis = true;
            facts.has_entity_axis = true;

            const auto report = diagnose_plan_capabilities(spec, facts);
            const auto* mean_node = find_plan_node(report, "mean");

            t.expect(report.status == PlanCapabilityStatus::Valid,
                "spatiotemporal facts make st.radius_mean plan valid");
            t.expect(report.incompatible_nodes == 0,
                "spatiotemporal facts produce no incompatible nodes");
            t.expect(std::find(report.used_axis_scopes.begin(),
                    report.used_axis_scopes.end(),
                    OpAxisScope::SpatioTemporal) != report.used_axis_scopes.end(),
                "spatiotemporal scope is recorded in used_axis_scopes");
            t.expect(std::find(report.used_topology_families.begin(),
                    report.used_topology_families.end(),
                    TopologyFamily::SpatioTemporalComposite) != report.used_topology_families.end(),
                "spatiotemporal topology family is recorded in used_topology_families");
            t.expect(mean_node && mean_node->status == PlanCapabilityStatus::Valid,
                "st.radius_mean node is valid on spatiotemporal facts");
            t.expect(mean_node && mean_node->required_topology_families ==
                std::vector<TopologyFamily>{ TopologyFamily::SpatioTemporalComposite },
                "st.radius_mean node preserves required topology family metadata");
            t.expect(mean_node && mean_node->required_topology_query_kinds ==
                std::vector<TopologyQueryKind>{ TopologyQueryKind::Intersection },
                "st.radius_mean node preserves required topology query metadata");
            t.expect(mean_node && mean_node->default_topology_name == builtin_spatiotemporal_topology_name(),
                "st.radius_mean node preserves default topology name");
            t.expect(mean_node && mean_node->topology_binding_param == "topology",
                "st.radius_mean node preserves topology binding param");
        }

        return true;
    }

    bool test_plan_capability_runtime_binding_contracts(ContractAsserts& t) {
        t.out << "[plan_capability_runtime_binding] runtime-aware plan binding diagnosis\n";

        const std::array<double, 3> close{ 10.0, 11.0, 12.0 };
        std::shared_ptr<align::AlignedBundle> bundle{};
        ColumnRef primary{};
        const bool runtime_ok = make_single_series_dataview_from_close(
            close,
            bundle,
            primary,
            static_cast<TimeKey>(20240101));
        t.expect(runtime_ok, "runtime-aware plan capability fixture builds single-series DataView");
        if (!runtime_ok) return true;

        ResolvedRuntimeInput runtime{};
        runtime.aligned_bundle = bundle;
        runtime.primary_ref = primary;
        runtime.primary_series_index = 0;
        runtime.run_manifest.preprocess.has_time_axis = true;
        runtime.run_manifest.preprocess.has_spatial_axis = false;
        runtime.run_manifest.preprocess.has_entity_axis = false;

        PlanSpec spec{};
        spec.numeric_nodes = {
            NodeSpec{ "other_close", "core.ref", {}, { {"ref", std::string("other:close")} } },
            NodeSpec{ "zero", "core.const", {}, { {"value", 0.0} } },
        };
        spec.bool_nodes = {
            NodeSpec{ "entry", "cmp.gt", { "other_close", "zero" }, {} },
        };
        spec.bind.entry_long = "entry";
        spec.bind.exit_long = "missing_exit";

        const auto report = diagnose_plan_capabilities(spec, runtime);
        const auto* other_close = find_plan_node(report, "other_close");
        const auto* exit_binding = find_plan_binding(report, "exit_long");

        t.expect(report.status == PlanCapabilityStatus::Invalid,
            "runtime-aware report is invalid for missing series binding");
        t.expect(report.incompatible_nodes == 1,
            "runtime-aware report counts invalid bound ref node");
        t.expect(report.incompatible_bindings == 1,
            "runtime-aware report counts invalid strategy binding target");
        t.expect(other_close && has_plan_reason(other_close, PlanCapabilityReasonCode::MissingRuntimeSeries),
            "core.ref failure cites missing runtime series");
        t.expect(exit_binding && has_binding_reason(exit_binding, PlanCapabilityReasonCode::InvalidBindingTarget),
            "binding failure cites invalid strategy binding target");

        return true;
    }

    bool test_plan_capability_runtime_topology_contracts(ContractAsserts& t) {
        t.out << "[plan_capability_runtime_topology] runtime-aware plan topology diagnosis\n";

        OperatorRegistry registry{};
        {
            OpEntry close{};
            close.spec.name = "core.close";
            close.spec.op_type = OpType::Numeric;
            close.spec.output = ValueType::Num;
            registry.add(std::move(close));
        }
        {
            OpEntry semantic{};
            semantic.spec.name = "semantic.weighted_mean";
            semantic.spec.op_type = OpType::Numeric;
            semantic.spec.inputs = { ValueType::Num };
            semantic.spec.output = ValueType::Num;
            semantic.spec.axis_scope = OpAxisScope::RowWise;
            semantic.spec.required_topology_families = { TopologyFamily::SemanticGraph };
            semantic.spec.required_topology_query_kinds = { TopologyQueryKind::WeightedNeighborSet };
            semantic.spec.default_topology_name = "entity.semantic.graph";
            semantic.spec.topology_binding_param = "topology";
            registry.add(std::move(semantic));
        }

        const std::array<double, 4> close{ 10.0, 11.0, 12.0, 13.0 };
        std::shared_ptr<align::AlignedBundle> bundle{};
        ColumnRef primary{};
        const bool runtime_ok = make_single_series_dataview_from_close(
            close,
            bundle,
            primary,
            static_cast<TimeKey>(20240101));
        t.expect(runtime_ok, "semantic-topology runtime fixture builds single-series DataView");
        t.expect(bundle && bundle->topology_catalog != nullptr,
            "semantic-topology runtime fixture preserves topology catalog sidecar");
        if (!runtime_ok || !bundle || !bundle->topology_catalog) return true;

        TopologySpec semantic_spec{};
        semantic_spec.name = "entity.semantic.graph";
        semantic_spec.family = TopologyFamily::SemanticGraph;
        semantic_spec.domain_scope = TopologyDomainScope::ObservationDomain;
        semantic_spec.query_kinds = { TopologyQueryKind::WeightedNeighborSet };
        semantic_spec.artifact_ref = TopologyArtifactRef{
            "entity.semantic.graph.v1",
            "artifacts/entity.semantic.graph.parquet",
            "parquet",
            "sha256:entity-semantic-v1",
            "python_pandas"
        };
        bundle->topology_catalog->add(semantic_spec);

        ResolvedRuntimeInput runtime{};
        runtime.aligned_bundle = bundle;
        runtime.topology_catalog = bundle->topology_catalog;
        runtime.primary_ref = primary;
        runtime.primary_series_index = 0;
        runtime.run_manifest.preprocess.has_time_axis = true;
        runtime.run_manifest.preprocess.has_spatial_axis = false;
        runtime.run_manifest.preprocess.has_entity_axis = false;
        runtime.run_manifest.topologies = bundle->topology_catalog->snapshot();

        PlanSpec spec{};
        spec.numeric_nodes = {
            NodeSpec{ "c", "core.close", {}, {} },
            NodeSpec{ "semantic_mean", "semantic.weighted_mean", { "c" }, {} },
        };

        const auto report = diagnose_plan_capabilities(spec, registry, runtime);
        const auto* semantic_node = find_plan_node(report, "semantic_mean");

        t.expect(report.status == PlanCapabilityStatus::Invalid,
            "runtime-aware plan capability rejects declared semantic topology without runtime adapters");
        t.expect(semantic_node && semantic_node->resolved_topology_name == "entity.semantic.graph",
            "runtime-aware semantic topology report preserves resolved named topology");
        t.expect(has_plan_reason(semantic_node, PlanCapabilityReasonCode::TopologyResolutionFailed),
            "runtime-aware semantic topology report cites runtime topology-resolution failure");
        t.expect(has_plan_reason_detail(semantic_node,
                PlanCapabilityReasonCode::TopologyResolutionFailed,
                "metadata-only"),
            "runtime-aware semantic topology failure preserves metadata-only bearing detail");

        append_plan_capability_snapshot(report, runtime.run_manifest);
        const auto* semantic_usage = find_topology_usage(runtime.run_manifest, "semantic_mean", "entity.semantic.graph");
        t.expect(semantic_usage != nullptr,
            "runtime-aware plan capability snapshot records semantic topology usage");
        if (semantic_usage) {
            t.expect(semantic_usage->catalog_present,
                "runtime-aware plan capability snapshot preserves semantic topology catalog presence");
            t.expect(!semantic_usage->runtime_resolved,
                "runtime-aware plan capability snapshot preserves failed semantic topology resolution");
            t.expect(semantic_usage->artifact_ref.stable_id == "entity.semantic.graph.v1",
                "runtime-aware plan capability snapshot preserves semantic topology artifact stable id");
        }

        return true;
    }

    bool test_plan_capability_runtime_locality_contracts(ContractAsserts& t) {
        t.out << "[plan_capability_runtime_locality] runtime-aware locality-subject diagnosis\n";

        const std::vector<double> primary_close = { 1.0, 100.0, 1.0 };
        const std::vector<double> secondary_close = { 0.0, 0.0, 0.0 };
        const std::vector<double> primary_x = { 0.0, 0.1, 10.0 };
        const std::vector<double> primary_y = { 0.0, 0.0, 0.0 };
        const std::vector<double> secondary_x = { 0.0, 10.0, 10.1 };
        const std::vector<double> secondary_y = { 0.0, 0.0, 0.0 };

        std::shared_ptr<align::AlignedBundle> bundle;
        ColumnRef primary{};
        const bool runtime_ok = make_two_series_dataview_with_index_schema(
            primary_close,
            secondary_close,
            make_time_spatial_index_schema(),
            bundle,
            primary,
            static_cast<TimeKey>(20240101),
            primary_x,
            primary_y,
            secondary_x,
            secondary_y);
        t.expect(runtime_ok, "runtime-aware locality-subject fixture builds two-series spatio-temporal DataView");
        if (!runtime_ok || !bundle) return true;

        ResolvedRuntimeInput runtime{};
        runtime.aligned_bundle = bundle;
        runtime.topology_catalog = bundle->topology_catalog;
        runtime.primary_ref = primary;
        runtime.primary_series_index = bundle->find_series_index(primary.series);
        runtime.series_key_to_entity = {
            { "A", "entity.A" },
            { "B", "entity.B" }
        };
        runtime.run_manifest.preprocess.has_time_axis = true;
        runtime.run_manifest.preprocess.has_spatial_axis = true;
        runtime.run_manifest.preprocess.has_entity_axis = true;
        runtime.run_manifest.topologies = bundle->topology_catalog->snapshot();

        PlanSpec spec{};
        spec.numeric_nodes = {
            NodeSpec{ "c", "core.close", {}, {} },
            NodeSpec{ "mean", "spatial.radius_mean", { "c" }, {
                { "radius", 0.2 },
                { "subject_series", std::string("missing.series") }
            } },
        };

        const auto registry = make_default_registry();
        const auto report = diagnose_plan_capabilities(spec, registry, runtime);
        const auto* mean_node = find_plan_node(report, "mean");

        t.expect(report.status == PlanCapabilityStatus::Invalid,
            "runtime-aware plan capability rejects unresolved locality subject bindings");
        t.expect(mean_node && has_plan_reason(mean_node, PlanCapabilityReasonCode::LocalitySubjectResolutionFailed),
            "runtime-aware locality-subject failure is reported explicitly");
        t.expect(has_plan_reason_detail(mean_node,
                PlanCapabilityReasonCode::LocalitySubjectResolutionFailed,
                "series not found"),
            "runtime-aware locality-subject failure preserves missing-series detail");

        append_plan_capability_snapshot(report, runtime.run_manifest);
        const auto* invalid_usage = find_topology_usage(
            runtime.run_manifest,
            "mean",
            builtin_spatial_topology_name());
        t.expect(invalid_usage != nullptr,
            "runtime-aware locality-subject snapshot records topology usage");
        if (invalid_usage) {
            t.expect(invalid_usage->locality_subject_kind == "series_key",
                "runtime-aware locality-subject snapshot preserves bound subject kind");
            t.expect(invalid_usage->locality_subject_value == "missing.series",
                "runtime-aware locality-subject snapshot preserves unresolved subject value");
            t.expect(invalid_usage->locality_subject_explicit,
                "runtime-aware locality-subject snapshot preserves explicit binding flag");
            t.expect(invalid_usage->resolved_series_key.empty(),
                "runtime-aware locality-subject snapshot keeps resolved series empty on failure");
            t.expect(invalid_usage->resolved_entity_key.empty(),
                "runtime-aware locality-subject snapshot keeps resolved entity empty on failure");
        }

        PlanSpec resolved_spec{};
        resolved_spec.numeric_nodes = {
            NodeSpec{ "c", "core.close", {}, {} },
            NodeSpec{ "mean", "spatial.radius_mean", { "c" }, {
                { "radius", 0.2 },
                { "subject_series", std::string("B") }
            } },
        };

        const auto resolved_report = diagnose_plan_capabilities(resolved_spec, registry, runtime);
        const auto* resolved_node = find_plan_node(resolved_report, "mean");
        t.expect(resolved_report.status == PlanCapabilityStatus::Valid,
            "runtime-aware plan capability accepts explicit resolved locality subject bindings");
        t.expect(resolved_node && resolved_node->has_runtime_locality_subject,
            "runtime-aware locality-subject report preserves resolved runtime subject");
        if (resolved_node && resolved_node->has_runtime_locality_subject) {
            t.expect(resolved_node->runtime_locality_subject.series_key == "B",
                "runtime-aware locality-subject report preserves resolved series key");
            t.expect(resolved_node->runtime_locality_subject.entity_key == "entity.B",
                "runtime-aware locality-subject report preserves resolved entity key");
        }

        append_plan_capability_snapshot(resolved_report, runtime.run_manifest);
        const auto* resolved_usage = find_topology_usage(
            runtime.run_manifest,
            "mean",
            builtin_spatial_topology_name());
        t.expect(resolved_usage != nullptr,
            "runtime-aware resolved locality-subject snapshot records topology usage");
        if (resolved_usage) {
            t.expect(resolved_usage->locality_subject_kind == "series_key",
                "runtime-aware resolved locality-subject snapshot preserves subject kind");
            t.expect(resolved_usage->locality_subject_value == "B",
                "runtime-aware resolved locality-subject snapshot preserves subject value");
            t.expect(resolved_usage->locality_subject_explicit,
                "runtime-aware resolved locality-subject snapshot preserves explicit binding");
            t.expect(resolved_usage->resolved_series_key == "B",
                "runtime-aware resolved locality-subject snapshot preserves resolved series key");
            t.expect(resolved_usage->resolved_entity_key == "entity.B",
                "runtime-aware resolved locality-subject snapshot preserves resolved entity key");
            t.expect(resolved_usage->runtime_resolved,
                "runtime-aware resolved locality-subject snapshot preserves successful runtime resolution");
        }

        return true;
    }

    bool test_multi_entity_ref_binding_overload_rejection_contracts(ContractAsserts& t) {
        t.out << "[multi_entity_ref_binding_overload_rejection] multi-entity runtime binding overload rejection\n";

        const std::vector<double> primary_close = { 1.0, 2.0, 3.0 };
        const std::vector<double> secondary_close = { 4.0, 5.0, 6.0 };
        const std::vector<double> primary_x = { 0.0, 0.1, 0.2 };
        const std::vector<double> primary_y = { 0.0, 0.0, 0.0 };
        const std::vector<double> secondary_x = { 10.0, 10.1, 10.2 };
        const std::vector<double> secondary_y = { 0.0, 0.0, 0.0 };

        std::shared_ptr<align::AlignedBundle> bundle{};
        ColumnRef primary{};
        const bool runtime_ok = make_two_series_dataview_with_index_schema(
            primary_close,
            secondary_close,
            make_time_spatial_index_schema(),
            bundle,
            primary,
            static_cast<TimeKey>(20240101),
            primary_x,
            primary_y,
            secondary_x,
            secondary_y);
        t.expect(runtime_ok, "multi-entity runtime-binding fixture builds two-series spatiotemporal DataView");
        if (!runtime_ok || !bundle) return true;

        ResolvedRuntimeInput runtime{};
        runtime.aligned_bundle = bundle;
        runtime.topology_catalog = bundle->topology_catalog;
        runtime.series_key_to_entity = {
            { "A", "entity.A" },
            { "B", "entity.B" }
        };
        runtime.run_manifest.preprocess.has_time_axis = true;
        runtime.run_manifest.preprocess.has_spatial_axis = true;
        runtime.run_manifest.preprocess.has_entity_axis = true;
        runtime.run_manifest.topologies = bundle->topology_catalog->snapshot();

        PlanSpec ref_spec{};
        ref_spec.numeric_nodes = {
            NodeSpec{ "close_leaf", "core.ref", {}, { {"ref", std::string("close")} } },
            NodeSpec{ "zero", "core.const", {}, { {"value", 0.0} } },
        };
        ref_spec.bool_nodes = {
            NodeSpec{ "entry", "cmp.gt", { "close_leaf", "zero" }, {} },
        };
        ref_spec.bind.entry_long = "entry";

        const auto ambiguous_ref_report = diagnose_plan_capabilities(ref_spec, runtime);
        const auto* close_leaf = find_plan_node(ambiguous_ref_report, "close_leaf");
        t.expect(ambiguous_ref_report.status == PlanCapabilityStatus::Invalid,
            "runtime-aware plan capability rejects unqualified refs on multi-series runtime without explicit primary");
        t.expect(close_leaf && has_plan_reason(close_leaf, PlanCapabilityReasonCode::AmbiguousRuntimeDefaultSeries),
            "unqualified multi-series ref failure cites ambiguous runtime default series");
        t.expect(has_plan_reason_detail(close_leaf,
                PlanCapabilityReasonCode::AmbiguousRuntimeDefaultSeries,
                "explicit primary/default series"),
            "unqualified multi-series ref failure preserves explicit target requirement detail");

        const auto registry = make_default_registry();
        const auto ambiguous_locality_spec = make_spatial_radius_mean_plan(
            "ambiguous_locality",
            0.2,
            0.0,
            std::string(builtin_spatial_topology_name()));
        const auto ambiguous_locality_report =
            diagnose_plan_capabilities(ambiguous_locality_spec, registry, runtime);
        const auto* mean_node = find_plan_node(ambiguous_locality_report, "mean");
        t.expect(ambiguous_locality_report.status == PlanCapabilityStatus::Invalid,
            "runtime-aware plan capability rejects implicit primary locality binding on multi-series runtime without explicit target");
        t.expect(mean_node && has_plan_reason(mean_node, PlanCapabilityReasonCode::LocalitySubjectResolutionFailed),
            "implicit multi-series locality binding failure cites locality-subject resolution");
        t.expect(has_plan_reason_detail(mean_node,
                PlanCapabilityReasonCode::LocalitySubjectResolutionFailed,
                "explicit compatibility primary series"),
            "implicit multi-series locality binding failure preserves explicit target requirement detail");

        runtime.primary_ref = primary;
        runtime.primary_series_index = bundle->find_series_index(primary.series);

        const auto resolved_ref_report = diagnose_plan_capabilities(ref_spec, runtime);
        const auto* resolved_close_leaf = find_plan_node(resolved_ref_report, "close_leaf");
        t.expect(resolved_ref_report.status == PlanCapabilityStatus::Valid,
            "runtime-aware plan capability accepts unqualified refs once explicit primary series is present");
        t.expect(resolved_close_leaf && resolved_close_leaf->status == PlanCapabilityStatus::Valid,
            "runtime-aware unqualified ref node becomes valid once explicit primary series is present");

        const auto resolved_locality_report =
            diagnose_plan_capabilities(ambiguous_locality_spec, registry, runtime);
        const auto* resolved_mean_node = find_plan_node(resolved_locality_report, "mean");
        t.expect(resolved_locality_report.status == PlanCapabilityStatus::Valid,
            "runtime-aware plan capability accepts implicit primary locality binding once explicit primary series is present");
        t.expect(resolved_mean_node && resolved_mean_node->has_runtime_locality_subject,
            "runtime-aware locality report preserves resolved primary locality subject once target is explicit");
        if (resolved_mean_node && resolved_mean_node->has_runtime_locality_subject) {
            t.expect(resolved_mean_node->runtime_locality_subject.series_key == "A",
                "runtime-aware locality resolution binds implicit primary subject to explicit primary series");
            t.expect(resolved_mean_node->runtime_locality_subject.entity_key == "entity.A",
                "runtime-aware locality resolution preserves explicit primary entity provenance");
        }

        return true;
    }

    bool test_multi_entity_runner_design_load_contracts(ContractAsserts& t) {
        t.out << "[multi_entity_runner_design_loads] neutral multi-entity runner design loads\n";

        const std::vector<double> primary_close = { 99.0, 110.0, 90.0, 120.0, 80.0, 130.0 };
        const std::vector<double> secondary_close = { 99.0, 101.0, 102.0, 103.0, 104.0, 105.0 };
        const std::vector<double> primary_x = { 0.0, 0.1, 0.2, 0.3, 0.4, 0.5 };
        const std::vector<double> primary_y = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
        const std::vector<double> secondary_x = { 10.0, 10.1, 10.2, 10.3, 10.4, 10.5 };
        const std::vector<double> secondary_y = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };

        std::shared_ptr<align::AlignedBundle> bundle{};
        ColumnRef primary{};
        const bool runtime_ok = make_two_series_dataview_with_index_schema(
            primary_close,
            secondary_close,
            make_time_spatial_index_schema(),
            bundle,
            primary,
            static_cast<TimeKey>(20240101),
            primary_x,
            primary_y,
            secondary_x,
            secondary_y);
        t.expect(runtime_ok, "multi-entity runner fixture builds two-series runtime");
        if (!runtime_ok || !bundle) return true;

        const DataView dv(*bundle);
        const PlanSpec spec = make_multi_entity_runner_contract_plan();
        PlanSweepRuntimeOptions opt{};
        RunManifest run_manifest{};
        run_manifest.preprocess.series_keys = { "A", "B" };
        run_manifest.preprocess.has_time_axis = true;
        run_manifest.preprocess.has_entity_axis = true;
        run_manifest.preprocess.has_spatial_axis = true;
        run_manifest.preprocess.time_kind = TimeKind::YyyyMmDd;
        run_manifest.preprocess.time_resolution = "daily";
        run_manifest.preprocess.alignment_applied = true;
        run_manifest.preprocess.alignment_calendar = align::CalendarPolicy::Intersection;
        run_manifest.preprocess.alignment_missing = align::MissingPolicy::NaN;
        run_manifest.preprocess.alignment_non_temporal_multi_series =
            align::NonTemporalMultiSeriesPolicy::Reject;
        run_manifest.preprocess.input_warnings.push_back("panel contract warning");
        run_manifest.primary_series_key = "A";
        run_manifest.primary_selection_mode = RuntimePrimarySelectionMode::ExplicitSeries;
        TopologySpec topology{};
        topology.name = builtin_spatial_topology_name();
        run_manifest.topologies.push_back(std::move(topology));
        run_manifest.topology_usages.push_back(TopologyUsageSnapshot{});
        const RunProvenanceSummary run_provenance =
            build_run_provenance_summary(run_manifest);

        {
            PanelRankingSpec ranking{};
            ranking.objective.primary.source = ObjectiveValueSource::ReportedMetric;
            ranking.objective.primary.name = "pnl";
            ranking.objective.direction = ObjectiveDirection::Maximize;

            const auto result = run_ranked_multi_entity_custom_planspec_over_dataview(
                dv,
                spec,
                opt,
                primary.col,
                2,
                ranking);

            t.expect(result.ranking.objective.primary.name == "pnl" &&
                    result.ranking.objective.penalties.empty(),
                "multi-entity runner preserves canonical pnl objective");
            t.expect(result.topk.size() == 2, "multi-entity runner keeps requested top-k when both entities succeed");
            t.expect(result.failures.empty(), "multi-entity runner design-load fixture has no failures");
            const PanelReport report = build_panel_report(result, &run_provenance);
            t.expect(report.resolved_objective.primary.name == "pnl",
                "panel report resolves pnl ranking objective");
            t.expect(report.top_rows.size() == result.topk.size(),
                "panel report preserves ranked row count");
            t.expect(report.run_provenance.primary_series_key == "A" &&
                    report.run_provenance.series_count == 2 &&
                    report.run_provenance.topology_usage_count == 1,
                "panel report preserves shared run provenance");
            std::ostringstream report_text;
            print_panel_report_summary(report, report_text);
            t.expect(report_text.str().find("Run provenance:") != std::string::npos,
                "panel report summary prints shared run provenance");
            if (result.topk.size() == 2) {
                t.expect(result.topk[0].entity_key == "entity.A" || result.topk[1].entity_key == "entity.A",
                    "multi-entity runner preserves entity identity distinct from series keys");
                const bool pnl_score_ok =
                    nearly_equal(result.topk[0].ranking_score, result.topk[0].metrics.pnl) &&
                    nearly_equal(result.topk[1].ranking_score, result.topk[1].metrics.pnl);
                t.expect(pnl_score_ok, "multi-entity runner scores pnl ranking directly from pnl");
                t.expect(report.top_rows[0].run.objective.rankability == RunRankabilityState::Rankable &&
                        report.top_rows[1].run.objective.rankability == RunRankabilityState::Rankable,
                    "panel report classifies pnl-ranked rows as rankable");
            }
        }

        {
            PanelRankingSpec ranking{};
            ranking.objective.primary.source = ObjectiveValueSource::ReportedMetric;
            ranking.objective.primary.name = "pnl";
            ranking.objective.direction = ObjectiveDirection::Maximize;
            ObjectivePenaltySpec penalty{};
            penalty.value.source = ObjectiveValueSource::ReportedMetric;
            penalty.value.name = "max_drawdown";
            penalty.weight = 0.5;
            ranking.objective.penalties.push_back(std::move(penalty));

            const auto result = run_ranked_multi_entity_custom_planspec_over_dataview(
                dv,
                spec,
                opt,
                primary.col,
                2,
                ranking);

            t.expect(result.ranking.objective.penalties.size() == 1 &&
                    nearly_equal(result.ranking.objective.penalties.front().weight, 0.5),
                "multi-entity runner preserves canonical pnl_dd-style objective");
            const PanelReport report = build_panel_report(result, &run_provenance);
            t.expect(report.resolved_objective.penalties.size() == 1,
                "panel report preserves pnl_dd penalty structure");
            if (result.topk.size() == 2) {
                const bool pnl_dd_score_ok =
                    nearly_equal(
                        result.topk[0].ranking_score,
                        result.topk[0].metrics.pnl - 0.5 * result.topk[0].metrics.max_drawdown) &&
                    nearly_equal(
                        result.topk[1].ranking_score,
                        result.topk[1].metrics.pnl - 0.5 * result.topk[1].metrics.max_drawdown);
                t.expect(pnl_dd_score_ok, "multi-entity runner scores pnl_dd ranking from explicit drawdown-weighted formula");
                t.expect(nearly_equal(report.top_rows[0].run.objective.score, result.topk[0].ranking_score) &&
                        nearly_equal(report.top_rows[1].run.objective.score, result.topk[1].ranking_score),
                    "panel report preserves per-row resolved objective scores");
            }
        }

        {
            PlanRunSpec run{};
            run.scope = PlanRunScope::PerEntityRanked;

            PanelRankingSpec ranking{};
            std::string ranking_err;
            t.expect(build_panel_ranking_spec(run, ranking, ranking_err),
                "phase 6 shared panel ranking adapter defaults to a canonical pnl objective");
            t.expect(ranking.objective.primary.name == "pnl" &&
                    ranking.objective.penalties.empty(),
                "phase 6 shared panel ranking adapter maps the default path to a zero-penalty pnl objective");

            PlanRunSpec structured_run{};
            structured_run.scope = PlanRunScope::PerEntityRanked;
            structured_run.objective_spec = RankingObjectiveSpec{};
            structured_run.objective_spec->primary.source = ObjectiveValueSource::ReportedMetric;
            structured_run.objective_spec->primary.name = "pnl";
            structured_run.objective_spec->direction = ObjectiveDirection::Maximize;
            ObjectiveTieBreakSpec tie_break{};
            tie_break.value.source = ObjectiveValueSource::ReportedMetric;
            tie_break.value.name = "trades";
            tie_break.direction = ObjectiveDirection::Minimize;
            structured_run.objective_spec->tie_breaks.push_back(tie_break);

            t.expect(build_panel_ranking_spec(structured_run, ranking, ranking_err),
                "phase 6 shared panel ranking adapter accepts structured run.objective");
            t.expect(ranking.objective.tie_breaks.size() == 1 &&
                    ranking.objective.primary.name == "pnl" &&
                    ranking.objective.tie_breaks.front().value.name == "trades",
                "phase 6 shared panel ranking adapter builds canonical ranking objective state");
        }

        return true;
    }

    bool test_multi_entity_runner_overload_rejection_contracts(ContractAsserts& t) {
        t.out << "[multi_entity_runner_overload_rejection] neutral multi-entity runner overload rejection\n";

        const std::vector<double> primary_close = { 99.0, 110.0, 90.0, 120.0 };
        const std::vector<double> secondary_close = { 99.0, 101.0, 102.0, 103.0 };
        const std::vector<double> primary_x = { 0.0, 0.1, 0.2, 0.3 };
        const std::vector<double> primary_y = { 0.0, 0.0, 0.0, 0.0 };
        const std::vector<double> secondary_x = { 10.0, 10.1, 10.2, 10.3 };
        const std::vector<double> secondary_y = { 0.0, 0.0, 0.0, 0.0 };

        std::shared_ptr<align::AlignedBundle> bundle{};
        ColumnRef primary{};
        const bool runtime_ok = make_two_series_dataview_with_index_schema(
            primary_close,
            secondary_close,
            make_time_spatial_index_schema(),
            bundle,
            primary,
            static_cast<TimeKey>(20240101),
            primary_x,
            primary_y,
            secondary_x,
            secondary_y);
        t.expect(runtime_ok, "multi-entity runner overload fixture builds two-series runtime");
        if (!runtime_ok || !bundle) return true;

        const DataView dv(*bundle);
        const PlanSpec bad_spec = make_spatial_radius_mean_plan(
            "multi_entity_runner_bad_subject",
            0.2,
            0.0,
            std::string(builtin_spatial_topology_name()),
            "missing.series");
        PanelRankingSpec ranking{};
        ranking.objective.primary.source = ObjectiveValueSource::ReportedMetric;
        ranking.objective.primary.name = "pnl";
        ranking.objective.direction = ObjectiveDirection::Maximize;
        ObjectivePenaltySpec penalty{};
        penalty.value.source = ObjectiveValueSource::ReportedMetric;
        penalty.value.name = "max_drawdown";
        penalty.weight = 0.25;
        ranking.objective.penalties.push_back(std::move(penalty));

        const auto invalid_col_result = run_ranked_multi_entity_custom_planspec_over_dataview(
            dv,
            bad_spec,
            PlanSweepRuntimeOptions{},
            ColumnId{ 9999u },
            2,
            ranking);
        t.expect(invalid_col_result.topk.empty(),
            "multi-entity runner keeps top-k empty when primary column is missing for every series");
        t.expect(invalid_col_result.failures.size() == 2,
            "multi-entity runner records a failure per series when primary column is missing");
        const PanelReport invalid_col_report = build_panel_report(invalid_col_result);
        t.expect(invalid_col_report.failures.size() == invalid_col_result.failures.size(),
            "panel report preserves missing-column failure count");
        if (invalid_col_result.failures.size() == 2) {
            t.expect(invalid_col_result.failures[0].series_key == "A" ||
                    invalid_col_result.failures[1].series_key == "A",
                "multi-entity runner failure records preserve failing series keys");
            t.expect(invalid_col_result.failures[0].entity_key == "entity.A" ||
                    invalid_col_result.failures[1].entity_key == "entity.A",
                "multi-entity runner failure records preserve failing entity keys");
            t.expect(invalid_col_report.failures[0].kind == PanelFailureKind::MissingPrimaryColumn &&
                    invalid_col_report.failures[1].kind == PanelFailureKind::MissingPrimaryColumn,
                "panel report classifies missing-primary-column failures explicitly");
        }

        const auto bad_subject_result = run_ranked_multi_entity_custom_planspec_over_dataview(
            dv,
            bad_spec,
            PlanSweepRuntimeOptions{},
            primary.col,
            2,
            ranking);
        t.expect(bad_subject_result.topk.empty(),
            "multi-entity runner keeps top-k empty when custom execution fails for every series");
        t.expect(bad_subject_result.failures.size() == 2,
            "multi-entity runner records execution failure per series");
        const PanelReport bad_subject_report = build_panel_report(bad_subject_result);
        if (bad_subject_result.failures.size() == 2) {
            const bool reason_ok =
                bad_subject_result.failures[0].reason.find("missing.series") != std::string::npos &&
                bad_subject_result.failures[1].reason.find("missing.series") != std::string::npos;
            t.expect(reason_ok,
                "multi-entity runner preserves per-series execution failure reason text");
            t.expect(bad_subject_report.failures[0].kind == PanelFailureKind::ExecutionFailed &&
                    bad_subject_report.failures[1].kind == PanelFailureKind::ExecutionFailed,
                "panel report classifies execution failures explicitly");
        }

        return true;
    }

    bool test_planspec_compile_determinism(ContractAsserts& t) {
        t.out << "[compile_determinism] PlanSpec compile determinism (graph + param binding)\n";

        PlanSpec spec{};
        spec.version = 1;
        spec.name = "contract_compile_det";
        spec.trade_mode = TradeMode::LongOnly;
        spec.params["n_fast"] = static_cast<std::int64_t>(3);
        spec.params["thr"] = 101.0;

        NodeSpec n_sma{};
        n_sma.id = "sma";
        n_sma.op = "ts.sma";
        n_sma.args = { "c" };
        n_sma.params["n"] = ParamRef{ "n_fast" };

        NodeSpec n_c{};
        n_c.id = "c";
        n_c.op = "core.close";

        NodeSpec n_th{};
        n_th.id = "th";
        n_th.op = "core.const";
        n_th.params["value"] = ParamRef{ "thr" };

        spec.numeric_nodes = { n_sma, n_c, n_th };

        NodeSpec b_entry{};
        b_entry.id = "entry";
        b_entry.op = "cmp.gt";
        b_entry.args = { "sma", "th" };

        NodeSpec b_exit{};
        b_exit.id = "exit";
        b_exit.op = "cmp.lt";
        b_exit.args = { "sma", "th" };

        spec.bool_nodes = { b_entry, b_exit };
        spec.bind.entry_long = "entry";
        spec.bind.exit_long = "exit";

        PlanSpec perm = spec;
        perm.numeric_nodes = { n_th, n_sma, n_c };
        perm.bool_nodes = { b_exit, b_entry };
        perm.params.clear();
        perm.params["thr"] = 101.0;
        perm.params["n_fast"] = static_cast<std::int64_t>(3);

        const auto registry = make_default_registry();
        CompileEnv env{};
        CompileResult c1{};
        CompileResult c2{};
        CompileResult c3{};
        std::string err1;
        std::string err2;
        std::string err3;
        const bool ok1 = compile_plan(spec, registry, env, c1, err1);
        const bool ok2 = compile_plan(spec, registry, env, c2, err2);
        const bool ok3 = compile_plan(perm, registry, env, c3, err3);

        t.expect(ok1, "compile pass #1 succeeds");
        t.expect(ok2, "compile pass #2 succeeds");
        t.expect(ok3, "compile pass #3 succeeds with reordered declarations");
        if (!ok1 || !ok2 || !ok3) {
            if (!ok1) t.out << "    compile#1 error: " << err1 << "\n";
            if (!ok2) t.out << "    compile#2 error: " << err2 << "\n";
            if (!ok3) t.out << "    compile#3 error: " << err3 << "\n";
            return true;
        }

        const auto gsig1 = canonical_graph_signature(spec);
        const auto gsig2 = canonical_graph_signature(perm);
        t.expect(gsig1 == gsig2, "canonical graph signature is stable across declaration order");

        std::string rsig1;
        std::string rsig2;
        std::string rerr;
        const bool rok1 = build_resolved_binding_signature(spec, registry, rsig1, rerr);
        const bool rok2 = build_resolved_binding_signature(perm, registry, rsig2, rerr);
        t.expect(rok1, "resolved binding signature build succeeds for base PlanSpec");
        t.expect(rok2, "resolved binding signature build succeeds for reordered PlanSpec");
        if (rok1 && rok2) {
            t.expect(rsig1 == rsig2, "resolved parameter bindings are stable across declaration order");
        }

        t.expect(c1.plan.name == c2.plan.name, "compiled plan name is stable");
        t.expect(c1.plan.mode == c2.plan.mode, "compiled trade mode is stable");
        t.expect(c1.exec.eval_mode == c2.exec.eval_mode, "compiled exec settings are stable");
        t.expect(c1.plan.first_valid_index == c2.plan.first_valid_index, "compiled warmup boundary is stable");
        t.expect(c1.plan.first_valid_index == 2, "param-ref binding resolves n_fast=3 into warmup=2");
        t.expect(c3.plan.first_valid_index == c1.plan.first_valid_index, "reordered PlanSpec compile keeps warmup boundary");

        const std::vector<double> close = {
            100.0, 101.0, 102.0, 103.0, 104.0, 105.0, 106.0, 107.0
        };
        std::shared_ptr<align::AlignedBundle> bundle;
        ColumnRef primary{};
        const bool okdv = make_single_series_dataview_from_close(close, bundle, primary);
        t.expect(okdv && bundle != nullptr, "compile determinism test creates DataView input");
        if (!okdv || !bundle) return true;

        const DataView dv(*bundle);
        const StrategyExecutor ex{};
        const auto r1 = ex.run(dv, primary, c1.plan, c1.exec);
        const auto r2 = ex.run(dv, primary, c2.plan, c2.exec);
        const auto r3 = ex.run(dv, primary, c3.plan, c3.exec);
        t.expect(backtest_equal(r1, r2), "compiled plans execute identically on fixed input");
        t.expect(backtest_equal(r1, r3), "reordered PlanSpec executes identically on fixed input");
        return true;
    }

    bool test_compile_warmup_semantics_contracts(ContractAsserts& t) {
        t.out << "[compile_warmup_semantics] execution warmup boundary follows bound strategy signals\n";

        const auto registry = make_default_registry();
        CompileEnv env{};

        auto make_spec = []() {
            PlanSpec spec{};
            spec.version = 1;
            spec.name = "compile_warmup_semantics";
            spec.trade_mode = TradeMode::LongOnly;
            spec.numeric_nodes = {
                NodeSpec{ "c", "core.close", {}, {} },
                NodeSpec{ "slow", "ts.sma", { "c" }, { {"n", static_cast<std::int64_t>(5)} } },
                NodeSpec{ "zero", "core.const", {}, { {"value", 0.0} } },
                NodeSpec{ "one", "core.const", {}, { {"value", 1.0} } },
            };
            spec.bool_nodes = {
                NodeSpec{ "entry_now", "cmp.gt", { "one", "zero" }, {} },
                NodeSpec{ "delayed_gate", "cmp.gt", { "slow", "zero" }, {} },
            };
            spec.bind.entry_long = "entry_now";
            spec.bind.exit_long = "";
            spec.bind.entry_short = "";
            spec.bind.exit_short = "";
            spec.bind.allow_trade = "";
            return spec;
        };

        {
            PlanSpec spec = make_spec();
            CompileResult compiled{};
            std::string err;
            const bool ok = compile_plan(spec, registry, env, compiled, err);
            t.expect(ok, "warmup semantics base compile succeeds");
            if (!ok) {
                t.out << "    compile error: " << err << "\n";
                return true;
            }
            t.expect(compiled.plan.first_valid_index == 0,
                "unbound delayed feature branches do not delay execution warmup");
        }

        {
            PlanSpec spec = make_spec();
            spec.bind.allow_trade = "delayed_gate";
            CompileResult compiled{};
            std::string err;
            const bool ok = compile_plan(spec, registry, env, compiled, err);
            t.expect(ok, "warmup semantics allow-trade compile succeeds");
            if (!ok) {
                t.out << "    compile error: " << err << "\n";
                return true;
            }
            t.expect(compiled.plan.first_valid_index == 4,
                "binding a delayed gate moves execution warmup to that bound signal");
        }

        {
            PlanSpec spec = make_spec();
            spec.bind.entry_long = "";
            spec.bind.allow_trade = "delayed_gate";
            CompileResult compiled{};
            std::string err;
            const bool ok = compile_plan(spec, registry, env, compiled, err);
            t.expect(ok, "warmup semantics gate-only compile succeeds");
            if (!ok) {
                t.out << "    compile error: " << err << "\n";
                return true;
            }
            t.expect(compiled.plan.first_valid_index == 4,
                "execution warmup still follows bound allow-trade gate when entry is unbound");
        }

        return true;
    }

    std::vector<double> seeded_close_series(std::uint32_t seed, std::size_t n) {
        std::vector<double> out;
        out.reserve(n);
        std::uint32_t state = seed;
        for (std::size_t i = 0; i < n; ++i) {
            state = state * 1664525u + 1013904223u;
            out.push_back(100.0 + static_cast<double>(state % 1000u) / 100.0);
        }
        return out;
    }

    bool test_executor_determinism(ContractAsserts& t) {
        t.out << "[executor_determinism] executor determinism (fixed seed -> stable hash/result)\n";

        constexpr std::uint32_t kSeed = 424242u;
        const auto close = seeded_close_series(kSeed, 6);

        std::shared_ptr<align::AlignedBundle> bundle;
        ColumnRef primary{};
        const bool okdv = make_single_series_dataview_from_close(close, bundle, primary);
        t.expect(okdv && bundle != nullptr, "executor determinism test creates DataView input");
        if (!okdv || !bundle) return true;

        StrategyPlan plan{};
        plan.name = "executor_determinism_contract";
        plan.mode = TradeMode::LongOnly;
        auto storage = std::make_shared<StrategyPlanStorage>();
        auto env = make_compile_env_storage();
        env->bool_leaf_values["entry_long_fixed"] = BoolBuffer(close.size(), 0u);
        env->bool_leaf_values["exit_long_fixed"] = BoolBuffer(close.size(), 0u);
        env->bool_leaf_values["allow_all"] = BoolBuffer(close.size(), 1u);
        if (close.size() > 1) env->bool_leaf_values["entry_long_fixed"][1] = 1u;
        if (close.size() > 3) env->bool_leaf_values["exit_long_fixed"][3] = 1u;

        storage->env = env;
        storage->program.bool_slot_count = 3;
        storage->program.bool_nodes.push_back(BoolProgramNode{
            "core.precomputed_bool",
            {},
            ResolvedParamMap{ {"name", std::string("entry_long_fixed")} },
            0
            });
        storage->program.bool_nodes.push_back(BoolProgramNode{
            "core.precomputed_bool",
            {},
            ResolvedParamMap{ {"name", std::string("exit_long_fixed")} },
            1
            });
        storage->program.bool_nodes.push_back(BoolProgramNode{
            "core.precomputed_bool",
            {},
            ResolvedParamMap{ {"name", std::string("allow_all")} },
            2
            });
        storage->program.entry_long_slot = 0;
        storage->program.exit_long_slot = 1;
        storage->program.allow_trade_slot = 2;
        plan.storage = std::move(storage);

        ExecSettings exec{};
        exec.eval_mode = BarEvalMode::SameBar;
        exec.fill_timing = FillTiming::SameBarClose;
        exec.close_out_at_end = true;
        exec.exits_first = true;
        exec.allow_same_bar_reversal = false;
        exec.costs = ExecCosts{};

        const DataView dv(*bundle);
        const StrategyExecutor ex{};
        const auto r1 = ex.run(dv, primary, plan, exec);
        const auto r2 = ex.run(dv, primary, plan, exec);
        t.expect(backtest_equal(r1, r2), "repeated executor runs are identical");

        constexpr std::uint64_t kExpectedHash = 0xb05a681b1b9abb69ULL;
        constexpr double kExpectedPnl = 3.9836766420520737;
        t.expect(r1.trades == 1, "executor fixed-signal scenario closes exactly one trade");
        t.expect(r1.long_trades == 1 && r1.short_trades == 0, "executor fixed-signal scenario trade sides are stable");
        t.expect(r1.trade_hash == kExpectedHash, "executor trade hash matches locked contract");
        t.expect(nearly_equal(r1.pnl, kExpectedPnl), "executor pnl matches locked contract");
        t.expect(r1.has_best_start_date, "executor best_start_date presence flag is stable");
        t.expect(r1.best_start_date == static_cast<TimeKey>(20240102), "executor best_start_date is stable");

        return true;
    }

} // namespace sugar::contract_suite_internal
