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

    bool test_topology_design_load_contracts(ContractAsserts& t) {
        t.out << "[topology_design_loads] current built-in topology resolution design loads\n";

        {
            const std::vector<double> close = { 10.0, 11.0, 12.0, 13.0 };
            std::shared_ptr<align::AlignedBundle> bundle{};
            ColumnRef primary{};
            const bool okdv = make_single_series_dataview_from_close(
                close,
                bundle,
                primary,
                static_cast<TimeKey>(20240101));
            t.expect(okdv && bundle != nullptr,
                "temporal topology design-load fixture creates temporal DataView input");
            if (okdv && bundle) {
                const DataView dv(*bundle);
                t.expect(dv.topology_catalog()->has(builtin_temporal_topology_name()),
                    "temporal topology design-load catalog includes temporal built-in");
                t.expect(dv.has_topology(builtin_temporal_topology_name()),
                    "temporal topology design-load reports built-in named topology");
                t.expect(dv.has_topology(TopologyFamily::TemporalWindow),
                    "temporal topology design-load reports temporal topology family");
                auto named = dv.resolve_topology(primary.series, builtin_temporal_topology_name());
                t.expect(named != nullptr,
                    "temporal topology design-load resolves temporal built-in by name");
                if (named) {
                    t.expect(named->topology_name() == builtin_temporal_topology_name(),
                        "temporal topology design-load named resolution returns temporal query object");
                }

            }
        }

        {
            const std::vector<double> close = { 10.0, 20.0, 30.0, 40.0, 50.0 };
            const std::vector<double> x = { 0.0, 1.0, 2.0, 8.0, 9.0 };
            const std::vector<double> y = { 0.0, 0.0, 0.0, 0.0, 0.0 };

            std::shared_ptr<align::AlignedBundle> bundle{};
            ColumnRef primary{};
            const bool okdv = make_single_series_dataview_with_index_schema(
                close,
                make_spatial_only_index_schema(),
                bundle,
                primary,
                static_cast<TimeKey>(0),
                x,
                y);
            t.expect(okdv && bundle != nullptr,
                "spatial topology design-load fixture creates spatial-only DataView input");
            if (okdv && bundle) {
                const DataView dv(*bundle);
                t.expect(dv.topology_catalog()->has(builtin_spatial_topology_name()),
                    "spatial topology design-load catalog includes spatial built-in");
                t.expect(dv.has_topology(builtin_spatial_topology_name()),
                    "spatial topology design-load reports built-in named topology");
                t.expect(dv.has_topology(TopologyFamily::SpatialMetric2D),
                    "spatial topology design-load reports spatial topology family");
                auto named = dv.resolve_topology(primary.series, builtin_spatial_topology_name());
                t.expect(named != nullptr,
                    "spatial topology design-load resolves spatial built-in by name");
                if (named) {
                    t.expect(named->topology_name() == builtin_spatial_topology_name(),
                        "spatial topology design-load named resolution returns spatial query object");
                }

            }
        }

        {
            const std::vector<double> close = { 10.0, 20.0, 30.0, 40.0 };
            const std::vector<double> x = { 0.0, 0.4, 10.0, 10.4 };
            const std::vector<double> y = { 0.0, 0.0, 0.0, 0.0 };

            std::shared_ptr<align::AlignedBundle> bundle{};
            ColumnRef primary{};
            const bool okdv = make_single_series_dataview_with_index_schema(
                close,
                make_time_spatial_index_schema(),
                bundle,
                primary,
                static_cast<TimeKey>(20240101),
                x,
                y);
            t.expect(okdv && bundle != nullptr,
                "spatiotemporal topology design-load fixture creates spatiotemporal DataView input");
            if (okdv && bundle) {
                const DataView dv(*bundle);
                t.expect(dv.topology_catalog()->has(builtin_temporal_topology_name()),
                    "spatiotemporal topology design-load catalog includes temporal built-in");
                t.expect(dv.topology_catalog()->has(builtin_spatial_topology_name()),
                    "spatiotemporal topology design-load catalog includes spatial built-in");
                t.expect(dv.topology_catalog()->has(builtin_spatiotemporal_topology_name()),
                    "spatiotemporal topology design-load catalog includes composite built-in");
                t.expect(dv.has_topology(TopologyFamily::TemporalWindow) &&
                    dv.has_topology(TopologyFamily::SpatialMetric2D) &&
                    dv.has_topology(TopologyFamily::SpatioTemporalComposite),
                    "spatiotemporal topology design-load reports all built-in topology families");

                auto temporal_named = dv.resolve_topology(primary.series, builtin_temporal_topology_name());
                auto spatial_named = dv.resolve_topology(primary.series, builtin_spatial_topology_name());
                auto composite_named = dv.resolve_topology(primary.series, builtin_spatiotemporal_topology_name());
                t.expect(temporal_named != nullptr,
                    "spatiotemporal topology design-load resolves temporal built-in by name");
                t.expect(spatial_named != nullptr,
                    "spatiotemporal topology design-load resolves spatial built-in by name");
                t.expect(composite_named != nullptr,
                    "spatiotemporal topology design-load resolves composite built-in by name");
                if (composite_named) {
                    t.expect(composite_named->topology_name() == builtin_spatiotemporal_topology_name(),
                        "spatiotemporal topology design-load named resolution returns composite query object");
                }

            }
        }

        return true;
    }

    bool test_topology_overload_rejection_contracts(ContractAsserts& t) {
        t.out << "[topology_overload_rejection] current built-in topology overload rejection contracts\n";

        {
            const std::vector<double> close = { 10.0, 11.0, 12.0, 13.0 };
            std::shared_ptr<align::AlignedBundle> bundle{};
            ColumnRef primary{};
            const bool okdv = make_single_series_dataview_from_close(
                close,
                bundle,
                primary,
                static_cast<TimeKey>(20240101));
            t.expect(okdv && bundle != nullptr,
                "temporal topology overload fixture creates temporal DataView input");
            if (okdv && bundle) {
                const DataView dv(*bundle);
                expect_topology_resolution_failure(
                    t,
                    "temporal topology overload missing-name case",
                    "is not available on this runtime",
                    [&]() {
                        return dv.resolve_topology(primary.series, "missing.topology");
                    });
                expect_topology_resolution_failure(
                    t,
                    "temporal topology overload spatial-name case",
                    "is not available on this runtime",
                    [&]() {
                        return dv.resolve_topology(primary.series, builtin_spatial_topology_name());
                    });
            }
        }

        {
            const std::vector<double> close = { 10.0, 20.0, 30.0, 40.0 };
            const std::vector<double> x = { 0.0, 0.1, 10.0, 10.1 };
            const std::vector<double> y = { 0.0, 0.0, 0.0, 0.0 };

            std::shared_ptr<align::AlignedBundle> bundle{};
            ColumnRef primary{};
            const bool okdv = make_single_series_dataview_with_index_schema(
                close,
                make_spatial_only_index_schema(),
                bundle,
                primary,
                static_cast<TimeKey>(0),
                x,
                y);
            t.expect(okdv && bundle != nullptr,
                "spatial topology overload fixture creates spatial-only DataView input");
            if (okdv && bundle) {
                const DataView dv(*bundle);
                RuntimeLocalitySubject observation_subject{};
                observation_subject.kind = LocalitySubjectKind::ObservationDomain;
                expect_topology_resolution_failure(
                    t,
                    "spatial topology overload temporal-name case",
                    "is not available on this runtime",
                    [&]() {
                        return dv.resolve_topology(primary.series, builtin_temporal_topology_name());
                    });
                expect_topology_resolution_failure(
                    t,
                    "spatial topology overload missing-series-local-subject case",
                    "requires a resolved series-local subject",
                    [&]() {
                        return dv.resolve_topology(observation_subject, builtin_spatial_topology_name());
                    });
            }
        }

        {
            const std::vector<double> close = { 10.0, 11.0, 12.0, 13.0 };
            std::shared_ptr<align::AlignedBundle> bundle{};
            ColumnRef primary{};
            const bool okdv = make_single_series_dataview_from_close(
                close,
                bundle,
                primary,
                static_cast<TimeKey>(20240101));
            t.expect(okdv && bundle != nullptr,
                "semantic topology overload fixture creates temporal DataView input");
            t.expect(bundle && bundle->topology_catalog != nullptr,
                "semantic topology overload fixture preserves topology catalog sidecar");
            if (okdv && bundle && bundle->topology_catalog) {
                TopologySpec semantic_spec{};
                semantic_spec.name = "entity.semantic.graph";
                semantic_spec.origin = TopologyOrigin::LayoutDeclared;
                semantic_spec.bearing = TopologyBearing::MetadataOnly;
                semantic_spec.family = TopologyFamily::SemanticGraph;
                semantic_spec.domain_scope = TopologyDomainScope::ObservationDomain;
                semantic_spec.query_kinds = { TopologyQueryKind::WeightedNeighborSet };
                bundle->topology_catalog->add(semantic_spec);

                const DataView dv(*bundle);
                expect_topology_resolution_failure(
                    t,
                    "semantic topology overload unimplemented-family case",
                    "metadata-only",
                    [&]() {
                        return dv.resolve_topology(primary.series, "entity.semantic.graph");
                    });
            }
        }

        return true;
    }

    bool test_topology_metadata_preservation_contracts(ContractAsserts& t) {
        t.out << "[topology_metadata_preservation] current topology metadata preservation contracts\n";

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
            RunManifest manifest{};
            append_plan_capability_snapshot(report, manifest);

            const auto* usage = find_topology_usage(manifest, "mean", builtin_spatial_topology_name());
            t.expect(usage != nullptr,
                "topology metadata snapshot records missing built-in topology usage");
            if (usage) {
                t.expect(!usage->catalog_present,
                    "topology metadata snapshot preserves missing built-in catalog state");
                t.expect(!usage->runtime_resolved,
                    "topology metadata snapshot preserves unresolved built-in runtime state");
                t.expect(usage->required_topology_families ==
                        std::vector<std::string>{ std::string(topology_family_name(TopologyFamily::SpatialMetric2D)) },
                    "topology metadata snapshot preserves required built-in topology family");
            }
        }

        {
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
            t.expect(runtime_ok, "topology metadata semantic fixture builds single-series DataView");
            t.expect(bundle && bundle->topology_catalog != nullptr,
                "topology metadata semantic fixture preserves topology catalog sidecar");
            if (runtime_ok && bundle && bundle->topology_catalog) {
                TopologySpec semantic_spec{};
                semantic_spec.name = "entity.semantic.graph";
                semantic_spec.origin = TopologyOrigin::LayoutDeclared;
                semantic_spec.bearing = TopologyBearing::MetadataOnly;
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

                PlanSpec semantic_plan{};
                semantic_plan.numeric_nodes = {
                    NodeSpec{ "c", "core.close", {}, {} },
                    NodeSpec{ "semantic_mean", "semantic.weighted_mean", { "c" }, {} },
                };

                const auto report = diagnose_plan_capabilities(semantic_plan, registry, runtime);
                append_plan_capability_snapshot(report, runtime.run_manifest);

                const auto* usage = find_topology_usage(runtime.run_manifest, "semantic_mean", "entity.semantic.graph");
                t.expect(usage != nullptr,
                    "topology metadata snapshot records semantic topology usage");
                if (usage) {
                    t.expect(usage->catalog_present,
                        "topology metadata snapshot preserves semantic topology catalog presence");
                    t.expect(!usage->runtime_resolved,
                        "topology metadata snapshot preserves unresolved semantic runtime state");
                    t.expect(usage->topology_origin == "layout_declared",
                        "topology metadata snapshot preserves semantic topology origin classification");
                    t.expect(usage->topology_bearing == "metadata_only",
                        "topology metadata snapshot preserves semantic topology bearing classification");
                    t.expect(usage->artifact_ref.stable_id == "entity.semantic.graph.v1",
                        "topology metadata snapshot preserves semantic topology artifact stable id");
                    t.expect(usage->required_topology_query_kinds ==
                            std::vector<std::string>{ std::string(topology_query_kind_name(TopologyQueryKind::WeightedNeighborSet)) },
                        "topology metadata snapshot preserves semantic topology query-kind metadata");
                }
            }
        }

        return true;
    }

} // namespace sugar::contract_suite_internal
