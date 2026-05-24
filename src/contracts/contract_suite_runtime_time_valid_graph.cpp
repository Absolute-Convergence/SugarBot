#include "contract_suite_internal.h"
#include "contract_suite_runtime_shared.h"
#include "strategy_plan_internal.h"

#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/plan_compiler.h"
#include "engine/plan_compiler_internal.h"
#include "engine/program_evaluator.h"
#include "engine/program_evaluator_internal.h"
#include "ingest/run_pipeline.h"
#include "op_kernels_topology.h"
#include "op_registry_internal.h"
#include "ops/op_registry.h"
#include "sweep/plan_capability.h"

namespace sugar::contract_suite_internal {

    namespace {

        StaticGraphEdge graph_edge(
            std::size_t source,
            std::size_t target,
            double weight,
            std::optional<TimeKey> valid_from = std::nullopt,
            std::optional<TimeKey> valid_to = std::nullopt,
            std::string version = {}) {
            StaticGraphEdge edge{};
            edge.source = source;
            edge.target = target;
            edge.weight = weight;
            edge.valid_from = valid_from;
            edge.valid_to = valid_to;
            edge.version = std::move(version);
            return edge;
        }

        const NumericBuffer* find_output_buffer(
            std::string_view node_id,
            const std::vector<NamedNumericOutput>& outputs,
            const ProgramEvaluationBuffers& buffers) {
            for (const auto& output : outputs) {
                if (output.node_id == node_id && output.output_slot < buffers.numeric_slots.size()) {
                    return &buffers.numeric_slots[output.output_slot];
                }
            }
            return nullptr;
        }

    } // namespace

    bool test_time_valid_graph_query_runtime_contracts(ContractAsserts& t) {
        t.out << "[time_valid_graph_query_runtime] time-valid and versioned graph query contracts\n";

        auto expect_throws_with = [&](auto&& fn, std::string_view needle, const std::string& label) {
            try {
                fn();
                return t.expect(false, label + " throws");
            }
            catch (const std::exception& ex) {
                return t.expect(std::string(ex.what()).find(needle) != std::string::npos, label);
            }
        };

        const std::vector<TimeKey> time_axis = {
            20240101,
            20240102,
            20240103,
            20240104,
        };
        const std::vector<StaticGraphEdge> direct_edges = {
            graph_edge(0, 1, 1.0, TimeKey{ 20240101 }, TimeKey{ 20240103 }),
            graph_edge(0, 2, 1.0, TimeKey{ 20240103 }, std::nullopt),
            graph_edge(0, 3, 1.0, std::nullopt, std::nullopt, "alt"),
        };
        const StaticGraphNeighborhoodQuery direct_query(
            "contract.graph.time_valid",
            4,
            direct_edges,
            true,
            true,
            time_axis);

        auto base_request = make_neighborhood_request(
            direct_query.topology_name(),
            0,
            GraphNeighborhoodRequest::neighbors(GraphNeighborhoodDirection::Outgoing));
        const auto row_time_result = direct_query.query(base_request);
        t.expect(row_time_result.indices == std::vector<std::size_t>{ 1 },
            "time-valid graph query uses the bound anchor row time by default");

        NeighborhoodRequest explicit_time = base_request;
        explicit_time.set_param("graph.time", static_cast<std::int64_t>(20240103));
        const auto later_time_result = direct_query.query(explicit_time);
        t.expect(later_time_result.indices == std::vector<std::size_t>{ 2 },
            "time-valid graph query treats valid_to as exclusive and valid_from as inclusive");

        NeighborhoodRequest versioned = base_request;
        versioned.set_param("graph.version", std::string("alt"));
        const auto versioned_result = direct_query.query(versioned);
        t.expect(versioned_result.indices == std::vector<std::size_t>({ 1, 3 }),
            "versioned graph query includes unversioned base edges plus the requested version overlay");

        const StaticGraphNeighborhoodQuery no_time_axis(
            "contract.graph.no_time_axis",
            4,
            direct_edges,
            true,
            true);
        auto no_time_request = base_request;
        no_time_request.topology_name = std::string(no_time_axis.topology_name());
        expect_throws_with(
            [&]() { (void)no_time_axis.query(no_time_request); },
            "time-valid edges require",
            "time-valid graph query rejects implicit time filtering without a time axis");

        expect_throws_with(
            [&]() {
                (void)StaticGraphNeighborhoodQuery(
                    "contract.graph.bad_range",
                    2,
                    std::vector<StaticGraphEdge>{
                        graph_edge(0, 1, 1.0, TimeKey{ 20240103 }, TimeKey{ 20240103 })
                    },
                    true,
                    true,
                    std::vector<TimeKey>{ 20240103, 20240104 });
            },
            "valid_to must be greater than valid_from",
            "time-valid graph query rejects empty validity ranges");

        LayoutSpec parsed_layout{};
        std::string err;
        const bool parse_ok = parse_layout_spec_json(
            "{\n"
            "  \"ingest\": {\n"
            "    \"header_policy\": \"RequiredHeaders\",\n"
            "    \"time_col\": \"date\",\n"
            "    \"time_kind\": \"YyyyMmDd\",\n"
            "    \"value_cols\": [\"close\"],\n"
            "    \"primary_value_col\": \"close\"\n"
            "  },\n"
            "  \"topologies\": [\n"
            "    {\n"
            "      \"name\": \"row.time.graph\",\n"
            "      \"bearing\": \"query_bearing\",\n"
            "      \"family\": \"relational_graph\",\n"
            "      \"domain_scope\": \"row_domain\",\n"
            "      \"query_kinds\": [\"neighbor_set\", \"weighted_neighbor_set\"],\n"
            "      \"directed\": true,\n"
            "      \"weighted\": true,\n"
            "      \"node_count\": 4,\n"
            "      \"edges\": [\n"
            "        {\"source\": 0, \"target\": 1, \"weight\": 1.5, \"valid_from\": 20240101, \"valid_to\": 20240103, \"version\": \"alt\"}\n"
            "      ]\n"
            "    }\n"
            "  ]\n"
            "}\n",
            parsed_layout,
            err);
        t.expect(parse_ok, "LayoutSpec parses time-valid and versioned graph edge metadata");
        if (parse_ok && !parsed_layout.topologies.empty() && !parsed_layout.topologies[0].static_edges.empty()) {
            const auto& edge = parsed_layout.topologies[0].static_edges[0];
            t.expect(edge.valid_from && *edge.valid_from == 20240101 &&
                    edge.valid_to && *edge.valid_to == 20240103 &&
                    edge.version == "alt",
                "LayoutSpec preserves parsed edge validity and version fields");
        }
        else if (!parse_ok) {
            t.out << "    parse error: " << err << "\n";
        }

        LayoutSpec bad_layout{};
        err.clear();
        const bool bad_parse_ok = parse_layout_spec_json(
            "{\n"
            "  \"ingest\": {\"header_policy\":\"RequiredHeaders\", \"time_col\":\"date\", \"time_kind\":\"YyyyMmDd\", \"value_cols\":[\"close\"], \"primary_value_col\":\"close\"},\n"
            "  \"topologies\": [{\"name\":\"bad.graph\", \"bearing\":\"query_bearing\", \"family\":\"relational_graph\", \"query_kinds\":[\"neighbor_set\"], \"edges\":[{\"source\":0, \"target\":1, \"valid_from\":20240103, \"valid_to\":20240103}]}]\n"
            "}\n",
            bad_layout,
            err);
        t.expect(!bad_parse_ok && err.find("valid_to must be greater than valid_from") != std::string::npos,
            "LayoutSpec rejects empty time-valid graph edge ranges");

        const auto dir = contract_tmp_case_dir("time_valid_graph_query_runtime");
        std::filesystem::create_directories(dir);
        const auto csv = dir / "time_valid_graph_query_runtime.csv";
        const std::string csv_body =
            "date,close\n"
            "20240101,10\n"
            "20240102,20\n"
            "20240103,30\n"
            "20240104,40\n";
        if (!write_text_file(csv, csv_body)) {
            return t.expect(false, "time-valid graph runtime fixture csv is written");
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
            "      \"name\": \"row.time.graph\",\n"
            "      \"bearing\": \"query_bearing\",\n"
            "      \"family\": \"relational_graph\",\n"
            "      \"domain_scope\": \"row_domain\",\n"
            "      \"query_kinds\": [\"neighbor_set\", \"weighted_neighbor_set\"],\n"
            "      \"directed\": true,\n"
            "      \"weighted\": true,\n"
            "      \"node_count\": 4,\n"
            "      \"edges\": [\n"
            "        {\"source\": 0, \"target\": 1, \"weight\": 1.0, \"valid_from\": 20240101, \"valid_to\": 20240103},\n"
            "        {\"source\": 1, \"target\": 3, \"weight\": 1.0},\n"
            "        {\"source\": 1, \"target\": 2, \"weight\": 2.0, \"version\": \"alt\"},\n"
            "        {\"source\": 2, \"target\": 3, \"weight\": 1.0, \"valid_from\": 20240103},\n"
            "        {\"source\": 3, \"target\": 0, \"weight\": 1.0, \"valid_to\": 20240103}\n"
            "      ]\n"
            "    }\n"
            "  ]\n"
            "}\n";

        LayoutSpec layout{};
        err.clear();
        const bool fixture_parse_ok = parse_layout_spec_json(layout_json, layout, err);
        t.expect(fixture_parse_ok, "time-valid graph runtime fixture layout parses");
        if (!fixture_parse_ok) {
            t.out << "    parse error: " << err << "\n";
            return false;
        }

        ResolvedObservationWorld world{};
        err.clear();
        const bool world_ok = build_resolved_observation_world(layout, csv.string(), world, err);
        t.expect(world_ok, "time-valid graph runtime fixture builds observation world");
        if (!world_ok) {
            t.out << "    world error: " << err << "\n";
            return false;
        }

        const DataView dv(*world.aligned_bundle);
        RuntimeLocalitySubject subject{};
        subject.kind = LocalitySubjectKind::ObservationDomain;
        auto query = dv.resolve_topology(subject, "row.time.graph");
        auto row_two_request = make_neighborhood_request(
            query->topology_name(),
            2,
            GraphNeighborhoodRequest::neighbors(GraphNeighborhoodDirection::Outgoing));
        const auto row_two_result = query->query(row_two_request);
        t.expect(row_two_result.indices == std::vector<std::size_t>{ 3 },
            "DataView-bound graph provider filters time-valid edges by each anchor row time");

        PlanSpec spec{};
        spec.version = 1;
        spec.name = "contract_time_valid_graph_query_runtime";
        spec.numeric_nodes = {
            NodeSpec{ "c", "core.close", {}, {} },
            NodeSpec{ "mean_default", "topology.neighbor_mean", { "c" }, {
                { "topology", std::string("row.time.graph") },
            } },
            NodeSpec{ "mean_alt", "topology.neighbor_mean", { "c" }, {
                { "topology", std::string("row.time.graph") },
                { "version", std::string("alt") },
            } },
            NodeSpec{ "weighted_alt", "topology.weighted_sum", { "c" }, {
                { "topology", std::string("row.time.graph") },
                { "version", std::string("alt") },
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
        t.expect(compile_ok, "time-valid graph topology operator PlanSpec compiles");
        if (!compile_ok) {
            t.out << "    compile error: " << err << "\n";
            return false;
        }

        CompileResult compiled{};
        err.clear();
        const bool instantiate_ok = instantiate_compiled_plan(plan_template, nullptr, compiled, err);
        t.expect(instantiate_ok, "time-valid graph topology operator PlanSpec instantiates");
        if (!instantiate_ok) {
            t.out << "    instantiate error: " << err << "\n";
            return false;
        }

        ResolvedRuntimeInput runtime{};
        err.clear();
        const bool bind_ok = bind_resolved_runtime_target(world, "close", RuntimeTargetSpec{}, runtime, err);
        t.expect(bind_ok, "time-valid graph runtime target binds");
        if (!bind_ok) {
            t.out << "    bind error: " << err << "\n";
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
        t.expect(eval_ok, "topology operators evaluate over time-valid and versioned graph provider");
        if (!eval_ok) {
            t.out << "    eval error: " << err << "\n";
            return false;
        }

        const auto* mean_default = find_output_buffer("mean_default", numeric_outputs, buffers);
        const auto* mean_alt = find_output_buffer("mean_alt", numeric_outputs, buffers);
        const auto* weighted_alt = find_output_buffer("weighted_alt", numeric_outputs, buffers);
        t.expect(mean_default && mean_default->size() == 4 &&
                nearly_equal((*mean_default)[0], 20.0) &&
                nearly_equal((*mean_default)[1], 40.0) &&
                nearly_equal((*mean_default)[2], 40.0) &&
                std::isnan((*mean_default)[3]),
            "topology.neighbor_mean uses anchor-row time filtering and skips versioned overlays by default");
        t.expect(mean_alt && mean_alt->size() == 4 &&
                nearly_equal((*mean_alt)[0], 20.0) &&
                nearly_equal((*mean_alt)[1], 35.0) &&
                nearly_equal((*mean_alt)[2], 40.0) &&
                std::isnan((*mean_alt)[3]),
            "topology.neighbor_mean version param includes matching graph-version overlays");
        t.expect(weighted_alt && weighted_alt->size() == 4 &&
                nearly_equal((*weighted_alt)[0], 20.0) &&
                nearly_equal((*weighted_alt)[1], 100.0) &&
                nearly_equal((*weighted_alt)[2], 40.0) &&
                nearly_equal((*weighted_alt)[3], 0.0),
            "topology.weighted_sum version param preserves weighted version-overlay semantics");

        auto capability = diagnose_plan_capabilities(spec, registry, runtime);
        t.expect(capability.status == PlanCapabilityStatus::Valid,
            "plan capability accepts time-valid and versioned graph topology operators");
        append_plan_capability_snapshot(capability, runtime.run_manifest);
        const auto* usage = find_topology_usage(runtime.run_manifest, "mean_alt", "row.time.graph");
        t.expect(usage && usage->runtime_resolved,
            "topology usage snapshot records runtime-resolved time-valid graph operator usage");

        TopologySpec no_time_topology{};
        no_time_topology.name = "row.time.graph";
        no_time_topology.bearing = TopologyBearing::QueryBearing;
        no_time_topology.family = TopologyFamily::RelationalGraph;
        no_time_topology.domain_scope = TopologyDomainScope::RowDomain;
        no_time_topology.query_kinds = { TopologyQueryKind::NeighborSet };
        no_time_topology.static_edges = {
            graph_edge(0, 1, 1.0, TimeKey{ 20240101 }, std::nullopt),
        };
        ObservationWorldFacts no_time_facts{};
        no_time_facts.series_count = 1;
        no_time_facts.row_count = 2;
        no_time_facts.has_time_axis = false;
        no_time_facts.available_topologies = { no_time_topology };
        PlanSpec no_time_plan{};
        no_time_plan.numeric_nodes = {
            NodeSpec{ "c", "core.close", {}, {} },
            NodeSpec{ "mean", "topology.neighbor_mean", { "c" }, {
                { "topology", std::string("row.time.graph") },
            } },
        };
        const auto no_time_report = diagnose_plan_capabilities(no_time_plan, registry, no_time_facts);
        const auto* no_time_node = find_plan_node(no_time_report, "mean");
        t.expect(no_time_report.status == PlanCapabilityStatus::Invalid &&
                no_time_node &&
                has_plan_reason(no_time_node, PlanCapabilityReasonCode::TopologyResolutionFailed),
            "plan capability rejects time-valid topology edges when the observation world has no time axis");
        t.expect(no_time_node &&
                has_plan_reason_detail(no_time_node,
                    PlanCapabilityReasonCode::TopologyResolutionFailed,
                    "time-valid edges"),
            "time-valid topology capability failure names the topology time-axis requirement");

        return true;
    }

} // namespace sugar::contract_suite_internal
