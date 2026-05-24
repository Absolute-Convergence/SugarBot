#include "contract_suite_internal.h"
#include "contract_suite_runtime_shared.h"
#include "strategy_plan_internal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <exception>
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
#include "engine/plan_compiler_internal.h"
#include "sweep/plan_sweep.h"
#include "engine/program_evaluator.h"
#include "engine/program_evaluator_internal.h"
#include "ingest/run_manifest.h"
#include "ingest/run_pipeline.h"
#include "op_kernels_topology.h"

namespace sugar::contract_suite_internal {

    bool test_static_graph_neighborhood_query_contracts(ContractAsserts& t) {
        t.out << "[static_graph_query] static graph NeighborhoodQuery contracts\n";

        auto expect_throws_with = [&](auto&& fn, std::string_view needle, const std::string& label) {
            try {
                fn();
                return t.expect(false, label + " throws");
            }
            catch (const std::exception& ex) {
                return t.expect(std::string(ex.what()).find(needle) != std::string::npos, label);
            }
        };

        {
            const std::vector<StaticGraphEdge> edges = {
                { 0, 2, 0.5 },
                { 0, 1, 0.75 },
                { 2, 0, 0.25 },
                { 0, 3, 2.0 },
                { 3, 0, 4.0 },
                { 0, 3, 1.0 },
                { 0, 0, 5.0 },
            };
            const StaticGraphNeighborhoodQuery q("contract.graph.directed_weighted", 4, edges, true, true);
            t.expect(q.topology_name() == "contract.graph.directed_weighted",
                "directed graph query exposes caller-provided topology name");
            t.expect(q.row_count() == 4,
                "directed graph query exposes row-domain boundedness");
            t.expect(!q.has_axis(AxisKind::Time) && !q.has_axis(AxisKind::Spatial),
                "graph query does not pretend to be a temporal or spatial axis");

            const auto outgoing_request = make_neighborhood_request(
                q.topology_name(),
                0,
                GraphNeighborhoodRequest::weighted_neighbors(GraphNeighborhoodDirection::Outgoing));
            const auto outgoing_first = q.query(outgoing_request);
            const auto outgoing_second = q.query(outgoing_request);
            t.expect(outgoing_first.topology_name == "contract.graph.directed_weighted" &&
                    outgoing_first.anchor.row_index == 0,
                "directed graph result preserves topology name and anchor row");
            t.expect(outgoing_first.indices == std::vector<std::size_t>({ 1, 2, 3 }),
                "directed graph outgoing query returns deterministic index ordering");
            t.expect(outgoing_first.weights == std::vector<double>({ 0.75, 0.5, 3.0 }),
                "directed graph outgoing weighted query aggregates duplicate edges deterministically");
            t.expect(outgoing_second.indices == outgoing_first.indices &&
                    outgoing_second.weights == outgoing_first.weights,
                "directed graph repeated query returns identical neighbor payloads");
            t.expect(outgoing_first.distances.empty() && outgoing_first.ordered && !outgoing_first.contiguous,
                "graph result is an ordered selection without metric distance payload");

            const auto capped = q.query(make_neighborhood_request(
                q.topology_name(),
                0,
                GraphNeighborhoodRequest::weighted_neighbors(
                    GraphNeighborhoodDirection::Outgoing,
                    false,
                    2)));
            t.expect(capped.indices == std::vector<std::size_t>({ 1, 2 }) &&
                    capped.weights == std::vector<double>({ 0.75, 0.5 }),
                "directed graph max-neighbor cap is deterministic after ordering");

            const auto unweighted_view = q.query(make_neighborhood_request(
                q.topology_name(),
                0,
                GraphNeighborhoodRequest::neighbors(GraphNeighborhoodDirection::Outgoing)));
            t.expect(unweighted_view.indices == outgoing_first.indices && unweighted_view.weights.empty(),
                "directed graph neighbor-set query omits weights even for weighted graphs");

            const auto incoming = q.query(make_neighborhood_request(
                q.topology_name(),
                0,
                GraphNeighborhoodRequest::weighted_neighbors(GraphNeighborhoodDirection::Incoming)));
            t.expect(incoming.indices == std::vector<std::size_t>({ 2, 3 }) &&
                    incoming.weights == std::vector<double>({ 0.25, 4.0 }),
                "directed graph incoming query follows edge direction");

            const auto both = q.query(make_neighborhood_request(
                q.topology_name(),
                0,
                GraphNeighborhoodRequest::weighted_neighbors(GraphNeighborhoodDirection::Both, true)));
            t.expect(both.indices == std::vector<std::size_t>({ 0, 1, 2, 3 }) &&
                    both.weights == std::vector<double>({ 5.0, 0.75, 0.75, 7.0 }),
                "directed graph bidirectional query aggregates incoming, outgoing, and explicit self edges");

            NeighborhoodRequest bad_direction = outgoing_request;
            bad_direction.set_param("graph.direction", static_cast<std::int64_t>(99));
            expect_throws_with(
                [&]() { (void)q.query(bad_direction); },
                "unknown graph.direction",
                "directed graph rejects unknown direction values");

            NeighborhoodRequest bad_cap = outgoing_request;
            bad_cap.set_param("graph.max_neighbors", static_cast<std::int64_t>(-1));
            expect_throws_with(
                [&]() { (void)q.query(bad_cap); },
                "graph.max_neighbors must be >= 0",
                "directed graph rejects negative neighbor caps");

            NeighborhoodRequest bad_kind = outgoing_request;
            bad_kind.query_kind = TopologyQueryKind::TransitionSet;
            expect_throws_with(
                [&]() { (void)q.query(bad_kind); },
                "only graph neighbor-set queries are supported",
                "directed graph rejects unsupported query kinds explicitly");

            NeighborhoodRequest bad_name = outgoing_request;
            bad_name.topology_name = "contract.graph.other";
            expect_throws_with(
                [&]() { (void)q.query(bad_name); },
                "unsupported topology name",
                "directed graph rejects mismatched topology names explicitly");
        }

        {
            const std::vector<StaticGraphEdge> edges = {
                { 0, 2, 9.0 },
                { 2, 1, 4.0 },
                { 3, 2, 6.0 },
                { 1, 2, 8.0 },
            };
            const StaticGraphNeighborhoodQuery q("contract.graph.undirected", 4, edges, false, false);
            const auto neighbors = q.query(make_neighborhood_request(
                q.topology_name(),
                2,
                GraphNeighborhoodRequest::neighbors()));
            t.expect(neighbors.indices == std::vector<std::size_t>({ 0, 1, 3 }) &&
                    neighbors.weights.empty(),
                "undirected graph neighbor-set query traverses both endpoints without weights");

            const auto weighted = q.query(make_neighborhood_request(
                q.topology_name(),
                2,
                GraphNeighborhoodRequest::weighted_neighbors()));
            t.expect(weighted.indices == std::vector<std::size_t>({ 0, 1, 3 }) &&
                    weighted.weights == std::vector<double>({ 1.0, 2.0, 1.0 }),
                "undirected unweighted graph can expose deterministic unit weights");
        }

        expect_throws_with(
            [&]() {
                const std::vector<StaticGraphEdge> edges = { { 0, 4, 1.0 } };
                (void)StaticGraphNeighborhoodQuery("contract.graph.bad_endpoint", 4, edges, true, true);
            },
            "edge endpoint out of range",
            "static graph constructor rejects out-of-range edge endpoints");

        expect_throws_with(
            [&]() {
                const std::vector<StaticGraphEdge> edges = {
                    { 0, 1, std::numeric_limits<double>::quiet_NaN() }
                };
                (void)StaticGraphNeighborhoodQuery("contract.graph.bad_weight", 4, edges, true, true);
            },
            "edge weight must be finite",
            "static graph constructor rejects non-finite edge weights");

        expect_throws_with(
            [&]() {
                const std::vector<StaticGraphEdge> edges = {};
                (void)StaticGraphNeighborhoodQuery("", 4, edges, true, true);
            },
            "topology name must not be empty",
            "static graph constructor rejects empty topology names");

        return true;
    }

    bool test_static_graph_runtime_binding_contracts(ContractAsserts& t) {
        t.out << "[static_graph_runtime_binding] static graph runtime binding and provenance contracts\n";

        const auto dir = contract_tmp_case_dir("static_graph_runtime_binding");
        std::filesystem::create_directories(dir);
        const auto csv = dir / "static_graph_runtime_binding.csv";
        const std::string csv_body =
            "date,close\n"
            "20240101,100\n"
            "20240102,101\n"
            "20240103,102\n"
            "20240104,103\n";
        if (!write_text_file(csv, csv_body)) {
            return t.expect(false, "static graph fixture csv is written");
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
            "      \"name\": \"row.trade.graph\",\n"
            "      \"bearing\": \"query_bearing\",\n"
            "      \"family\": \"relational_graph\",\n"
            "      \"domain_scope\": \"row_domain\",\n"
            "      \"query_kinds\": [\"neighbor_set\", \"weighted_neighbor_set\"],\n"
            "      \"directed\": true,\n"
            "      \"weighted\": true,\n"
            "      \"node_count\": 4,\n"
            "      \"artifact_ref\": {\n"
            "        \"stable_id\": \"row.trade.graph.v1\",\n"
            "        \"locator\": \"inline://layout/topologies/row.trade.graph\",\n"
            "        \"format\": \"inline_static_graph\",\n"
            "        \"digest\": \"sha256:row-trade-graph-v1\",\n"
            "        \"producer\": \"contract_fixture\"\n"
            "      },\n"
            "      \"edges\": [\n"
            "        {\"source\": 0, \"target\": 2, \"weight\": 0.5},\n"
            "        {\"source\": 0, \"target\": 1, \"weight\": 0.75},\n"
            "        {\"source\": 2, \"target\": 0, \"weight\": 0.25},\n"
            "        {\"source\": 0, \"target\": 3, \"weight\": 2.0},\n"
            "        {\"source\": 0, \"target\": 3, \"weight\": 1.0}\n"
            "      ]\n"
            "    }\n"
            "  ]\n"
            "}\n";

        LayoutSpec layout{};
        std::string err;
        const bool parse_ok = parse_layout_spec_json(layout_json, layout, err);
        t.expect(parse_ok, "LayoutSpec parses inline static graph topology");
        if (!parse_ok) {
            t.out << "    parse error: " << err << "\n";
            return false;
        }
        t.expect(layout.topologies.size() == 1,
            "LayoutSpec keeps declared static graph topology");
        if (!layout.topologies.empty()) {
            const auto& spec = layout.topologies.front();
            t.expect(spec.bearing == TopologyBearing::QueryBearing,
                "static graph layout preserves query-bearing classification");
            t.expect(spec.family == TopologyFamily::RelationalGraph,
                "static graph layout preserves graph family");
            t.expect(spec.node_count == 4,
                "static graph layout preserves node_count");
            t.expect(spec.static_edges.size() == 5,
                "static graph layout parses inline edge payload");
            t.expect(spec.artifact_ref && spec.artifact_ref->stable_id == "row.trade.graph.v1",
                "static graph layout preserves artifact provenance");
        }

        ResolvedObservationWorld world{};
        err.clear();
        const bool world_ok = build_resolved_observation_world(layout, csv.string(), world, err);
        t.expect(world_ok, "observation world mounts declared static graph topology");
        if (!world_ok) {
            t.out << "    world error: " << err << "\n";
            return false;
        }
        t.expect(world.topology_catalog && world.topology_catalog->has("row.trade.graph"),
            "topology catalog exposes declared static graph by name");
        const auto* mounted = world.topology_catalog ? world.topology_catalog->find("row.trade.graph") : nullptr;
        if (mounted) {
            t.expect(mounted->bearing == TopologyBearing::QueryBearing,
                "mounted static graph remains query-bearing");
            t.expect(mounted->static_edges.size() == 5,
                "mounted static graph keeps edge payload on the sidecar");
        }

        const auto manifest_it = std::find_if(
            world.run_manifest.topologies.begin(),
            world.run_manifest.topologies.end(),
            [](const TopologySpec& spec) { return spec.name == "row.trade.graph"; });
        t.expect(manifest_it != world.run_manifest.topologies.end(),
            "run manifest snapshots declared static graph topology provenance");
        if (manifest_it != world.run_manifest.topologies.end()) {
            t.expect(manifest_it->origin == TopologyOrigin::LayoutDeclared &&
                    manifest_it->bearing == TopologyBearing::QueryBearing,
                "run manifest preserves graph origin and bearing");
            t.expect(manifest_it->artifact_ref &&
                    manifest_it->artifact_ref->stable_id == "row.trade.graph.v1",
                "run manifest preserves graph artifact stable id");
        }

        const DataView dv(*world.aligned_bundle);
        RuntimeLocalitySubject observation_subject{};
        observation_subject.kind = LocalitySubjectKind::ObservationDomain;
        auto query = dv.resolve_topology(observation_subject, "row.trade.graph");
        t.expect(query != nullptr,
            "DataView resolves query-bearing static graph topology");
        if (query) {
            const auto result = query->query(make_neighborhood_request(
                query->topology_name(),
                0,
                GraphNeighborhoodRequest::weighted_neighbors(GraphNeighborhoodDirection::Outgoing)));
            t.expect(result.indices == std::vector<std::size_t>({ 1, 2, 3 }) &&
                    result.weights == std::vector<double>({ 0.75, 0.5, 3.0 }),
                "resolved static graph query returns deterministic weighted neighbors from LayoutSpec edges");
        }

        ResolvedRuntimeInput runtime{};
        err.clear();
        const bool bind_ok = bind_resolved_runtime_target(world, "close", RuntimeTargetSpec{}, runtime, err);
        t.expect(bind_ok, "static graph runtime target binding succeeds");
        if (!bind_ok) {
            t.out << "    bind error: " << err << "\n";
            return false;
        }

        OperatorRegistry registry = make_default_registry();
        {
            OpEntry graph{};
            graph.spec.name = "graph.weighted_mean";
            graph.spec.op_type = OpType::Numeric;
            graph.spec.inputs = { ValueType::Num };
            graph.spec.output = ValueType::Num;
            graph.spec.axis_scope = OpAxisScope::RowWise;
            graph.spec.required_topology_families = { TopologyFamily::RelationalGraph };
            graph.spec.required_topology_query_kinds = { TopologyQueryKind::WeightedNeighborSet };
            graph.spec.default_topology_name = "row.trade.graph";
            graph.spec.topology_binding_param = "topology";
            graph.spec.default_locality_subject_kind = LocalitySubjectKind::ObservationDomain;
            registry.add(std::move(graph));
        }

        PlanSpec plan{};
        plan.version = 1;
        plan.name = "contract_static_graph_runtime_binding";
        plan.numeric_nodes = {
            NodeSpec{ "c", "core.close", {}, {} },
            NodeSpec{ "graph_mean", "graph.weighted_mean", { "c" }, {} },
        };

        const auto report = diagnose_plan_capabilities(plan, registry, runtime);
        t.expect(report.status == PlanCapabilityStatus::Valid,
            "plan capability accepts runtime-resolved static graph topology");
        append_plan_capability_snapshot(report, runtime.run_manifest);
        const auto* usage = find_topology_usage(runtime.run_manifest, "graph_mean", "row.trade.graph");
        t.expect(usage != nullptr,
            "topology usage snapshot records static graph plan-node usage");
        if (usage) {
            t.expect(usage->catalog_present && usage->runtime_resolved,
                "topology usage records graph catalog presence and runtime resolution");
            t.expect(usage->topology_origin == "layout_declared" &&
                    usage->topology_bearing == "query_bearing",
                "topology usage preserves graph origin and query-bearing classification");
            t.expect(usage->artifact_ref.stable_id == "row.trade.graph.v1",
                "topology usage preserves graph artifact stable id");
            t.expect(usage->locality_subject_kind == "observation_domain",
                "topology usage records graph query subject provenance");
        }

        return true;
    }

} // namespace sugar::contract_suite_internal
