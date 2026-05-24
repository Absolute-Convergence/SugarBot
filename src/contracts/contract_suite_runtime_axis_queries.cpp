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

    bool test_axis_query_temporal_contracts(ContractAsserts& t) {
        t.out << "[axis_query_temporal] temporal axis payload and neighborhood-query contracts\n";

        const std::vector<double> close = { 100.0, 101.0, 102.0, 103.0, 104.0 };

        std::shared_ptr<align::AlignedBundle> bundle;
        ColumnRef primary{};
        const bool okdv = make_single_series_dataview_from_close(close, bundle, primary, static_cast<TimeKey>(20240101));
        t.expect(okdv && bundle != nullptr, "temporal-only query fixture creates DataView input");
        if (!okdv || !bundle) return true;

        const DataView dv(*bundle);
        t.expect(dv.axes().size() == 1, "temporal-only DataView exposes one axis definition");
        t.expect(dv.topology_catalog()->has(builtin_temporal_topology_name()),
            "temporal-only DataView topology catalog includes built-in temporal topology");
        t.expect(dv.has_topology(builtin_temporal_topology_name()),
            "temporal-only DataView reports built-in temporal topology by name");
        t.expect(dv.has_topology(TopologyFamily::TemporalWindow),
            "temporal-only DataView reports temporal topology family");
        t.expect(dv.topology(builtin_temporal_topology_name()) != nullptr,
            "temporal-only DataView resolves temporal topology spec");

        auto named = dv.resolve_topology(primary.series, builtin_temporal_topology_name());
        t.expect(named != nullptr, "temporal-only DataView resolves built-in temporal topology by name");
        if (named) {
            t.expect(named->topology_name() == builtin_temporal_topology_name(),
                "temporal-only named topology resolution returns temporal query object");
        }

        ResolvedLocalitySubject primary_subject{};
        primary_subject.kind = LocalitySubjectKind::PrimarySeries;
        const auto resolved_primary_subject = dv.resolve_locality_subject(primary_subject, primary.series);
        t.expect(resolved_primary_subject.series_key == primary.series,
            "temporal-only DataView resolves primary-series locality subject to primary series key");
        t.expect(resolved_primary_subject.entity_key == primary.series,
            "temporal-only DataView resolves primary-series locality subject to identity entity key");

        auto subject_named = dv.resolve_topology(primary_subject, primary.series, builtin_temporal_topology_name());
        t.expect(subject_named != nullptr, "temporal-only DataView resolves topology from locality-subject contract");

        auto nq = dv.make_neighborhood_query(primary.series);
        t.expect(nq != nullptr, "temporal-only DataView builds neighborhood query");
        if (nq) {
            t.expect(nq->topology_name() == builtin_temporal_topology_name(),
                "temporal-only neighborhood query exposes built-in topology name");
            t.expect(nq->has_axis(AxisKind::Time), "temporal-only query advertises time axis");
            t.expect(!nq->has_axis(AxisKind::Spatial), "temporal-only query excludes spatial axis");

            const auto win = nq->query_time(3, TimeNeighborhoodRequest::last_n_samples(3));
            t.expect(win.begin == 1 && win.end == 4 && win.anchor == 3,
                "temporal-only time neighborhood returns expected contiguous row window");

            const auto canonical = nq->query(
                make_neighborhood_request(
                    nq->topology_name(),
                    3,
                    TimeNeighborhoodRequest::last_n_samples(3)));
            t.expect(canonical.topology_name == builtin_temporal_topology_name(),
                "temporal-only canonical neighborhood result preserves topology name");
            t.expect(canonical.anchor.row_index == 3,
                "temporal-only canonical neighborhood result preserves anchor row");
            t.expect(canonical.contiguous && canonical.ordered,
                "temporal-only canonical neighborhood result preserves contiguous ordered fast path");
            t.expect(canonical.begin == 1 && canonical.end == 4,
                "temporal-only canonical neighborhood result preserves ordered-window bounds");
            t.expect(canonical.materialize_indices() == std::vector<std::size_t>({ 1, 2, 3 }),
                "temporal-only canonical neighborhood result materializes contiguous indices on demand");
            t.expect(neighborhood_result_to_slice(canonical).begin == win.begin &&
                neighborhood_result_to_slice(canonical).end == win.end,
                "temporal-only compatibility adapter round-trips canonical result back to slice");
        }

        if (subject_named) {
            const auto canonical = subject_named->query(
                make_neighborhood_request(
                    subject_named->topology_name(),
                    3,
                    TimeNeighborhoodRequest::last_n_samples(2)));
            t.expect(canonical.anchor.series == primary.series,
                "temporal-only subject-bound query preserves resolved series on canonical anchor");
            t.expect(canonical.anchor.entity == primary.series,
                "temporal-only subject-bound query preserves resolved entity on canonical anchor");
        }

        ResolvedLocalitySubject observation_domain_subject{};
        observation_domain_subject.kind = LocalitySubjectKind::ObservationDomain;
        auto observation_domain_query = dv.resolve_topology(
            observation_domain_subject,
            primary.series,
            builtin_temporal_topology_name());
        t.expect(observation_domain_query != nullptr,
            "temporal-only DataView can resolve temporal topology for observation-domain subject");

        return true;
    }

    bool test_axis_query_spatial_contracts(ContractAsserts& t) {
        t.out << "[axis_query_spatial] spatial axis payload and neighborhood-query contracts\n";

        const std::vector<double> close = { 100.0, 101.0, 102.0, 103.0, 104.0 };
        const std::vector<double> x = { 0.0, 1.0, 4.0, 10.0, 11.0 };
        const std::vector<double> y = { 0.0, 0.0, 0.0, 0.0, 0.0 };

        std::shared_ptr<align::AlignedBundle> bundle;
        ColumnRef primary{};
        const bool okdv = make_single_series_dataview_with_index_schema(
            close,
            make_spatial_only_index_schema(),
            bundle,
            primary,
            static_cast<TimeKey>(0),
            x,
            y);
        t.expect(okdv && bundle != nullptr, "spatial-only query fixture creates DataView input");
        if (!okdv || !bundle) return true;

        bundle->entity_keys = { "station.alpha" };
        const DataView dv(*bundle);
        const auto spatial = dv.spatial_axis_2d(primary.series);

        t.expect(dv.topology_catalog()->has(builtin_spatial_topology_name()),
            "spatial-only DataView topology catalog includes built-in spatial topology");
        t.expect(dv.has_topology(builtin_spatial_topology_name()),
            "spatial-only DataView reports built-in spatial topology by name");
        t.expect(dv.has_topology(TopologyFamily::SpatialMetric2D),
            "spatial-only DataView reports spatial topology family");

        auto named = dv.resolve_topology(primary.series, builtin_spatial_topology_name());
        t.expect(named != nullptr, "spatial-only DataView resolves built-in spatial topology by name");
        if (named) {
            t.expect(named->topology_name() == builtin_spatial_topology_name(),
                "spatial-only named topology resolution returns spatial query object");
        }

        ResolvedLocalitySubject entity_subject{};
        entity_subject.kind = LocalitySubjectKind::EntityKey;
        entity_subject.value = "station.alpha";
        entity_subject.explicit_binding = true;
        const auto resolved_entity_subject = dv.resolve_locality_subject(entity_subject);
        t.expect(resolved_entity_subject.series_key == primary.series,
            "spatial-only DataView resolves entity-key locality subject to backing series key");
        t.expect(resolved_entity_subject.entity_key == "station.alpha",
            "spatial-only DataView preserves entity-key locality subject value");

        auto entity_named = dv.resolve_topology(entity_subject, {}, builtin_spatial_topology_name());
        t.expect(entity_named != nullptr, "spatial-only DataView resolves topology from entity-key subject");

        t.expect(spatial.has_value() && spatial->valid(), "spatial-only DataView exposes spatial axis payloads");
        if (spatial.has_value() && spatial->valid()) {
            const bool names_ok = spatial->x_name == "x" && spatial->y_name == "y";
            if (!names_ok) {
                t.out << "    spatial payload names: x='" << spatial->x_name << "' y='" << spatial->y_name << "'\n";
            }
            t.expect(names_ok,
                "spatial-only DataView preserves spatial axis column names");
            t.expect(spatial->x.size() == close.size() && spatial->y.size() == close.size(),
                "spatial-only DataView spatial payload sizes match row domain");
        }

        auto nq = dv.make_neighborhood_query(primary.series);
        t.expect(nq != nullptr, "spatial-only DataView builds neighborhood query");
        if (nq) {
            t.expect(nq->topology_name() == builtin_spatial_topology_name(),
                "spatial-only neighborhood query exposes built-in topology name");
            t.expect(!nq->has_axis(AxisKind::Time), "spatial-only query excludes time axis");
            t.expect(nq->has_axis(AxisKind::Spatial), "spatial-only query advertises spatial axis");

            const auto near = nq->query_spatial(0, SpatialNeighborhoodRequest::radius_euclidean(1.1));
            t.expect(near.indices == std::vector<std::size_t>({ 0, 1 }),
                "spatial-only radius neighborhood returns expected rows");

            const auto knear = nq->query_spatial(4, SpatialNeighborhoodRequest::nearest_k(2));
            t.expect(knear.indices == std::vector<std::size_t>({ 3, 4 }),
                "spatial-only k-nearest neighborhood returns expected rows");

            const auto canonical = nq->query(
                make_neighborhood_request(
                    nq->topology_name(),
                    4,
                    SpatialNeighborhoodRequest::nearest_k(2)));
            t.expect(canonical.topology_name == builtin_spatial_topology_name(),
                "spatial-only canonical neighborhood result preserves topology name");
            t.expect(canonical.anchor.row_index == 4,
                "spatial-only canonical neighborhood result preserves anchor row");
            t.expect(!canonical.contiguous && canonical.ordered,
                "spatial-only canonical neighborhood result stays selection-based but ordered");
            t.expect(canonical.indices == std::vector<std::size_t>({ 3, 4 }),
                "spatial-only canonical neighborhood result preserves selected indices");
            t.expect(canonical.distances.size() == canonical.indices.size(),
                "spatial-only canonical neighborhood result can preserve optional metric distances");
            t.expect(neighborhood_result_to_selection(canonical).indices == knear.indices,
                "spatial-only compatibility adapter round-trips canonical result back to selection");
        }

        if (entity_named) {
            const auto canonical = entity_named->query(
                make_neighborhood_request(
                    entity_named->topology_name(),
                    4,
                    SpatialNeighborhoodRequest::nearest_k(2)));
            t.expect(canonical.anchor.series == primary.series,
                "spatial-only entity-bound query preserves resolved series on canonical anchor");
            t.expect(canonical.anchor.entity == "station.alpha",
                "spatial-only entity-bound query preserves resolved entity on canonical anchor");
        }

        return true;
    }

    bool test_axis_query_spatiotemporal_contracts(ContractAsserts& t) {
        t.out << "[axis_query_spatiotemporal] spatiotemporal axis payload and neighborhood-query contracts\n";

        const std::vector<double> close = { 100.0, 101.0, 102.0, 103.0, 104.0 };
        const std::vector<double> x = { 0.0, 1.0, 4.0, 10.0, 11.0 };
        const std::vector<double> y = { 0.0, 0.0, 0.0, 0.0, 0.0 };

        std::shared_ptr<align::AlignedBundle> bundle;
        ColumnRef primary{};
        const bool okdv = make_single_series_dataview_with_index_schema(
            close,
            make_time_spatial_index_schema(),
            bundle,
            primary,
            static_cast<TimeKey>(20240101),
            x,
            y);
        t.expect(okdv && bundle != nullptr, "spatio-temporal query fixture creates DataView input");
        if (!okdv || !bundle) return true;

        const DataView dv(*bundle);
        t.expect(dv.topology_catalog()->has(builtin_temporal_topology_name()),
            "spatio-temporal DataView topology catalog includes temporal built-in");
        t.expect(dv.topology_catalog()->has(builtin_spatial_topology_name()),
            "spatio-temporal DataView topology catalog includes spatial built-in");
        t.expect(dv.topology_catalog()->has(builtin_spatiotemporal_topology_name()),
            "spatio-temporal DataView topology catalog includes composite built-in");
        t.expect(dv.has_topology(builtin_temporal_topology_name()) &&
            dv.has_topology(builtin_spatial_topology_name()) &&
            dv.has_topology(builtin_spatiotemporal_topology_name()),
            "spatio-temporal DataView reports all built-in named topologies");
        t.expect(dv.has_topology(TopologyFamily::TemporalWindow) &&
            dv.has_topology(TopologyFamily::SpatialMetric2D) &&
            dv.has_topology(TopologyFamily::SpatioTemporalComposite),
            "spatio-temporal DataView reports all built-in topology families");

        auto temporal_named = dv.resolve_topology(primary.series, builtin_temporal_topology_name());
        t.expect(temporal_named != nullptr, "spatio-temporal DataView resolves temporal built-in by name");
        if (temporal_named) {
            t.expect(temporal_named->topology_name() == builtin_temporal_topology_name(),
                "spatio-temporal DataView temporal named resolution returns time query object");
        }

        auto spatial_named = dv.resolve_topology(primary.series, builtin_spatial_topology_name());
        t.expect(spatial_named != nullptr, "spatio-temporal DataView resolves spatial built-in by name");
        if (spatial_named) {
            t.expect(spatial_named->topology_name() == builtin_spatial_topology_name(),
                "spatio-temporal DataView spatial named resolution returns spatial query object");
        }

        auto composite_named = dv.resolve_topology(primary.series, builtin_spatiotemporal_topology_name());
        t.expect(composite_named != nullptr, "spatio-temporal DataView resolves composite built-in by name");
        if (composite_named) {
            t.expect(composite_named->topology_name() == builtin_spatiotemporal_topology_name(),
                "spatio-temporal DataView composite named resolution returns composite query object");
        }

        ResolvedLocalitySubject primary_subject{};
        primary_subject.kind = LocalitySubjectKind::PrimarySeries;
        auto subject_named = dv.resolve_topology(
            primary_subject,
            primary.series,
            builtin_spatiotemporal_topology_name());
        t.expect(subject_named != nullptr,
            "spatio-temporal DataView resolves composite topology from primary-series subject");

        auto nq = dv.make_neighborhood_query(primary.series);
        t.expect(nq != nullptr, "spatio-temporal DataView builds neighborhood query");
        if (nq) {
            t.expect(nq->topology_name() == builtin_spatiotemporal_topology_name(),
                "spatio-temporal neighborhood query exposes built-in topology name");
            t.expect(nq->has_axis(AxisKind::Time), "spatio-temporal query advertises time axis");
            t.expect(nq->has_axis(AxisKind::Spatial), "spatio-temporal query advertises spatial axis");

            const auto both = nq->query_spatiotemporal(
                2,
                SpatioTemporalNeighborhoodRequest::intersection(
                    TimeNeighborhoodRequest::last_n_samples(3),
                    SpatialNeighborhoodRequest::radius_euclidean(3.1)));
            t.expect(both.indices == std::vector<std::size_t>({ 1, 2 }),
                "spatio-temporal neighborhood intersects time and spatial locality");

            const auto canonical = nq->query(
                make_neighborhood_request(
                    nq->topology_name(),
                    2,
                    SpatioTemporalNeighborhoodRequest::intersection(
                        TimeNeighborhoodRequest::last_n_samples(3),
                        SpatialNeighborhoodRequest::radius_euclidean(3.1))));
            t.expect(canonical.topology_name == builtin_spatiotemporal_topology_name(),
                "spatio-temporal canonical neighborhood result preserves topology name");
            t.expect(canonical.anchor.row_index == 2,
                "spatio-temporal canonical neighborhood result preserves anchor row");
            t.expect(canonical.indices == std::vector<std::size_t>({ 1, 2 }),
                "spatio-temporal canonical neighborhood result preserves intersection indices");
            t.expect(neighborhood_result_to_selection(canonical).indices == both.indices,
                "spatio-temporal compatibility adapter round-trips canonical result back to selection");
        }

        if (subject_named) {
            const auto canonical = subject_named->query(
                make_neighborhood_request(
                    subject_named->topology_name(),
                    2,
                    SpatioTemporalNeighborhoodRequest::intersection(
                        TimeNeighborhoodRequest::last_n_samples(2),
                        SpatialNeighborhoodRequest::radius_euclidean(3.1))));
            t.expect(canonical.anchor.series == primary.series,
                "spatio-temporal subject-bound query preserves resolved series on canonical anchor");
            t.expect(canonical.anchor.entity == primary.series,
                "spatio-temporal subject-bound query preserves resolved entity on canonical anchor");
        }

        return true;
    }

} // namespace sugar::contract_suite_internal
