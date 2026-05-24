#include "contract_suite_internal.h"
#include "contract_suite_runtime_shared.h"
#include "strategy_plan_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "engine/plan_compiler.h"
#include "engine/plan_compiler_internal.h"
#include "engine/program_evaluator.h"
#include "engine/program_evaluator_internal.h"
#include "ingest/run_pipeline.h"
#include "op_registry_internal.h"
#include "ops/op_registry.h"
#include "sweep/plan_capability.h"

namespace sugar::contract_suite_internal {

    bool test_hierarchy_query_runtime_contracts(ContractAsserts& t) {
        t.out << "[hierarchy_query_runtime] hierarchy query provider, binding, and operator contracts\n";

        auto expect_throws_with = [&](auto&& fn, std::string_view needle, const std::string& label) {
            try {
                fn();
                return t.expect(false, label + " throws");
            }
            catch (const std::exception& ex) {
                return t.expect(std::string(ex.what()).find(needle) != std::string::npos, label);
            }
        };

        const std::vector<StaticGraphEdge> hierarchy_edges = {
            { 0, 1, 1.0 },
            { 0, 2, 1.0 },
            { 1, 3, 1.0 },
            { 1, 4, 1.0 },
            { 2, 5, 1.0 },
        };
        const HierarchyNeighborhoodQuery hierarchy("contract.hierarchy", 6, hierarchy_edges);

        t.expect(hierarchy.topology_name() == "contract.hierarchy",
            "hierarchy query exposes caller-provided topology name");
        t.expect(hierarchy.row_count() == 6,
            "hierarchy query exposes row-domain boundedness");
        t.expect(!hierarchy.has_axis(AxisKind::Time) && !hierarchy.has_axis(AxisKind::Spatial),
            "hierarchy query does not pretend to be a temporal or spatial axis");

        const auto parent = hierarchy.query(make_neighborhood_request(
            hierarchy.topology_name(),
            3,
            HierarchyNeighborhoodRequest::parent()));
        t.expect(parent.indices == std::vector<std::size_t>({ 1 }),
            "hierarchy parent traversal returns the direct parent");

        const auto ancestors = hierarchy.query(make_neighborhood_request(
            hierarchy.topology_name(),
            3,
            HierarchyNeighborhoodRequest::parent(false, 2)));
        t.expect(ancestors.indices == std::vector<std::size_t>({ 1, 0 }),
            "hierarchy parent traversal honors max-depth ancestor bounds");

        const auto root_parent = hierarchy.query(make_neighborhood_request(
            hierarchy.topology_name(),
            0,
            HierarchyNeighborhoodRequest::parent()));
        t.expect(root_parent.indices.empty(),
            "hierarchy parent traversal handles missing parents");

        const auto children = hierarchy.query(make_neighborhood_request(
            hierarchy.topology_name(),
            0,
            HierarchyNeighborhoodRequest::children()));
        t.expect(children.indices == std::vector<std::size_t>({ 1, 2 }),
            "hierarchy child traversal returns deterministic direct children");

        const auto descendants = hierarchy.query(make_neighborhood_request(
            hierarchy.topology_name(),
            0,
            HierarchyNeighborhoodRequest::children(false, 2)));
        t.expect(descendants.indices == std::vector<std::size_t>({ 1, 2, 3, 4, 5 }),
            "hierarchy child traversal expands descendants by bounded depth");

        const auto capped_descendants = hierarchy.query(make_neighborhood_request(
            hierarchy.topology_name(),
            0,
            HierarchyNeighborhoodRequest::children(false, 2, 3)));
        t.expect(capped_descendants.indices == std::vector<std::size_t>({ 1, 2, 3 }),
            "hierarchy child traversal honors max-neighbor caps");

        const auto leaf_children = hierarchy.query(make_neighborhood_request(
            hierarchy.topology_name(),
            5,
            HierarchyNeighborhoodRequest::children()));
        t.expect(leaf_children.indices.empty(),
            "hierarchy child traversal handles missing children");

        NeighborhoodRequest sibling_request{};
        sibling_request.topology_name = std::string(hierarchy.topology_name());
        sibling_request.anchor.row_index = 4;
        sibling_request.query_kind = TopologyQueryKind::NeighborSet;
        sibling_request.include_anchor = false;
        sibling_request.set_param("hierarchy.traversal", std::string("siblings"));
        const auto siblings = hierarchy.query(sibling_request);
        t.expect(siblings.indices == std::vector<std::size_t>({ 3 }),
            "hierarchy sibling traversal returns deterministic same-parent siblings");

        const auto graph_incoming = hierarchy.query(make_neighborhood_request(
            hierarchy.topology_name(),
            4,
            GraphNeighborhoodRequest::neighbors(GraphNeighborhoodDirection::Incoming)));
        t.expect(graph_incoming.indices == std::vector<std::size_t>({ 1 }),
            "hierarchy query maps incoming graph direction to parent traversal");

        const auto graph_outgoing = hierarchy.query(make_neighborhood_request(
            hierarchy.topology_name(),
            1,
            GraphNeighborhoodRequest::neighbors(GraphNeighborhoodDirection::Outgoing)));
        t.expect(graph_outgoing.indices == std::vector<std::size_t>({ 3, 4 }),
            "hierarchy query maps outgoing graph direction to child traversal");

        const auto parent_and_children = hierarchy.query(make_neighborhood_request(
            hierarchy.topology_name(),
            1,
            GraphNeighborhoodRequest::neighbors(GraphNeighborhoodDirection::Both)));
        t.expect(parent_and_children.indices == std::vector<std::size_t>({ 0, 3, 4 }),
            "hierarchy query maps both graph direction to parent-and-child traversal");

        NeighborhoodRequest bad_traversal = make_neighborhood_request(
            hierarchy.topology_name(),
            1,
            HierarchyNeighborhoodRequest::children());
        bad_traversal.set_param("hierarchy.traversal", std::string("cousins"));
        expect_throws_with(
            [&]() { (void)hierarchy.query(bad_traversal); },
            "unknown hierarchy.traversal",
            "hierarchy query rejects unknown traversal params explicitly");

        NeighborhoodRequest bad_depth = make_neighborhood_request(
            hierarchy.topology_name(),
            1,
            HierarchyNeighborhoodRequest::children());
        bad_depth.set_param("hierarchy.max_depth", static_cast<std::int64_t>(-1));
        expect_throws_with(
            [&]() { (void)hierarchy.query(bad_depth); },
            "hierarchy.max_depth must be >= 0",
            "hierarchy query rejects negative depth params explicitly");

        NeighborhoodRequest bad_kind = make_neighborhood_request(
            hierarchy.topology_name(),
            1,
            HierarchyNeighborhoodRequest::children());
        bad_kind.query_kind = TopologyQueryKind::WeightedNeighborSet;
        expect_throws_with(
            [&]() { (void)hierarchy.query(bad_kind); },
            "only hierarchy neighbor-set queries are supported",
            "hierarchy query rejects unsupported query kinds explicitly");

        expect_throws_with(
            [&]() {
                (void)HierarchyNeighborhoodQuery(
                    "contract.hierarchy.multiple_parents",
                    3,
                    std::vector<StaticGraphEdge>{ { 0, 2, 1.0 }, { 1, 2, 1.0 } });
            },
            "multiple parents",
            "hierarchy constructor rejects child nodes with multiple parents");

        expect_throws_with(
            [&]() {
                (void)HierarchyNeighborhoodQuery(
                    "contract.hierarchy.cycle",
                    3,
                    std::vector<StaticGraphEdge>{ { 0, 1, 1.0 }, { 1, 2, 1.0 }, { 2, 0, 1.0 } });
            },
            "cycle detected",
            "hierarchy constructor rejects cycles");

        const auto dir = contract_tmp_case_dir("hierarchy_query_runtime");
        std::filesystem::create_directories(dir);
        const auto csv = dir / "hierarchy_query_runtime.csv";
        const std::string csv_body =
            "date,close\n"
            "20240101,10\n"
            "20240102,20\n"
            "20240103,30\n"
            "20240104,40\n"
            "20240105,50\n"
            "20240106,60\n";
        if (!write_text_file(csv, csv_body)) {
            return t.expect(false, "hierarchy runtime fixture csv is written");
        }

        const std::string layout_json =
            "{\n"
            "  \"ingest\": {\n"
            "    \"header_policy\": \"RequiredHeaders\",\n"
            "    \"time_col\": \"date\",\n"
            "    \"time_kind\": \"YyyyMmDd\",\n"
            "    \"value_cols\": [\"close\"],\n"
            "    \"primary_value_col\": \"close\",\n"
            "    \"sort_by_time\": true,\n"
            "    \"dedupe\": \"Error\"\n"
            "  },\n"
            "  \"topologies\": [\n"
            "    {\n"
            "      \"name\": \"row.account.hierarchy\",\n"
            "      \"bearing\": \"query_bearing\",\n"
            "      \"family\": \"hierarchy\",\n"
            "      \"domain_scope\": \"row_domain\",\n"
            "      \"query_kinds\": [\"neighbor_set\"],\n"
            "      \"node_count\": 6,\n"
            "      \"artifact_ref\": {\n"
            "        \"stable_id\": \"row.account.hierarchy.v1\",\n"
            "        \"locator\": \"inline://layout/topologies/row.account.hierarchy\",\n"
            "        \"format\": \"inline_hierarchy\",\n"
            "        \"digest\": \"sha256:row-account-hierarchy-v1\",\n"
            "        \"producer\": \"contract_fixture\"\n"
            "      },\n"
            "      \"edges\": [\n"
            "        {\"source\": 0, \"target\": 1},\n"
            "        {\"source\": 0, \"target\": 2},\n"
            "        {\"source\": 1, \"target\": 3},\n"
            "        {\"source\": 1, \"target\": 4},\n"
            "        {\"source\": 2, \"target\": 5}\n"
            "      ]\n"
            "    }\n"
            "  ]\n"
            "}\n";

        LayoutSpec layout{};
        std::string err;
        const bool parse_ok = parse_layout_spec_json(layout_json, layout, err);
        t.expect(parse_ok, "LayoutSpec parses inline hierarchy topology");
        if (!parse_ok) {
            t.out << "    parse error: " << err << "\n";
            return false;
        }
        t.expect(!layout.topologies.empty() &&
                layout.topologies.front().family == TopologyFamily::Hierarchy,
            "LayoutSpec preserves hierarchy topology family");

        ResolvedObservationWorld world{};
        err.clear();
        const bool world_ok = build_resolved_observation_world(layout, csv.string(), world, err);
        t.expect(world_ok, "observation world mounts declared hierarchy topology");
        if (!world_ok) {
            t.out << "    world error: " << err << "\n";
            return false;
        }
        t.expect(world.topology_catalog && world.topology_catalog->has("row.account.hierarchy"),
            "topology catalog exposes declared hierarchy by name");

        const DataView dv(*world.aligned_bundle);
        RuntimeLocalitySubject observation_subject{};
        observation_subject.kind = LocalitySubjectKind::ObservationDomain;
        auto query = dv.resolve_topology(observation_subject, "row.account.hierarchy");
        t.expect(query != nullptr,
            "DataView resolves query-bearing hierarchy topology");
        if (query) {
            const auto resolved_children = query->query(make_neighborhood_request(
                query->topology_name(),
                0,
                HierarchyNeighborhoodRequest::children(false, 2)));
            t.expect(resolved_children.indices == std::vector<std::size_t>({ 1, 2, 3, 4, 5 }),
                "resolved hierarchy query returns deterministic descendants from LayoutSpec edges");
        }

        if (world.topology_catalog) {
            TopologySpec invalid_hierarchy{};
            invalid_hierarchy.name = "row.account.invalid_hierarchy";
            invalid_hierarchy.bearing = TopologyBearing::QueryBearing;
            invalid_hierarchy.family = TopologyFamily::Hierarchy;
            invalid_hierarchy.domain_scope = TopologyDomainScope::RowDomain;
            invalid_hierarchy.query_kinds = { TopologyQueryKind::NeighborSet };
            invalid_hierarchy.node_count = 3;
            invalid_hierarchy.static_edges = {
                { 0, 2, 1.0 },
                { 1, 2, 1.0 },
            };
            std::string add_err;
            const bool add_invalid_ok = world.topology_catalog->add(std::move(invalid_hierarchy), &add_err);
            t.expect(add_invalid_ok,
                "contract can mount invalid hierarchy fixture for runtime diagnostic coverage");
            if (add_invalid_ok) {
                expect_throws_with(
                    [&]() { (void)dv.resolve_topology(observation_subject, "row.account.invalid_hierarchy"); },
                    "could not be materialized",
                    "DataView hierarchy materialization failures include topology context");
                expect_throws_with(
                    [&]() { (void)dv.resolve_topology(observation_subject, "row.account.invalid_hierarchy"); },
                    "multiple parents",
                    "DataView hierarchy materialization failures preserve provider diagnostic detail");
            }
        }

        ResolvedRuntimeInput runtime{};
        err.clear();
        const bool bind_ok = bind_resolved_runtime_target(world, "close", RuntimeTargetSpec{}, runtime, err);
        t.expect(bind_ok, "hierarchy runtime target binding succeeds");
        if (!bind_ok) {
            t.out << "    bind error: " << err << "\n";
            return false;
        }

        PlanSpec spec{};
        spec.version = 1;
        spec.name = "contract_hierarchy_query_runtime";
        spec.numeric_nodes = {
            NodeSpec{ "c", "core.close", {}, {} },
            NodeSpec{ "child_mean", "topology.neighbor_mean", { "c" }, {
                { "topology", std::string("row.account.hierarchy") },
            } },
            NodeSpec{ "parent_mean", "topology.neighbor_mean", { "c" }, {
                { "topology", std::string("row.account.hierarchy") },
                { "direction", std::string("incoming") },
            } },
            NodeSpec{ "ancestor_mean", "topology.neighbor_mean", { "c" }, {
                { "topology", std::string("row.account.hierarchy") },
                { "traversal", std::string("parent") },
                { "max_depth", static_cast<std::int64_t>(2) },
            } },
            NodeSpec{ "descendant_mean", "topology.neighbor_mean", { "c" }, {
                { "topology", std::string("row.account.hierarchy") },
                { "traversal", std::string("children") },
                { "max_depth", static_cast<std::int64_t>(2) },
            } },
            NodeSpec{ "sibling_mean", "topology.neighbor_mean", { "c" }, {
                { "topology", std::string("row.account.hierarchy") },
                { "traversal", std::string("siblings") },
            } },
            NodeSpec{ "child_degree", "topology.degree", {}, {
                { "topology", std::string("row.account.hierarchy") },
            } },
            NodeSpec{ "family_degree", "topology.degree", {}, {
                { "topology", std::string("row.account.hierarchy") },
                { "traversal", std::string("parent_and_children") },
            } },
        };

        const auto registry = make_default_registry();
        CompileEnv env{};
        CompiledPlanTemplate plan_template{};
        std::vector<NamedNumericOutput> numeric_outputs;
        err.clear();
        const bool compile_ok = compile_plan_template_with_numeric_outputs(
            spec,
            registry,
            env,
            plan_template,
            numeric_outputs,
            err);
        t.expect(compile_ok, "hierarchy topology operator PlanSpec compiles");
        if (!compile_ok) {
            t.out << "    compile error: " << err << "\n";
            return false;
        }

        CompileResult compiled{};
        err.clear();
        const bool instantiate_ok = instantiate_compiled_plan(plan_template, nullptr, compiled, err);
        t.expect(instantiate_ok, "hierarchy topology operator PlanSpec instantiates");
        if (!instantiate_ok) {
            t.out << "    instantiate error: " << err << "\n";
            return false;
        }

        ProgramEvaluationBuffers buffers{};
        err.clear();
        const bool eval_ok = evaluate_strategy_plan_program_buffers(
            DataView(*runtime.aligned_bundle),
            runtime.primary_ref,
            compiled.plan,
            buffers,
            err);
        t.expect(eval_ok, "topology operators evaluate over hierarchy provider");
        if (!eval_ok) {
            t.out << "    eval error: " << err << "\n";
            return false;
        }

        const auto output_slot = [&](std::string_view id) -> std::optional<std::size_t> {
            for (const auto& output : numeric_outputs) {
                if (output.node_id == id) return output.output_slot;
            }
            return std::nullopt;
        };
        const auto output_buffer = [&](std::string_view id) -> const NumericBuffer* {
            const auto slot = output_slot(id);
            if (!slot.has_value() || *slot >= buffers.numeric_slots.size()) return nullptr;
            return &buffers.numeric_slots[*slot];
        };

        const auto* child_mean = output_buffer("child_mean");
        const auto* parent_mean = output_buffer("parent_mean");
        const auto* ancestor_mean = output_buffer("ancestor_mean");
        const auto* descendant_mean = output_buffer("descendant_mean");
        const auto* sibling_mean = output_buffer("sibling_mean");
        const auto* child_degree = output_buffer("child_degree");
        const auto* family_degree = output_buffer("family_degree");
        t.expect(child_mean && child_mean->size() == 6 &&
                nearly_equal((*child_mean)[0], 25.0) &&
                nearly_equal((*child_mean)[1], 45.0) &&
                nearly_equal((*child_mean)[2], 60.0) &&
                std::isnan((*child_mean)[3]),
            "topology.neighbor_mean reuses hierarchy children traversal");
        t.expect(parent_mean && parent_mean->size() == 6 &&
                std::isnan((*parent_mean)[0]) &&
                nearly_equal((*parent_mean)[3], 20.0) &&
                nearly_equal((*parent_mean)[5], 30.0),
            "topology.neighbor_mean reuses hierarchy parent traversal");
        t.expect(ancestor_mean && ancestor_mean->size() == 6 &&
                std::isnan((*ancestor_mean)[0]) &&
                nearly_equal((*ancestor_mean)[1], 10.0) &&
                nearly_equal((*ancestor_mean)[3], 15.0) &&
                nearly_equal((*ancestor_mean)[5], 20.0),
            "topology.neighbor_mean exposes bounded hierarchy ancestor traversal");
        t.expect(descendant_mean && descendant_mean->size() == 6 &&
                nearly_equal((*descendant_mean)[0], 40.0) &&
                nearly_equal((*descendant_mean)[1], 45.0) &&
                nearly_equal((*descendant_mean)[2], 60.0) &&
                std::isnan((*descendant_mean)[3]),
            "topology.neighbor_mean exposes bounded hierarchy descendant traversal");
        t.expect(sibling_mean && sibling_mean->size() == 6 &&
                std::isnan((*sibling_mean)[0]) &&
                nearly_equal((*sibling_mean)[1], 30.0) &&
                nearly_equal((*sibling_mean)[3], 50.0) &&
                std::isnan((*sibling_mean)[5]),
            "topology.neighbor_mean exposes hierarchy sibling traversal from PlanSpec params");
        t.expect(child_degree && child_degree->size() == 6 &&
                nearly_equal((*child_degree)[0], 2.0) &&
                nearly_equal((*child_degree)[1], 2.0) &&
                nearly_equal((*child_degree)[3], 0.0),
            "topology.degree counts hierarchy children");
        t.expect(family_degree && family_degree->size() == 6 &&
                nearly_equal((*family_degree)[0], 2.0) &&
                nearly_equal((*family_degree)[1], 3.0) &&
                nearly_equal((*family_degree)[3], 1.0),
            "topology.degree exposes hierarchy parent-and-child traversal from PlanSpec params");

        auto capability = diagnose_plan_capabilities(spec, registry, runtime);
        t.expect(capability.status == PlanCapabilityStatus::Valid,
            "plan capability accepts generic topology operators over hierarchy topology");
        append_plan_capability_snapshot(capability, runtime.run_manifest);
        const auto* usage = find_topology_usage(runtime.run_manifest, "child_mean", "row.account.hierarchy");
        t.expect(usage && usage->runtime_resolved,
            "topology usage snapshot records runtime-resolved hierarchy provenance");
        const auto manifest_it = std::find_if(
            runtime.run_manifest.topologies.begin(),
            runtime.run_manifest.topologies.end(),
            [](const TopologySpec& spec) { return spec.name == "row.account.hierarchy"; });
        t.expect(manifest_it != runtime.run_manifest.topologies.end() &&
                manifest_it->family == TopologyFamily::Hierarchy,
            "run manifest preserves hierarchy topology family");
        if (usage) {
            t.expect(usage->topology_bearing == "query_bearing" &&
                    usage->artifact_ref.stable_id == "row.account.hierarchy.v1",
                "topology usage snapshot preserves hierarchy artifact provenance");
            t.expect(usage->required_topology_query_kinds ==
                    std::vector<std::string>{ std::string(topology_query_kind_name(TopologyQueryKind::NeighborSet)) },
                "topology usage snapshot records hierarchy neighbor-set requirement");
        }

        PlanSpec invalid_traversal = spec;
        invalid_traversal.name = "contract_hierarchy_query_invalid_traversal";
        invalid_traversal.numeric_nodes = {
            NodeSpec{ "bad", "topology.degree", {}, {
                { "topology", std::string("row.account.hierarchy") },
                { "traversal", std::string("cousins") },
            } },
        };
        CompiledPlanTemplate invalid_template{};
        std::vector<NamedNumericOutput> invalid_outputs;
        err.clear();
        const bool invalid_compile_ok = compile_plan_template_with_numeric_outputs(
            invalid_traversal,
            registry,
            env,
            invalid_template,
            invalid_outputs,
            err);
        t.expect(invalid_compile_ok,
            "hierarchy traversal validation leaves shape-valid PlanSpec compilation intact");
        if (invalid_compile_ok) {
            CompileResult invalid_compiled{};
            err.clear();
            const bool invalid_instantiate_ok = instantiate_compiled_plan(invalid_template, nullptr, invalid_compiled, err);
            t.expect(invalid_instantiate_ok,
                "hierarchy invalid traversal PlanSpec instantiates before runtime param validation");
            if (invalid_instantiate_ok) {
                ProgramEvaluationBuffers invalid_buffers{};
                err.clear();
                const bool invalid_eval_ok = evaluate_strategy_plan_program_buffers(
                    DataView(*runtime.aligned_bundle),
                    runtime.primary_ref,
                    invalid_compiled.plan,
                    invalid_buffers,
                    err);
                t.expect(!invalid_eval_ok &&
                        err.find("unknown topology hierarchy traversal") != std::string::npos,
                    "hierarchy topology operator reports unknown traversal params explicitly");
            }
        }

        return true;
    }

} // namespace sugar::contract_suite_internal
