#include "contract_suite_internal.h"
#include "contract_suite_runtime_shared.h"
#include "strategy_plan_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <limits>
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

    bool test_semantic_neighbor_query_runtime_contracts(ContractAsserts& t) {
        t.out << "[semantic_neighbor_query_runtime] semantic embedding query provider, binding, and operator contracts\n";

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
            const std::vector<SemanticEmbeddingVector> embeddings = {
                { 0, { 0.0, 0.0 } },
                { 1, { 1.0, 0.0 } },
                { 2, { 0.0, 2.0 } },
                { 3, { 4.0, 0.0 } },
                { 4, { 0.0, -1.0 } },
            };
            const SemanticEmbeddingNeighborhoodQuery semantic("contract.semantic", 5, embeddings);
            t.expect(semantic.topology_name() == "contract.semantic",
                "semantic query exposes caller-provided topology name");
            t.expect(semantic.row_count() == 5,
                "semantic query exposes row-domain boundedness");
            t.expect(!semantic.has_axis(AxisKind::Time) && !semantic.has_axis(AxisKind::Spatial),
                "semantic query does not pretend to be a temporal or spatial axis");

            NeighborhoodRequest nearest{};
            nearest.topology_name = std::string(semantic.topology_name());
            nearest.anchor.row_index = 0;
            nearest.query_kind = TopologyQueryKind::MetricKNearest;
            nearest.include_anchor = false;
            nearest.set_param("semantic.k_nearest", std::size_t{ 3 });
            const auto nearest_result = semantic.query(nearest);
            t.expect(nearest_result.indices == std::vector<std::size_t>({ 1, 4, 2 }) &&
                    nearest_result.distances.size() == 3 &&
                    nearly_equal(nearest_result.distances[0], 1.0) &&
                    nearly_equal(nearest_result.distances[1], 1.0) &&
                    nearly_equal(nearest_result.distances[2], 2.0),
                "semantic metric k-nearest query sorts by distance and stable node tie-break");

            NeighborhoodRequest radius{};
            radius.topology_name = std::string(semantic.topology_name());
            radius.anchor.row_index = 0;
            radius.query_kind = TopologyQueryKind::MetricRadius;
            radius.include_anchor = false;
            radius.set_param("semantic.radius", 1.0);
            const auto radius_result = semantic.query(radius);
            t.expect(radius_result.indices == std::vector<std::size_t>({ 1, 4 }) &&
                    radius_result.distances == std::vector<double>({ 1.0, 1.0 }),
                "semantic radius query returns bounded metric-distance payloads");

            NeighborhoodRequest weighted{};
            weighted.topology_name = std::string(semantic.topology_name());
            weighted.anchor.row_index = 0;
            weighted.query_kind = TopologyQueryKind::WeightedNeighborSet;
            weighted.include_anchor = false;
            weighted.set_param("semantic.max_neighbors", std::size_t{ 3 });
            const auto weighted_result = semantic.query(weighted);
            t.expect(weighted_result.indices == std::vector<std::size_t>({ 1, 4, 2 }) &&
                    weighted_result.weights.size() == 3 &&
                    nearly_equal(weighted_result.weights[0], 0.5) &&
                    nearly_equal(weighted_result.weights[1], 0.5) &&
                    nearly_equal(weighted_result.weights[2], 1.0 / 3.0),
                "semantic weighted-neighbor query maps Euclidean distance to deterministic similarity weights");

            NeighborhoodRequest include_anchor = nearest;
            include_anchor.include_anchor = true;
            include_anchor.set_param("semantic.k_nearest", std::size_t{ 1 });
            const auto anchor_result = semantic.query(include_anchor);
            t.expect(anchor_result.indices == std::vector<std::size_t>({ 0 }) &&
                    anchor_result.distances == std::vector<double>({ 0.0 }),
                "semantic query can include anchor with zero distance");

            NeighborhoodRequest bad_cap = weighted;
            bad_cap.set_param("semantic.max_neighbors", static_cast<std::int64_t>(-1));
            expect_throws_with(
                [&]() { (void)semantic.query(bad_cap); },
                "semantic.max_neighbors must be >= 0",
                "semantic query rejects negative max-neighbor caps");

            NeighborhoodRequest bad_kind = nearest;
            bad_kind.query_kind = TopologyQueryKind::TransitionSet;
            expect_throws_with(
                [&]() { (void)semantic.query(bad_kind); },
                "only semantic nearest-neighbor queries are supported",
                "semantic query rejects unsupported query kinds explicitly");
        }

        expect_throws_with(
            [&]() {
                (void)SemanticEmbeddingNeighborhoodQuery(
                    "contract.semantic.duplicate",
                    2,
                    std::vector<SemanticEmbeddingVector>{
                        { 0, { 0.0 } },
                        { 0, { 1.0 } },
                    });
            },
            "duplicate embedding node",
            "semantic query constructor rejects duplicate embedding nodes");

        expect_throws_with(
            [&]() {
                (void)SemanticEmbeddingNeighborhoodQuery(
                    "contract.semantic.dimension",
                    2,
                    std::vector<SemanticEmbeddingVector>{
                        { 0, { 0.0, 1.0 } },
                        { 1, { 1.0 } },
                    });
            },
            "embedding dimensions must match",
            "semantic query constructor rejects inconsistent embedding dimensions");

        expect_throws_with(
            [&]() {
                (void)SemanticEmbeddingNeighborhoodQuery(
                    "contract.semantic.non_finite",
                    1,
                    std::vector<SemanticEmbeddingVector>{
                        { 0, { std::numeric_limits<double>::quiet_NaN() } },
                    });
            },
            "embedding values must be finite",
            "semantic query constructor rejects non-finite embedding values");

        {
            const SemanticEmbeddingNeighborhoodQuery sparse(
                "contract.semantic.sparse",
                3,
                std::vector<SemanticEmbeddingVector>{
                    { 0, { 0.0 } },
                    { 2, { 1.0 } },
                });
            NeighborhoodRequest missing_anchor{};
            missing_anchor.topology_name = std::string(sparse.topology_name());
            missing_anchor.anchor.row_index = 1;
            missing_anchor.query_kind = TopologyQueryKind::NeighborSet;
            expect_throws_with(
                [&]() { (void)sparse.query(missing_anchor); },
                "anchor embedding is not available",
                "semantic query rejects anchors without embedding vectors");
        }

        const auto dir = contract_tmp_case_dir("semantic_neighbor_query_runtime");
        std::filesystem::create_directories(dir);
        const auto csv = dir / "semantic_neighbor_query_runtime.csv";
        const std::string csv_body =
            "date,close\n"
            "20240101,10\n"
            "20240102,20\n"
            "20240103,30\n"
            "20240104,40\n";
        if (!write_text_file(csv, csv_body)) {
            return t.expect(false, "semantic runtime fixture csv is written");
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
            "      \"name\": \"row.embedding.semantic\",\n"
            "      \"bearing\": \"query_bearing\",\n"
            "      \"family\": \"semantic_graph\",\n"
            "      \"domain_scope\": \"row_domain\",\n"
            "      \"query_kinds\": [\"neighbor_set\", \"weighted_neighbor_set\", \"metric_k_nearest\", \"metric_radius\"],\n"
            "      \"node_count\": 4,\n"
            "      \"artifact_ref\": {\n"
            "        \"stable_id\": \"row.embedding.semantic.v1\",\n"
            "        \"locator\": \"inline://layout/topologies/row.embedding.semantic\",\n"
            "        \"format\": \"inline_semantic_embeddings\",\n"
            "        \"digest\": \"sha256:row-embedding-semantic-v1\",\n"
            "        \"producer\": \"contract_fixture\"\n"
            "      },\n"
            "      \"embeddings\": [\n"
            "        {\"node\": 0, \"vector\": [0.0, 0.0]},\n"
            "        {\"node\": 1, \"vector\": [1.0, 0.0]},\n"
            "        {\"node\": 2, \"vector\": [0.0, 2.0]},\n"
            "        {\"node\": 3, \"vector\": [4.0, 0.0]}\n"
            "      ]\n"
            "    }\n"
            "  ]\n"
            "}\n";

        LayoutSpec layout{};
        std::string err;
        const bool parse_ok = parse_layout_spec_json(layout_json, layout, err);
        t.expect(parse_ok, "LayoutSpec parses inline semantic embedding topology");
        if (!parse_ok) {
            t.out << "    parse error: " << err << "\n";
            return false;
        }
        t.expect(!layout.topologies.empty() &&
                layout.topologies.front().family == TopologyFamily::SemanticGraph &&
                layout.topologies.front().semantic_embeddings.size() == 4,
            "LayoutSpec preserves semantic family and embedding payload");

        LayoutSpec bad_layout{};
        err.clear();
        const bool bad_parse_ok = parse_layout_spec_json(
            "{ \"ingest\": {"
            "    \"header_policy\": \"RequiredHeaders\","
            "    \"time_col\": \"date\","
            "    \"time_kind\": \"YyyyMmDd\","
            "    \"value_cols\": [\"close\"],"
            "    \"primary_value_col\": \"close\""
            "  },"
            "  \"topologies\": [ { \"name\": \"bad.semantic\", \"embeddings\": {} } ] }",
            bad_layout,
            err);
        t.expect(!bad_parse_ok && err.find("layout.topologies[].embeddings must be array") != std::string::npos,
            "LayoutSpec rejects malformed semantic embedding payloads explicitly");

        ResolvedObservationWorld world{};
        err.clear();
        const bool world_ok = build_resolved_observation_world(layout, csv.string(), world, err);
        t.expect(world_ok, "observation world mounts declared semantic topology");
        if (!world_ok) {
            t.out << "    world error: " << err << "\n";
            return false;
        }
        t.expect(world.topology_catalog && world.topology_catalog->has("row.embedding.semantic"),
            "topology catalog exposes declared semantic graph by name");

        const auto manifest_it = std::find_if(
            world.run_manifest.topologies.begin(),
            world.run_manifest.topologies.end(),
            [](const TopologySpec& spec) { return spec.name == "row.embedding.semantic"; });
        t.expect(manifest_it != world.run_manifest.topologies.end() &&
                manifest_it->family == TopologyFamily::SemanticGraph,
            "run manifest snapshots semantic graph topology family");
        if (manifest_it != world.run_manifest.topologies.end()) {
            t.expect(manifest_it->artifact_ref &&
                    manifest_it->artifact_ref->stable_id == "row.embedding.semantic.v1",
                "run manifest preserves semantic graph artifact stable id");
            t.expect(manifest_it->semantic_embeddings.size() == 4,
                "run manifest keeps semantic embedding payload available to runtime adapters");
        }

        const DataView dv(*world.aligned_bundle);
        RuntimeLocalitySubject observation_subject{};
        observation_subject.kind = LocalitySubjectKind::ObservationDomain;
        auto query = dv.resolve_topology(observation_subject, "row.embedding.semantic");
        t.expect(query != nullptr,
            "DataView resolves query-bearing semantic embedding topology");
        if (query) {
            NeighborhoodRequest nearest{};
            nearest.topology_name = std::string(query->topology_name());
            nearest.anchor.row_index = 0;
            nearest.query_kind = TopologyQueryKind::MetricKNearest;
            nearest.include_anchor = false;
            nearest.set_param("semantic.k_nearest", std::size_t{ 2 });
            const auto nearest_result = query->query(nearest);
            t.expect(nearest_result.indices == std::vector<std::size_t>({ 1, 2 }) &&
                    nearest_result.distances.size() == 2 &&
                    nearly_equal(nearest_result.distances[0], 1.0) &&
                    nearly_equal(nearest_result.distances[1], 2.0),
                "resolved semantic topology returns deterministic metric nearest neighbors");

            NeighborhoodRequest weighted{};
            weighted.topology_name = std::string(query->topology_name());
            weighted.anchor.row_index = 0;
            weighted.query_kind = TopologyQueryKind::WeightedNeighborSet;
            weighted.include_anchor = false;
            weighted.set_param("graph.max_neighbors", std::size_t{ 2 });
            const auto weighted_result = query->query(weighted);
            t.expect(weighted_result.indices == std::vector<std::size_t>({ 1, 2 }) &&
                    weighted_result.weights.size() == 2 &&
                    nearly_equal(weighted_result.weights[0], 0.5) &&
                    nearly_equal(weighted_result.weights[1], 1.0 / 3.0),
                "resolved semantic topology honors generic max-neighbor caps for weighted queries");
        }

        if (world.topology_catalog) {
            TopologySpec invalid_semantic{};
            invalid_semantic.name = "row.embedding.invalid_semantic";
            invalid_semantic.bearing = TopologyBearing::QueryBearing;
            invalid_semantic.family = TopologyFamily::SemanticGraph;
            invalid_semantic.domain_scope = TopologyDomainScope::RowDomain;
            invalid_semantic.query_kinds = { TopologyQueryKind::NeighborSet };
            invalid_semantic.node_count = 2;
            invalid_semantic.semantic_embeddings = {
                { 0, { 0.0, 0.0 } },
                { 1, { 1.0 } },
            };
            std::string add_err;
            const bool add_invalid_ok = world.topology_catalog->add(std::move(invalid_semantic), &add_err);
            t.expect(add_invalid_ok,
                "contract can mount invalid semantic fixture for runtime diagnostic coverage");
            if (add_invalid_ok) {
                expect_throws_with(
                    [&]() { (void)dv.resolve_topology(observation_subject, "row.embedding.invalid_semantic"); },
                    "semantic graph topology",
                    "DataView semantic materialization failures include topology context");
                expect_throws_with(
                    [&]() { (void)dv.resolve_topology(observation_subject, "row.embedding.invalid_semantic"); },
                    "embedding dimensions must match",
                    "DataView semantic materialization failures preserve provider diagnostic detail");
            }
        }

        ResolvedRuntimeInput runtime{};
        err.clear();
        const bool bind_ok = bind_resolved_runtime_target(world, "close", RuntimeTargetSpec{}, runtime, err);
        t.expect(bind_ok, "semantic runtime target binding succeeds");
        if (!bind_ok) {
            t.out << "    bind error: " << err << "\n";
            return false;
        }

        PlanSpec spec{};
        spec.version = 1;
        spec.name = "contract_semantic_neighbor_query_runtime";
        spec.numeric_nodes = {
            NodeSpec{ "c", "core.close", {}, {} },
            NodeSpec{ "mean", "topology.neighbor_mean", { "c" }, {
                { "topology", std::string("row.embedding.semantic") },
                { "max_neighbors", static_cast<std::int64_t>(2) },
            } },
            NodeSpec{ "weighted", "topology.weighted_sum", { "c" }, {
                { "topology", std::string("row.embedding.semantic") },
                { "max_neighbors", static_cast<std::int64_t>(2) },
            } },
            NodeSpec{ "degree", "topology.degree", {}, {
                { "topology", std::string("row.embedding.semantic") },
                { "max_neighbors", static_cast<std::int64_t>(2) },
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
        t.expect(compile_ok, "semantic topology operator PlanSpec compiles");
        if (!compile_ok) {
            t.out << "    compile error: " << err << "\n";
            return false;
        }

        CompileResult compiled{};
        err.clear();
        const bool instantiate_ok = instantiate_compiled_plan(plan_template, nullptr, compiled, err);
        t.expect(instantiate_ok, "semantic topology operator PlanSpec instantiates");
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
        t.expect(eval_ok, "topology operators evaluate over semantic embedding provider");
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

        const auto* mean = output_buffer("mean");
        const auto* weighted = output_buffer("weighted");
        const auto* degree = output_buffer("degree");
        t.expect(mean && mean->size() == 4 &&
                nearly_equal((*mean)[0], 25.0) &&
                nearly_equal((*mean)[1], 20.0) &&
                nearly_equal((*mean)[3], 15.0),
            "topology.neighbor_mean consumes semantic nearest-neighbor selections");
        t.expect(weighted && weighted->size() == 4 &&
                nearly_equal((*weighted)[0], 20.0) &&
                nearly_equal((*weighted)[3], 7.0),
            "topology.weighted_sum consumes semantic distance-derived weights");
        t.expect(degree && degree->size() == 4 &&
                nearly_equal((*degree)[0], 2.0) &&
                nearly_equal((*degree)[1], 2.0) &&
                nearly_equal((*degree)[3], 2.0),
            "topology.degree counts capped semantic nearest neighbors");

        auto capability = diagnose_plan_capabilities(spec, registry, runtime);
        t.expect(capability.status == PlanCapabilityStatus::Valid,
            "plan capability accepts generic topology operators over semantic graph topology");
        append_plan_capability_snapshot(capability, runtime.run_manifest);
        const auto* usage = find_topology_usage(runtime.run_manifest, "mean", "row.embedding.semantic");
        t.expect(usage && usage->runtime_resolved,
            "topology usage snapshot records runtime-resolved semantic topology provenance");
        if (usage) {
            t.expect(usage->topology_origin == "layout_declared" &&
                    usage->topology_bearing == "query_bearing" &&
                    usage->artifact_ref.stable_id == "row.embedding.semantic.v1",
                "topology usage snapshot preserves semantic origin, bearing, and artifact provenance");
            t.expect(usage->required_topology_query_kinds ==
                    std::vector<std::string>{ std::string(topology_query_kind_name(TopologyQueryKind::NeighborSet)) },
                "semantic neighbor-mean usage records neighbor-set query requirement");
        }
        const auto* weighted_usage = find_topology_usage(runtime.run_manifest, "weighted", "row.embedding.semantic");
        t.expect(weighted_usage && weighted_usage->required_topology_query_kinds ==
                std::vector<std::string>{ std::string(topology_query_kind_name(TopologyQueryKind::WeightedNeighborSet)) },
            "semantic weighted-sum usage records weighted-neighbor-set query requirement");

        return true;
    }

} // namespace sugar::contract_suite_internal
