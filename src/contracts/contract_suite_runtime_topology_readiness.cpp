#include "contract_suite_internal.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "ingest/neighborhood_query.h"

namespace sugar::contract_suite_internal {

    namespace {
        bool result_indices_within_bound(const NeighborhoodResult& result) {
            const auto indices = result.materialize_indices();
            return std::all_of(indices.begin(), indices.end(),
                [&](std::size_t index) {
                    return index < result.row_count_bound;
                });
        }

        bool result_payloads_align(const NeighborhoodResult& result) {
            const std::size_t size = result.size();
            return (result.weights.empty() || result.weights.size() == size) &&
                (result.distances.empty() || result.distances.size() == size);
        }

        bool has_param_value(
            const NeighborhoodResult& result,
            std::string_view name,
            const std::string& expected) {
            const auto* value = result.find_param_as<std::string>(name);
            return value && *value == expected;
        }

        template <typename T>
        bool has_param_value(
            const NeighborhoodResult& result,
            std::string_view name,
            T expected) {
            const auto* value = result.find_param_as<T>(name);
            return value && *value == expected;
        }

        bool result_preserves_request_metadata(
            const NeighborhoodResult& result,
            const NeighborhoodRequest& request) {
            return result.query_kind == request.query_kind &&
                result.include_anchor == request.include_anchor &&
                result.query_params == request.params;
        }
    }

    bool test_topology_readiness_runtime_contracts(ContractAsserts& t) {
        t.out << "[topology_readiness_runtime] final topology-query readiness contracts\n";

        {
            const std::vector<TimeKey> time = { 10, 20, 30, 40 };
            const TimeNeighborhoodQuery temporal(time);
            const auto request = make_neighborhood_request(
                temporal.topology_name(),
                3,
                TimeNeighborhoodRequest::last_n_samples(2, false));
            const auto result = temporal.query(request);

            t.expect(result_preserves_request_metadata(result, request),
                "temporal result preserves typed query metadata");
            t.expect(result.contiguous && result.materialize_indices() == std::vector<std::size_t>({ 1, 2 }),
                "temporal result preserves stable typed neighbor identity");
            t.expect(result.row_count_bound == time.size() &&
                    result.has_result_cap &&
                    result.result_cap == 2 &&
                    result_indices_within_bound(result),
                "temporal result exposes explicit boundedness");
            t.expect(has_param_value(result, "time.last_n", std::size_t{ 2 }),
                "temporal result preserves typed time bound field");
        }

        {
            const std::vector<double> x = { 0.0, 1.0, 0.0, 3.0 };
            const std::vector<double> y = { 0.0, 0.0, 2.0, 0.0 };
            const SpatialNeighborhoodQuery2D spatial(x, y);
            const auto request = make_neighborhood_request(
                spatial.topology_name(),
                0,
                SpatialNeighborhoodRequest::nearest_k(2, false));
            const auto result = spatial.query(request);

            t.expect(result_preserves_request_metadata(result, request),
                "spatial result preserves typed query metadata");
            t.expect(result.indices == std::vector<std::size_t>({ 1, 2 }) &&
                    result.distances.size() == 2 &&
                    result_payloads_align(result),
                "spatial result aligns stable neighbor identity with distance payloads");
            t.expect(result.row_count_bound == x.size() &&
                    result.has_result_cap &&
                    result.result_cap == 2 &&
                    result_indices_within_bound(result),
                "spatial result exposes explicit boundedness");
            t.expect(has_param_value(result, "spatial.k_nearest", std::size_t{ 2 }),
                "spatial result preserves typed nearest-neighbor cap field");
        }

        {
            const std::vector<TimeKey> time = { 20240101, 20240102, 20240103, 20240104 };
            const std::vector<StaticGraphEdge> edges = {
                StaticGraphEdge{ 0, 1, 1.0 },
                StaticGraphEdge{ 0, 2, 2.0, std::nullopt, std::nullopt, "stress" },
                StaticGraphEdge{ 3, 0, 4.0, std::nullopt, std::nullopt, "stress" },
                StaticGraphEdge{ 0, 3, 3.0, TimeKey{ 20240103 }, std::nullopt },
            };
            const StaticGraphNeighborhoodQuery graph("contract.f8.graph", 4, edges, true, true, time);
            auto request = make_neighborhood_request(
                graph.topology_name(),
                0,
                GraphNeighborhoodRequest::weighted_neighbors(GraphNeighborhoodDirection::Outgoing, false, 2));
            request.set_param("graph.version", std::string("stress"));
            const auto result = graph.query(request);
            const auto repeated = graph.query(request);

            t.expect(result_preserves_request_metadata(result, request),
                "graph result preserves typed query metadata");
            t.expect(result.indices == std::vector<std::size_t>({ 1, 2 }) &&
                    result.weights == std::vector<double>({ 1.0, 2.0 }) &&
                    result_payloads_align(result),
                "graph result aligns stable neighbor identity with weight payloads");
            t.expect(result.indices == repeated.indices &&
                    result.weights == repeated.weights &&
                    result.query_params == repeated.query_params,
                "graph result ordering and metadata are deterministic across repeated queries");
            t.expect(result.row_count_bound == 4 &&
                    result.has_result_cap &&
                    result.result_cap == 2 &&
                    result_indices_within_bound(result),
                "graph result exposes explicit boundedness");
            t.expect(has_param_value(result, "graph.direction", static_cast<std::int64_t>(GraphNeighborhoodDirection::Outgoing)) &&
                    has_param_value(result, "graph.version", std::string("stress")),
                "graph result preserves typed direction and version fields");

            auto time_request = make_neighborhood_request(
                graph.topology_name(),
                0,
                GraphNeighborhoodRequest::neighbors(GraphNeighborhoodDirection::Outgoing));
            time_request.set_param("graph.time", static_cast<std::int64_t>(20240103));
            const auto time_result = graph.query(time_request);
            t.expect(time_result.indices == std::vector<std::size_t>({ 1, 3 }) &&
                    has_param_value(time_result, "graph.time", static_cast<std::int64_t>(20240103)),
                "time-valid graph result preserves typed time filter field");
        }

        {
            const std::vector<StaticGraphEdge> edges = {
                StaticGraphEdge{ 0, 1, 1.0 },
                StaticGraphEdge{ 0, 2, 1.0 },
                StaticGraphEdge{ 1, 3, 1.0 },
                StaticGraphEdge{ 1, 4, 1.0 },
            };
            const HierarchyNeighborhoodQuery hierarchy("contract.f8.hierarchy", 5, edges);
            auto request = make_neighborhood_request(
                hierarchy.topology_name(),
                0,
                HierarchyNeighborhoodRequest::children(false, 2, 3));
            const auto result = hierarchy.query(request);

            t.expect(result_preserves_request_metadata(result, request),
                "hierarchy result preserves typed query metadata");
            t.expect(result.indices == std::vector<std::size_t>({ 1, 2, 3 }),
                "hierarchy result preserves deterministic traversal identity");
            t.expect(result.row_count_bound == 5 &&
                    result.has_result_cap &&
                    result.result_cap == 3 &&
                    result_indices_within_bound(result),
                "hierarchy result exposes explicit boundedness");
            t.expect(has_param_value(result, "hierarchy.traversal", static_cast<std::int64_t>(HierarchyTraversal::Children)) &&
                    has_param_value(result, "hierarchy.max_depth", std::size_t{ 2 }),
                "hierarchy result preserves typed traversal and depth fields");
        }

        {
            const std::vector<SemanticEmbeddingVector> embeddings = {
                { 0, { 0.0, 0.0 } },
                { 1, { 1.0, 0.0 } },
                { 2, { 0.0, 2.0 } },
                { 3, { 4.0, 0.0 } },
            };
            const SemanticEmbeddingNeighborhoodQuery semantic("contract.f8.semantic", 4, embeddings);

            NeighborhoodRequest nearest{};
            nearest.topology_name = std::string(semantic.topology_name());
            nearest.anchor.row_index = 0;
            nearest.query_kind = TopologyQueryKind::MetricKNearest;
            nearest.include_anchor = false;
            nearest.set_param("semantic.k_nearest", std::size_t{ 2 });
            const auto nearest_result = semantic.query(nearest);
            t.expect(result_preserves_request_metadata(nearest_result, nearest),
                "semantic metric result preserves typed query metadata");
            t.expect(nearest_result.indices == std::vector<std::size_t>({ 1, 2 }) &&
                    nearest_result.distances.size() == 2 &&
                    result_payloads_align(nearest_result),
                "semantic metric result aligns neighbor identity with distance payloads");
            t.expect(nearest_result.row_count_bound == 4 &&
                    nearest_result.has_result_cap &&
                    nearest_result.result_cap == 2 &&
                    result_indices_within_bound(nearest_result),
                "semantic metric result exposes explicit boundedness");

            NeighborhoodRequest weighted{};
            weighted.topology_name = std::string(semantic.topology_name());
            weighted.anchor.row_index = 0;
            weighted.query_kind = TopologyQueryKind::WeightedNeighborSet;
            weighted.include_anchor = false;
            weighted.set_param("semantic.max_neighbors", std::size_t{ 2 });
            const auto weighted_result = semantic.query(weighted);
            t.expect(weighted_result.indices == std::vector<std::size_t>({ 1, 2 }) &&
                    weighted_result.weights.size() == 2 &&
                    result_payloads_align(weighted_result),
                "semantic weighted result aligns neighbor identity with weight payloads");
            t.expect(result_preserves_request_metadata(weighted_result, weighted) &&
                    has_param_value(weighted_result, "semantic.max_neighbors", std::size_t{ 2 }),
                "semantic weighted result preserves typed cap field");
        }

        return true;
    }

} // namespace sugar::contract_suite_internal
