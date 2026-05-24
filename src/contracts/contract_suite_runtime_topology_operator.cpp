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

    bool test_topology_operator_runtime_contracts(ContractAsserts& t) {
        t.out << "[topology_operator_runtime] generic topology operator runtime contracts\n";

        const std::vector<double> values = { 10.0, 20.0, 30.0, 40.0 };
        const std::vector<StaticGraphEdge> edges = {
            { 0, 1, 2.0 },
            { 0, 2, 0.5 },
            { 1, 2, 1.0 },
            { 2, 0, 1.0 },
            { 2, 3, 4.0 },
        };
        const StaticGraphNeighborhoodQuery direct_query(
            "contract.graph.operators",
            values.size(),
            edges,
            true,
            true);

        const auto direct_mean = op_kernels::topology_neighbor_mean(
            values,
            direct_query,
            GraphNeighborhoodDirection::Outgoing,
            false,
            std::numeric_limits<std::size_t>::max());
        const auto direct_min = op_kernels::topology_neighbor_min(
            values,
            direct_query,
            GraphNeighborhoodDirection::Outgoing,
            false,
            std::numeric_limits<std::size_t>::max());
        const auto direct_max = op_kernels::topology_neighbor_max(
            values,
            direct_query,
            GraphNeighborhoodDirection::Outgoing,
            false,
            std::numeric_limits<std::size_t>::max());
        const auto direct_weighted = op_kernels::topology_weighted_sum(
            values,
            direct_query,
            GraphNeighborhoodDirection::Outgoing,
            false,
            std::numeric_limits<std::size_t>::max());
        const auto direct_degree = op_kernels::topology_degree(
            direct_query,
            GraphNeighborhoodDirection::Outgoing,
            false,
            std::numeric_limits<std::size_t>::max(),
            values.size());

        t.expect(direct_mean.size() == values.size() &&
                nearly_equal(direct_mean[0], 25.0) &&
                nearly_equal(direct_mean[1], 30.0) &&
                nearly_equal(direct_mean[2], 25.0) &&
                std::isnan(direct_mean[3]),
            "topology.neighbor_mean kernel aggregates outgoing neighbor values");
        t.expect(direct_min.size() == values.size() &&
                nearly_equal(direct_min[0], 20.0) &&
                nearly_equal(direct_min[1], 30.0) &&
                nearly_equal(direct_min[2], 10.0) &&
                std::isnan(direct_min[3]),
            "topology.neighbor_min kernel aggregates outgoing neighbor values");
        t.expect(direct_max.size() == values.size() &&
                nearly_equal(direct_max[0], 30.0) &&
                nearly_equal(direct_max[1], 30.0) &&
                nearly_equal(direct_max[2], 40.0) &&
                std::isnan(direct_max[3]),
            "topology.neighbor_max kernel aggregates outgoing neighbor values");
        t.expect(direct_weighted.size() == values.size() &&
                nearly_equal(direct_weighted[0], 55.0) &&
                nearly_equal(direct_weighted[1], 30.0) &&
                nearly_equal(direct_weighted[2], 170.0) &&
                nearly_equal(direct_weighted[3], 0.0),
            "topology.weighted_sum kernel combines weights with neighbor values");
        t.expect(direct_degree.size() == values.size() &&
                nearly_equal(direct_degree[0], 2.0) &&
                nearly_equal(direct_degree[1], 1.0) &&
                nearly_equal(direct_degree[2], 2.0) &&
                nearly_equal(direct_degree[3], 0.0),
            "topology.degree kernel counts bounded neighbor sets");

        const auto capped_mean = op_kernels::topology_neighbor_mean(
            values,
            direct_query,
            GraphNeighborhoodDirection::Outgoing,
            false,
            1);
        t.expect(capped_mean.size() == values.size() &&
                nearly_equal(capped_mean[0], 20.0) &&
                nearly_equal(capped_mean[2], 10.0),
            "topology kernels honor max_neighbors caps after deterministic provider ordering");

        const auto incoming_degree = op_kernels::topology_degree(
            direct_query,
            GraphNeighborhoodDirection::Incoming,
            false,
            std::numeric_limits<std::size_t>::max(),
            values.size());
        t.expect(incoming_degree.size() == values.size() &&
                nearly_equal(incoming_degree[0], 1.0) &&
                nearly_equal(incoming_degree[1], 1.0) &&
                nearly_equal(incoming_degree[2], 2.0) &&
                nearly_equal(incoming_degree[3], 1.0),
            "topology.degree honors incoming graph direction");

        const auto dir = contract_tmp_case_dir("topology_operator_runtime");
        std::filesystem::create_directories(dir);
        const auto csv = dir / "topology_operator_runtime.csv";
        const std::string csv_body =
            "date,close\n"
            "20240101,10\n"
            "20240102,20\n"
            "20240103,30\n"
            "20240104,40\n";
        if (!write_text_file(csv, csv_body)) {
            return t.expect(false, "topology operator fixture csv is written");
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
            "        \"stable_id\": \"row.trade.graph.operators.v1\",\n"
            "        \"locator\": \"inline://layout/topologies/row.trade.graph\",\n"
            "        \"format\": \"inline_static_graph\",\n"
            "        \"digest\": \"sha256:row-trade-graph-operators-v1\",\n"
            "        \"producer\": \"contract_fixture\"\n"
            "      },\n"
            "      \"edges\": [\n"
            "        {\"source\": 0, \"target\": 1, \"weight\": 2.0},\n"
            "        {\"source\": 0, \"target\": 2, \"weight\": 0.5},\n"
            "        {\"source\": 1, \"target\": 2, \"weight\": 1.0},\n"
            "        {\"source\": 2, \"target\": 0, \"weight\": 1.0},\n"
            "        {\"source\": 2, \"target\": 3, \"weight\": 4.0}\n"
            "      ]\n"
            "    }\n"
            "  ]\n"
            "}\n";

        LayoutSpec layout{};
        std::string err;
        const bool parse_ok = parse_layout_spec_json(layout_json, layout, err);
        t.expect(parse_ok, "LayoutSpec parses topology operator graph fixture");
        if (!parse_ok) {
            t.out << "    parse error: " << err << "\n";
            return false;
        }

        ResolvedObservationWorld world{};
        err.clear();
        const bool world_ok = build_resolved_observation_world(layout, csv.string(), world, err);
        t.expect(world_ok, "topology operator fixture builds observation world");
        if (!world_ok) {
            t.out << "    world error: " << err << "\n";
            return false;
        }

        ResolvedRuntimeInput runtime{};
        err.clear();
        const bool bind_ok = bind_resolved_runtime_target(world, "close", RuntimeTargetSpec{}, runtime, err);
        t.expect(bind_ok, "topology operator fixture binds runtime target");
        if (!bind_ok) {
            t.out << "    bind error: " << err << "\n";
            return false;
        }

        PlanSpec spec{};
        spec.version = 1;
        spec.name = "contract_topology_operator_runtime";
        spec.numeric_nodes = {
            NodeSpec{ "c", "core.close", {}, {} },
            NodeSpec{ "mean", "topology.neighbor_mean", { "c" }, { { "topology", std::string("row.trade.graph") } } },
            NodeSpec{ "min", "topology.neighbor_min", { "c" }, { { "topology", std::string("row.trade.graph") } } },
            NodeSpec{ "max", "topology.neighbor_max", { "c" }, { { "topology", std::string("row.trade.graph") } } },
            NodeSpec{ "weighted", "topology.weighted_sum", { "c" }, { { "topology", std::string("row.trade.graph") } } },
            NodeSpec{ "degree", "topology.degree", {}, { { "topology", std::string("row.trade.graph") } } },
            NodeSpec{ "incoming_degree", "topology.degree", {}, {
                { "topology", std::string("row.trade.graph") },
                { "direction", std::string("incoming") },
            } },
            NodeSpec{ "capped_mean", "topology.neighbor_mean", { "c" }, {
                { "topology", std::string("row.trade.graph") },
                { "max_neighbors", static_cast<std::int64_t>(1) },
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
        t.expect(compile_ok, "topology operator PlanSpec compiles all numeric outputs");
        if (!compile_ok) {
            t.out << "    compile error: " << err << "\n";
            return false;
        }

        CompileResult compiled{};
        err.clear();
        const bool instantiate_ok = instantiate_compiled_plan(plan_template, nullptr, compiled, err);
        t.expect(instantiate_ok, "topology operator compiled template instantiates");
        if (!instantiate_ok) {
            t.out << "    instantiate error: " << err << "\n";
            return false;
        }

        const DataView dv(*runtime.aligned_bundle);
        ProgramEvaluationBuffers buffers{};
        err.clear();
        const bool eval_ok = evaluate_strategy_plan_program_buffers(
            dv,
            runtime.primary_ref,
            compiled.plan,
            buffers,
            err);
        t.expect(eval_ok, "topology operators evaluate through the compiled runtime");
        if (!eval_ok) {
            t.out << "    eval error: " << err << "\n";
            return false;
        }

        const auto output_slot = [&](std::string_view id) -> std::optional<std::size_t> {
            for (const auto& output : numeric_outputs) {
                if (output.node_id == id) {
                    return output.output_slot;
                }
            }
            return std::nullopt;
        };
        const auto output_buffer = [&](std::string_view id) -> const NumericBuffer* {
            const auto slot = output_slot(id);
            if (!slot.has_value() || *slot >= buffers.numeric_slots.size()) {
                return nullptr;
            }
            return &buffers.numeric_slots[*slot];
        };

        const auto* mean = output_buffer("mean");
        const auto* min = output_buffer("min");
        const auto* max = output_buffer("max");
        const auto* weighted = output_buffer("weighted");
        const auto* degree = output_buffer("degree");
        const auto* incoming = output_buffer("incoming_degree");
        const auto* capped = output_buffer("capped_mean");

        t.expect(mean && mean->size() == values.size() &&
                nearly_equal((*mean)[0], 25.0) &&
                nearly_equal((*mean)[1], 30.0) &&
                nearly_equal((*mean)[2], 25.0) &&
                std::isnan((*mean)[3]),
            "topology.neighbor_mean runtime output is row-aligned and provider-backed");
        t.expect(min && min->size() == values.size() &&
                nearly_equal((*min)[0], 20.0) &&
                nearly_equal((*min)[2], 10.0),
            "topology.neighbor_min runtime output is provider-backed");
        t.expect(max && max->size() == values.size() &&
                nearly_equal((*max)[0], 30.0) &&
                nearly_equal((*max)[2], 40.0),
            "topology.neighbor_max runtime output is provider-backed");
        t.expect(weighted && weighted->size() == values.size() &&
                nearly_equal((*weighted)[0], 55.0) &&
                nearly_equal((*weighted)[2], 170.0) &&
                nearly_equal((*weighted)[3], 0.0),
            "topology.weighted_sum runtime output preserves edge weights");
        t.expect(degree && degree->size() == values.size() &&
                nearly_equal((*degree)[0], 2.0) &&
                nearly_equal((*degree)[3], 0.0),
            "topology.degree runtime output counts outgoing neighbors");
        t.expect(incoming && incoming->size() == values.size() &&
                nearly_equal((*incoming)[0], 1.0) &&
                nearly_equal((*incoming)[2], 2.0),
            "topology.degree runtime output honors direction param");
        t.expect(capped && capped->size() == values.size() &&
                nearly_equal((*capped)[0], 20.0) &&
                nearly_equal((*capped)[2], 10.0),
            "topology.neighbor_mean runtime output honors max_neighbors param");

        auto capability = diagnose_plan_capabilities(spec, registry, runtime);
        t.expect(capability.status == PlanCapabilityStatus::Valid,
            "plan capability accepts generic topology operators over graph topology");
        append_plan_capability_snapshot(capability, runtime.run_manifest);
        const auto* usage = find_topology_usage(runtime.run_manifest, "mean", "row.trade.graph");
        t.expect(usage && usage->runtime_resolved,
            "topology operator usage records runtime-resolved topology provenance");
        if (usage) {
            t.expect(usage->topology_bearing == "query_bearing" &&
                    usage->artifact_ref.stable_id == "row.trade.graph.operators.v1",
                "topology operator usage preserves query-bearing artifact provenance");
            t.expect(usage->required_topology_query_kinds ==
                    std::vector<std::string>{ std::string(topology_query_kind_name(TopologyQueryKind::NeighborSet)) },
                "topology operator usage records neighbor-set query requirement");
        }

        const auto* weighted_usage = find_topology_usage(runtime.run_manifest, "weighted", "row.trade.graph");
        t.expect(weighted_usage && weighted_usage->required_topology_query_kinds ==
                std::vector<std::string>{ std::string(topology_query_kind_name(TopologyQueryKind::WeightedNeighborSet)) },
            "topology.weighted_sum usage records weighted-neighbor-set query requirement");

        PlanSpec missing_topology = spec;
        missing_topology.name = "contract_topology_operator_missing_topology";
        missing_topology.numeric_nodes = {
            NodeSpec{ "c", "core.close", {}, {} },
            NodeSpec{ "mean", "topology.neighbor_mean", { "c" }, {} },
        };
        auto missing_report = diagnose_plan_capabilities(missing_topology, registry, runtime);
        const auto* missing_node = find_plan_node(missing_report, "mean");
        t.expect(missing_report.status == PlanCapabilityStatus::Invalid &&
                missing_node &&
                has_plan_reason_detail(
                    missing_node,
                    PlanCapabilityReasonCode::ParameterValidationFailed,
                    "Missing required param 'topology'"),
            "topology operators reject missing explicit topology binding");

        PlanSpec unweighted = spec;
        unweighted.name = "contract_topology_operator_unweighted";
        unweighted.numeric_nodes = {
            NodeSpec{ "c", "core.close", {}, {} },
            NodeSpec{ "weighted", "topology.weighted_sum", { "c" }, { { "topology", std::string("row.unweighted.graph") } } },
        };
        if (world.topology_catalog) {
            TopologySpec unweighted_graph{};
            unweighted_graph.name = "row.unweighted.graph";
            unweighted_graph.bearing = TopologyBearing::QueryBearing;
            unweighted_graph.family = TopologyFamily::RelationalGraph;
            unweighted_graph.domain_scope = TopologyDomainScope::RowDomain;
            unweighted_graph.query_kinds = { TopologyQueryKind::NeighborSet };
            unweighted_graph.node_count = values.size();
            unweighted_graph.static_edges = edges;
            std::string add_err;
            (void)world.topology_catalog->add(std::move(unweighted_graph), &add_err);
        }
        auto unweighted_report = diagnose_plan_capabilities(unweighted, registry, runtime);
        const auto* weighted_node = find_plan_node(unweighted_report, "weighted");
        t.expect(unweighted_report.status == PlanCapabilityStatus::Invalid &&
                weighted_node &&
                has_plan_reason_detail(
                    weighted_node,
                    PlanCapabilityReasonCode::MissingRequiredTopologyQueryKind,
                    "weighted_neighbor_set"),
            "topology.weighted_sum rejects topologies without weighted-neighbor support");

        return true;
    }

} // namespace sugar::contract_suite_internal
