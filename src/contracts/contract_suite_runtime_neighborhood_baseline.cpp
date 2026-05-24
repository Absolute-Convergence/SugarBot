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

    bool test_neighborhood_query_baseline_contracts(ContractAsserts& t) {
        t.out << "[neighborhood_query_baseline] NeighborhoodQuery baseline contracts\n";

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
            const std::vector<TimeKey> time = { 10, 20, 30, 40, 50 };
            const TimeNeighborhoodQuery q(time);
            t.expect(q.topology_name() == builtin_temporal_topology_name(),
                "temporal query exposes stable topology name");
            t.expect(q.row_count() == time.size(),
                "temporal query exposes row-domain boundedness");
            t.expect(q.has_axis(AxisKind::Time) && !q.has_axis(AxisKind::Spatial),
                "temporal query exposes only the time axis");

            const auto request = make_neighborhood_request(
                q.topology_name(),
                3,
                TimeNeighborhoodRequest::last_n_samples(3));
            const auto first = q.query(request);
            const auto second = q.query(request);
            t.expect(first.topology_name == builtin_temporal_topology_name() &&
                    first.anchor.row_index == 3,
                "temporal result preserves topology name and anchor row");
            t.expect(first.contiguous && first.ordered && first.begin == 1 && first.end == 4,
                "temporal last-n query preserves deterministic contiguous bounds");
            t.expect(first.materialize_indices() == std::vector<std::size_t>({ 1, 2, 3 }),
                "temporal result materializes bounded indices deterministically");
            t.expect(second.begin == first.begin && second.end == first.end &&
                    second.materialize_indices() == first.materialize_indices(),
                "temporal repeated query returns identical results");

            const auto excluded = q.query(make_neighborhood_request(
                q.topology_name(),
                3,
                TimeNeighborhoodRequest::last_n_samples(2, false)));
            t.expect(excluded.materialize_indices() == std::vector<std::size_t>({ 1, 2 }),
                "temporal include_anchor=false remains bounded before the anchor");

            const auto lookback = q.query(make_neighborhood_request(
                q.topology_name(),
                3,
                TimeNeighborhoodRequest::lookback(5)));
            t.expect(lookback.materialize_indices() == std::vector<std::size_t>({ 3 }),
                "temporal lookback query uses declared time bound");

            NeighborhoodRequest bad = request;
            bad.query_kind = TopologyQueryKind::NeighborSet;
            expect_throws_with(
                [&]() { (void)q.query(bad); },
                "only ordered-window queries are supported",
                "temporal unsupported query kind fails explicitly");
        }

        {
            const double nan = std::numeric_limits<double>::quiet_NaN();
            const std::vector<double> x = { 0.0, 1.0, -1.0, nan };
            const std::vector<double> y = { 0.0, 0.0, 0.0, 0.0 };
            auto q = std::make_unique<SpatialNeighborhoodQuery2D>(x, y);
            t.expect(q->topology_name() == builtin_spatial_topology_name(),
                "spatial query exposes stable topology name");
            t.expect(q->row_count() == x.size(),
                "spatial query exposes row-domain boundedness");
            t.expect(!q->has_axis(AxisKind::Time) && q->has_axis(AxisKind::Spatial),
                "spatial query exposes only the spatial axis");

            const auto radius_request = make_neighborhood_request(
                q->topology_name(),
                0,
                SpatialNeighborhoodRequest::radius_euclidean(1.1));
            const auto radius_first = q->query(radius_request);
            const auto radius_second = q->query(radius_request);
            t.expect(radius_first.indices == std::vector<std::size_t>({ 0, 1, 2 }),
                "spatial radius query is deterministic and skips non-finite candidates");
            t.expect(radius_first.indices == radius_second.indices &&
                    radius_first.distances == radius_second.distances,
                "spatial repeated query returns identical indices and distances");
            t.expect(radius_first.distances.size() == radius_first.indices.size() &&
                    nearly_equal(radius_first.distances[0], 0.0) &&
                    nearly_equal(radius_first.distances[1], 1.0) &&
                    nearly_equal(radius_first.distances[2], 1.0),
                "spatial distance payload aligns with selected indices");

            const auto knear = q->query(make_neighborhood_request(
                q->topology_name(),
                0,
                SpatialNeighborhoodRequest::nearest_k(1, false)));
            t.expect(knear.indices == std::vector<std::size_t>({ 1 }) &&
                    knear.distances.size() == 1 &&
                    nearly_equal(knear.distances[0], 1.0),
                "spatial k-nearest query obeys explicit cap and tie order");

            NeighborhoodRequest bad = radius_request;
            bad.query_kind = TopologyQueryKind::WeightedNeighborSet;
            expect_throws_with(
                [&]() { (void)q->query(bad); },
                "only spatial metric queries are supported",
                "spatial unsupported query kind fails explicitly");

            NeighborhoodAnchor defaults{};
            defaults.series = "series.alpha";
            defaults.entity = "entity.alpha";
            auto bound = bind_neighborhood_query_subject(std::move(q), defaults);
            const auto bound_result = bound->query(make_neighborhood_request(
                bound->topology_name(),
                0,
                SpatialNeighborhoodRequest::nearest_k(1)));
            t.expect(bound_result.anchor.series == "series.alpha" &&
                    bound_result.anchor.entity == "entity.alpha",
                "bound query fills default subject provenance on the canonical anchor");
        }

        {
            const std::vector<TimeKey> time = { 10, 20, 30, 40, 50 };
            const std::vector<double> x = { 0.0, 1.0, 3.0, 1.0, 10.0 };
            const std::vector<double> y = { 0.0, 0.0, 0.0, 0.0, 0.0 };
            const SpatioTemporalNeighborhoodQuery2D q(time, x, y);
            t.expect(q.topology_name() == builtin_spatiotemporal_topology_name(),
                "spatiotemporal query exposes stable topology name");
            t.expect(q.row_count() == time.size(),
                "spatiotemporal query exposes row-domain boundedness");
            t.expect(q.has_axis(AxisKind::Time) && q.has_axis(AxisKind::Spatial),
                "spatiotemporal query exposes time and spatial axes");

            const auto intersection_request = make_neighborhood_request(
                q.topology_name(),
                3,
                SpatioTemporalNeighborhoodRequest::intersection(
                    TimeNeighborhoodRequest::last_n_samples(3),
                    SpatialNeighborhoodRequest::radius_euclidean(0.1)));
            const auto intersection_first = q.query(intersection_request);
            const auto intersection_second = q.query(intersection_request);
            t.expect(intersection_first.topology_name == builtin_spatiotemporal_topology_name() &&
                    intersection_first.anchor.row_index == 3,
                "spatiotemporal result preserves topology name and anchor row");
            t.expect(intersection_first.indices == std::vector<std::size_t>({ 1, 3 }),
                "spatiotemporal intersection combines bounded time and spatial queries deterministically");
            t.expect(intersection_second.indices == intersection_first.indices,
                "spatiotemporal repeated intersection returns identical results");

            auto union_request = make_neighborhood_request(
                q.topology_name(),
                2,
                SpatioTemporalNeighborhoodRequest::intersection(
                    TimeNeighborhoodRequest::last_n_samples(2),
                    SpatialNeighborhoodRequest::radius_euclidean(2.1)));
            union_request.set_param("spatiotemporal.require_both", false);
            const auto union_result = q.query(union_request);
            t.expect(union_result.indices == std::vector<std::size_t>({ 1, 2, 3 }),
                "spatiotemporal union mode is deterministic and sorted");

            NeighborhoodRequest bad = intersection_request;
            bad.query_kind = TopologyQueryKind::TransitionSet;
            expect_throws_with(
                [&]() { (void)q.query(bad); },
                "unsupported query kind",
                "spatiotemporal unsupported query kind fails explicitly");
        }

        {
            NeighborhoodSelection selection{};
            selection.anchor = 7;
            selection.indices = { 2, 4 };
            auto result = make_neighborhood_result(
                "graph.prototype",
                NeighborhoodAnchor{ 7, "series.beta", "entity.beta" },
                selection,
                { 0.25, 0.75 },
                { 0.8, 0.2 },
                false);
            t.expect(result.size() == 2 && result.materialize_indices() == selection.indices,
                "canonical result helper preserves explicit neighbor identity payload");
            t.expect(result.distances == std::vector<double>({ 0.25, 0.75 }) &&
                    result.weights == std::vector<double>({ 0.8, 0.2 }),
                "canonical result helper preserves aligned distance and weight payloads");
            t.expect(result.anchor.series == "series.beta" &&
                    result.anchor.entity == "entity.beta" &&
                    result.topology_name == "graph.prototype",
                "canonical result helper preserves topology and subject provenance");
        }

        return true;
    }

} // namespace sugar::contract_suite_internal
