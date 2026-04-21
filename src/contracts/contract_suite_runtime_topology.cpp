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

    bool test_locality_design_load_contracts(ContractAsserts& t) {
        t.out << "[locality_design_loads] current locality execution design loads\n";

        {
            const std::vector<double> close = { 10.0, 20.0, 30.0 };
            const std::vector<double> x = { 0.0, 0.1, 10.0 };
            const std::vector<double> y = { 0.0, 0.0, 0.0 };

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
                "spatial locality design-load fixture creates spatial-only DataView input");
            if (okdv && bundle) {
                const PlanSpec spec = make_spatial_radius_mean_plan(
                    "contract_locality_design_spatial",
                    0.2,
                    16.0,
                    std::string(builtin_spatial_topology_name()));

                const DataView dv(*bundle);
                ProgramSignalOutputs signals{};
                std::string err;
                const bool eval_ok = compile_and_eval_plan_signals(spec, dv, primary, signals, err);
                t.expect(eval_ok, "spatial.radius_mean design-load plan compiles and evaluates");
                t.expect(err.empty(), "spatial.radius_mean design-load evaluation has no error");
                t.expect(signals.entry_long.size() == close.size(),
                    "spatial.radius_mean design-load evaluation produces row-domain-sized entry buffer");
                if (signals.entry_long.size() == close.size()) {
                    t.expect(signals.entry_long[0] == kBoolFalse,
                        "spatial.radius_mean design load keeps first row below threshold when local mean is 15");
                    t.expect(signals.entry_long[1] == kBoolFalse,
                        "spatial.radius_mean design load keeps second row below threshold when local mean is 15");
                    t.expect(signals.entry_long[2] == kBoolTrue,
                        "spatial.radius_mean design load raises final row when isolated local mean exceeds threshold");
                }
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
                make_time_spatial_index_schema(),
                bundle,
                primary,
                static_cast<TimeKey>(20240101),
                x,
                y);
            t.expect(okdv && bundle != nullptr,
                "spatiotemporal locality design-load fixture creates spatiotemporal DataView input");
            if (okdv && bundle) {
                const PlanSpec spec = make_spatiotemporal_radius_mean_plan(
                    "contract_locality_design_spatiotemporal",
                    2,
                    0.2,
                    25.0,
                    std::string(builtin_spatiotemporal_topology_name()));

                const DataView dv(*bundle);
                ProgramSignalOutputs signals{};
                std::string err;
                const bool eval_ok = compile_and_eval_plan_signals(spec, dv, primary, signals, err);
                t.expect(eval_ok, "st.radius_mean design-load plan compiles and evaluates");
                t.expect(err.empty(), "st.radius_mean design-load evaluation has no error");
                t.expect(signals.entry_long.size() == close.size(),
                    "st.radius_mean design-load evaluation produces row-domain-sized entry buffer");
                if (signals.entry_long.size() == close.size()) {
                    t.expect(signals.entry_long[0] == kBoolFalse,
                        "st.radius_mean design load keeps warmup row false");
                    t.expect(signals.entry_long[1] == kBoolFalse,
                        "st.radius_mean design load keeps second row below threshold when local mean is 15");
                    t.expect(signals.entry_long[2] == kBoolTrue,
                        "st.radius_mean design load raises third row when local mean exceeds threshold");
                    t.expect(signals.entry_long[3] == kBoolTrue,
                        "st.radius_mean design load raises final row when local mean exceeds threshold");
                }
            }
        }

        {
            const std::vector<double> primary_close = { 1.0, 100.0, 1.0 };
            const std::vector<double> secondary_close = { 0.0, 0.0, 0.0 };
            const std::vector<double> primary_x = { 0.0, 0.1, 10.0 };
            const std::vector<double> primary_y = { 0.0, 0.0, 0.0 };
            const std::vector<double> secondary_x = { 0.0, 10.0, 10.1 };
            const std::vector<double> secondary_y = { 0.0, 0.0, 0.0 };

            std::shared_ptr<align::AlignedBundle> bundle{};
            ColumnRef primary{};
            const bool okdv = make_two_series_dataview_with_index_schema(
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
            t.expect(okdv && bundle != nullptr,
                "locality-subject design-load fixture creates multi-series DataView input");
            if (okdv && bundle) {
                const PlanSpec default_spec = make_spatial_radius_mean_plan(
                    "contract_locality_design_default_subject",
                    0.2,
                    50.0,
                    std::string(builtin_spatial_topology_name()));
                const PlanSpec subject_spec = make_spatial_radius_mean_plan(
                    "contract_locality_design_explicit_subject",
                    0.2,
                    50.0,
                    std::string(builtin_spatial_topology_name()),
                    std::string("B"));

                const DataView dv(*bundle);
                ProgramSignalOutputs default_signals{};
                ProgramSignalOutputs subject_signals{};
                std::string err;
                const bool default_ok = compile_and_eval_plan_signals(default_spec, dv, primary, default_signals, err);
                t.expect(default_ok, "default locality-subject design-load plan compiles and evaluates");
                t.expect(err.empty(), "default locality-subject design-load evaluation has no error");
                err.clear();
                const bool subject_ok = compile_and_eval_plan_signals(subject_spec, dv, primary, subject_signals, err);
                t.expect(subject_ok, "explicit locality-subject design-load plan compiles and evaluates");
                t.expect(err.empty(), "explicit locality-subject design-load evaluation has no error");
                t.expect(default_signals.entry_long.size() == primary_close.size() &&
                        subject_signals.entry_long.size() == primary_close.size(),
                    "locality-subject design-load evaluation produces row-domain-sized buffers for both bindings");
                if (default_signals.entry_long.size() == primary_close.size() &&
                    subject_signals.entry_long.size() == primary_close.size()) {
                    t.expect(default_signals.entry_long[0] == kBoolTrue,
                        "default locality subject uses primary-series locality by default");
                    t.expect(subject_signals.entry_long[0] == kBoolFalse,
                        "explicit subject_series redirects locality to the alternate series");
                }
            }
        }

        return true;
    }

    bool test_locality_overload_rejection_contracts(ContractAsserts& t) {
        t.out << "[locality_overload_rejection] current locality overload rejection contracts\n";

        PlanSweepRuntimeOptions opt{};

        {
            const std::vector<double> close = { 10.0, 20.0, 30.0 };
            const std::vector<double> x = { 0.0, 0.1, 10.0 };
            const std::vector<double> y = { 0.0, 0.0, 0.0 };

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
                "locality overload fixture creates spatial-only DataView input");
            if (okdv && bundle) {
                auto spec = make_spatial_radius_mean_plan(
                    "contract_locality_overload_missing_topology",
                    1.1,
                    14.0,
                    "missing.topology");
                add_standard_exit_binding(spec);

                const DataView dv(*bundle);
                std::ostringstream log;
                BacktestResult run{};
                const bool ok = run_custom_plan_spec(dv, primary, spec, opt, log, &run);
                t.expect(!ok,
                    "locality overload rejects unavailable named topology for spatial.radius_mean");
                t.expect(log.str().find("required topology 'missing.topology' is not available on DataView") != std::string::npos,
                    "locality overload preserves explicit missing-topology reason");
            }
        }

        {
            const std::vector<double> close = { 10.0, 20.0, 30.0 };
            const std::vector<double> x = { 0.0, 0.1, 10.0 };
            const std::vector<double> y = { 0.0, 0.0, 0.0 };

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
                "locality bad-subject fixture creates spatial-only DataView input");
            if (okdv && bundle) {
                auto spec = make_spatial_radius_mean_plan(
                    "contract_locality_overload_bad_subject",
                    1.1,
                    14.0,
                    std::string(builtin_spatial_topology_name()),
                    std::string("missing.series"));
                add_standard_exit_binding(spec);

                const DataView dv(*bundle);
                std::ostringstream log;
                BacktestResult run{};
                const bool ok = run_custom_plan_spec(dv, primary, spec, opt, log, &run);
                t.expect(!ok,
                    "locality overload rejects unresolved locality subject for spatial.radius_mean");
                const std::string msg = log.str();
                t.expect(msg.find("locality subject could not be resolved") != std::string::npos,
                    "locality overload preserves explicit locality-subject resolution failure");
                t.expect(msg.find("series not found") != std::string::npos,
                    "locality overload preserves missing-series detail");
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
                "locality topology-family overload fixture creates spatiotemporal DataView input");
            if (okdv && bundle) {
                auto spec = make_spatiotemporal_radius_mean_plan(
                    "contract_locality_overload_topology_family",
                    2,
                    0.5,
                    20.0,
                    std::string(builtin_temporal_topology_name()));
                add_standard_exit_binding(spec);

                const DataView dv(*bundle);
                std::ostringstream log;
                BacktestResult run{};
                const bool ok = run_custom_plan_spec(dv, primary, spec, opt, log, &run);
                t.expect(!ok,
                    "locality overload rejects topology family mismatch for st.radius_mean");
                const std::string msg = log.str();
                t.expect(msg.find("has family 'temporal_window'") != std::string::npos,
                    "locality overload reports actual mismatched topology family");
                t.expect(msg.find("spatiotemporal_composite") != std::string::npos,
                    "locality overload reports required topology family");
            }
        }

        return true;
    }

    bool test_locality_metadata_preservation_contracts(ContractAsserts& t) {
        t.out << "[locality_metadata_preservation] current locality metadata preservation contracts\n";

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
            t.expect(runtime_ok, "locality metadata semantic fixture builds single-series DataView");
            t.expect(bundle && bundle->topology_catalog != nullptr,
                "locality metadata semantic fixture preserves topology catalog sidecar");
            if (runtime_ok && bundle && bundle->topology_catalog) {
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
                append_plan_capability_snapshot(report, runtime.run_manifest);

                const auto* usage = find_topology_usage(runtime.run_manifest, "semantic_mean", "entity.semantic.graph");
                t.expect(usage != nullptr,
                    "locality metadata snapshot records semantic topology usage without runtime execution");
                if (usage) {
                    t.expect(usage->catalog_present,
                        "locality metadata snapshot preserves semantic topology catalog presence");
                    t.expect(!usage->runtime_resolved,
                        "locality metadata snapshot preserves unresolved runtime state for semantic topology");
                    t.expect(usage->artifact_ref.stable_id == "entity.semantic.graph.v1",
                        "locality metadata snapshot preserves semantic topology artifact stable id");
                }
            }
        }

        {
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
            t.expect(runtime_ok, "locality metadata subject fixture builds two-series spatio-temporal DataView");
            if (runtime_ok && bundle) {
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

                const auto registry = make_default_registry();

                const PlanSpec unresolved_spec = make_spatial_radius_mean_plan(
                    "contract_locality_metadata_unresolved_subject",
                    0.2,
                    0.0,
                    std::string(builtin_spatial_topology_name()),
                    std::string("missing.series"));

                const auto unresolved_report = diagnose_plan_capabilities(unresolved_spec, registry, runtime);
                append_plan_capability_snapshot(unresolved_report, runtime.run_manifest);

                const auto* unresolved_usage = find_topology_usage(
                    runtime.run_manifest,
                    "mean",
                    builtin_spatial_topology_name());
                t.expect(unresolved_usage != nullptr,
                    "locality metadata snapshot records unresolved locality subject usage");
                if (unresolved_usage) {
                    t.expect(unresolved_usage->locality_subject_kind == "series_key",
                        "locality metadata snapshot preserves unresolved subject kind");
                    t.expect(unresolved_usage->locality_subject_value == "missing.series",
                        "locality metadata snapshot preserves unresolved subject value");
                    t.expect(unresolved_usage->locality_subject_explicit,
                        "locality metadata snapshot preserves unresolved explicit binding");
                    t.expect(unresolved_usage->resolved_series_key.empty(),
                        "locality metadata snapshot keeps resolved series empty on subject failure");
                    t.expect(unresolved_usage->resolved_entity_key.empty(),
                        "locality metadata snapshot keeps resolved entity empty on subject failure");
                }

                const PlanSpec resolved_spec = make_spatial_radius_mean_plan(
                    "contract_locality_metadata_resolved_subject",
                    0.2,
                    0.0,
                    std::string(builtin_spatial_topology_name()),
                    std::string("B"));

                const auto resolved_report = diagnose_plan_capabilities(resolved_spec, registry, runtime);
                append_plan_capability_snapshot(resolved_report, runtime.run_manifest);

                const auto* resolved_usage = find_topology_usage(
                    runtime.run_manifest,
                    "mean",
                    builtin_spatial_topology_name());
                t.expect(resolved_usage != nullptr,
                    "locality metadata snapshot records resolved locality subject usage");
                if (resolved_usage) {
                    t.expect(resolved_usage->locality_subject_kind == "series_key",
                        "locality metadata snapshot preserves resolved subject kind");
                    t.expect(resolved_usage->locality_subject_value == "B",
                        "locality metadata snapshot preserves resolved subject value");
                    t.expect(resolved_usage->locality_subject_explicit,
                        "locality metadata snapshot preserves resolved explicit binding");
                    t.expect(resolved_usage->resolved_series_key == "B",
                        "locality metadata snapshot preserves resolved series key");
                    t.expect(resolved_usage->resolved_entity_key == "entity.B",
                        "locality metadata snapshot preserves resolved entity key");
                    t.expect(resolved_usage->runtime_resolved,
                        "locality metadata snapshot preserves successful runtime resolution");
                }
            }
        }

        return true;
    }

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

    bool test_axis_domain_runtime_contracts(ContractAsserts& t) {
        t.out << "[axis_domains] temporal/spatial/spatiotemporal runtime domain contracts\n";

        const std::vector<double> close = {
            100.0, 101.0, 102.0, 103.0, 104.0, 105.0, 106.0, 107.0
        };
        const std::vector<double> spatial_x = {
            0.0, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0
        };
        const std::vector<double> spatial_y(close.size(), 0.0);

        PlanSpec rowwise_spec{};
        rowwise_spec.version = 1;
        rowwise_spec.name = "contract_axis_domain_rowwise";
        rowwise_spec.trade_mode = TradeMode::LongOnly;

        NodeSpec rw_close{};
        rw_close.id = "c";
        rw_close.op = "core.close";

        NodeSpec rw_thr{};
        rw_thr.id = "thr";
        rw_thr.op = "core.const";
        rw_thr.params["value"] = 103.0;

        rowwise_spec.numeric_nodes = { rw_close, rw_thr };

        NodeSpec rw_entry{};
        rw_entry.id = "entry";
        rw_entry.op = "cmp.gt";
        rw_entry.args = { "c", "thr" };

        NodeSpec rw_exit{};
        rw_exit.id = "exit";
        rw_exit.op = "cmp.lt";
        rw_exit.args = { "c", "thr" };

        rowwise_spec.bool_nodes = { rw_entry, rw_exit };
        rowwise_spec.bind.entry_long = "entry";
        rowwise_spec.bind.exit_long = "exit";

        PlanSpec temporal_local_spec = rowwise_spec;
        temporal_local_spec.name = "contract_axis_domain_temporal_local";

        NodeSpec tl_sma{};
        tl_sma.id = "sma";
        tl_sma.op = "ts.sma";
        tl_sma.args = { "c" };
        tl_sma.params["n"] = static_cast<std::int64_t>(3);

        temporal_local_spec.numeric_nodes = { tl_sma, rw_close, rw_thr };

        NodeSpec tl_entry{};
        tl_entry.id = "entry";
        tl_entry.op = "cmp.gt";
        tl_entry.args = { "sma", "thr" };

        NodeSpec tl_exit{};
        tl_exit.id = "exit";
        tl_exit.op = "cmp.lt";
        tl_exit.args = { "sma", "thr" };

        temporal_local_spec.bool_nodes = { tl_entry, tl_exit };
        temporal_local_spec.bind.entry_long = "entry";
        temporal_local_spec.bind.exit_long = "exit";

        PlanSpec mixed_axis_spec = rowwise_spec;
        mixed_axis_spec.name = "contract_axis_domain_mixed_axes";

        NodeSpec mx_spatial{};
        mx_spatial.id = "sp";
        mx_spatial.op = "spatial.identity";
        mx_spatial.args = { "c" };

        mixed_axis_spec.numeric_nodes = { rw_close, mx_spatial, rw_thr };

        NodeSpec mx_entry{};
        mx_entry.id = "entry";
        mx_entry.op = "cmp.gt";
        mx_entry.args = { "sp", "thr" };

        NodeSpec mx_exit{};
        mx_exit.id = "exit";
        mx_exit.op = "cmp.lt";
        mx_exit.args = { "sp", "thr" };

        mixed_axis_spec.bool_nodes = { mx_entry, mx_exit };
        mixed_axis_spec.bind.entry_long = "entry";
        mixed_axis_spec.bind.exit_long = "exit";

        PlanSweepRuntimeOptions opt{};

        {
            std::shared_ptr<align::AlignedBundle> bundle;
            ColumnRef primary{};
            const bool okdv = make_single_series_dataview_from_close(close, bundle, primary);
            t.expect(okdv && bundle != nullptr, "temporal-only fixture creates DataView input");
            if (okdv && bundle) {
                const DataView dv(*bundle);
                t.expect(dv.row_count() == close.size(), "temporal-only DataView row_count follows observation domain");
                t.expect(!dv.empty(), "temporal-only DataView empty() is false");
                t.expect(dv.index_schema() != nullptr, "temporal-only DataView exposes index_schema");
                t.expect(bundle->row_count() == close.size(), "temporal-only AlignedBundle row_count follows observation domain");
                t.expect(bundle->time_axis().size() == close.size(), "temporal-only AlignedBundle exposes time_axis values");
                t.expect(dv.has_axis(AxisKind::Time), "temporal-only DataView has time axis");
                t.expect(!dv.has_axis(AxisKind::Spatial), "temporal-only DataView excludes spatial axis");
                t.expect(dv.axis(AxisKind::Time) != nullptr, "temporal-only DataView resolves time axis definition");
                t.expect(dv.axis(AxisKind::Spatial) == nullptr, "temporal-only DataView has no spatial axis definition");
                t.expect(dv.time().size() == close.size(), "temporal-only DataView compatibility time() follows semantic time axis");
                t.expect(dv.time_axis().size() == close.size(), "temporal-only DataView exposes semantic time_axis values");

                std::ostringstream rowwise_log;
                BacktestResult rowwise_out{};
                const bool rowwise_ok = run_custom_plan_spec(dv, primary, rowwise_spec, opt, rowwise_log, &rowwise_out);
                t.expect(rowwise_ok, "row-wise contract plan executes on temporal-only DataView");
            }
        }

        {
            std::shared_ptr<align::AlignedBundle> bundle;
            ColumnRef primary{};
            const bool okdv = make_single_series_dataview_with_index_schema(
                close, make_spatial_only_index_schema(), bundle, primary, static_cast<TimeKey>(0), spatial_x, spatial_y);
            t.expect(okdv && bundle != nullptr, "spatial-only fixture creates DataView input");
            if (okdv && bundle) {
                const DataView dv(*bundle);
                t.expect(dv.row_count() == close.size(), "spatial-only DataView row_count follows observation domain");
                t.expect(bundle->row_count() == close.size(), "spatial-only AlignedBundle row_count follows observation domain");
                t.expect(dv.index_schema() != nullptr, "spatial-only DataView exposes index_schema");
                t.expect(!dv.has_axis(AxisKind::Time), "spatial-only DataView excludes time axis");
                t.expect(dv.has_axis(AxisKind::Spatial), "spatial-only DataView has spatial axis");
                t.expect(dv.axis(AxisKind::Time) == nullptr, "spatial-only DataView has no time axis definition");
                t.expect(dv.axis(AxisKind::Spatial) != nullptr, "spatial-only DataView resolves spatial axis definition");
                t.expect(dv.time().empty(), "spatial-only DataView compatibility time() is empty");
                t.expect(bundle->time_axis().empty(), "spatial-only AlignedBundle time_axis is empty");
                t.expect(dv.time_axis().empty(), "spatial-only DataView semantic time_axis is empty");

                std::ostringstream temporal_log;
                BacktestResult temporal_out{};
                const bool temporal_ok = run_custom_plan_spec(dv, primary, temporal_local_spec, opt, temporal_log, &temporal_out);
                t.expect(!temporal_ok, "temporal-local op contract rejects spatial-only DataView");
                t.expect(temporal_log.str().find("required axis 'time'") != std::string::npos,
                    "temporal-local rejection reports missing time axis explicitly");

                std::ostringstream rowwise_log;
                BacktestResult rowwise_out{};
                const bool rowwise_ok = run_custom_plan_spec(dv, primary, rowwise_spec, opt, rowwise_log, &rowwise_out);
                t.expect(rowwise_ok,
                    "row-wise contract plan executes on spatial-only DataView after operator-axis cleanup");
                t.expect(rowwise_log.str().find("required axis 'time'") == std::string::npos,
                    "row-wise spatial-only execution no longer reports a missing time axis");
                t.expect(!rowwise_out.has_best_start_date,
                    "spatial-only row-wise execution omits best_start_date without time axis");
            }
        }

        {
            std::shared_ptr<align::AlignedBundle> bundle;
            ColumnRef primary{};
            const bool okdv = make_single_series_dataview_with_index_schema(
                close, make_time_spatial_index_schema(), bundle, primary, static_cast<TimeKey>(20240101), spatial_x, spatial_y);
            t.expect(okdv && bundle != nullptr, "spatio-temporal fixture creates DataView input");
            if (okdv && bundle) {
                const DataView dv(*bundle);
                t.expect(dv.row_count() == close.size(), "spatio-temporal DataView row_count follows observation domain");
                t.expect(bundle->row_count() == close.size(), "spatio-temporal AlignedBundle row_count follows observation domain");
                t.expect(dv.index_schema() != nullptr, "spatio-temporal DataView exposes index_schema");
                t.expect(dv.has_axis(AxisKind::Time), "spatio-temporal DataView has time axis");
                t.expect(dv.has_axis(AxisKind::Spatial), "spatio-temporal DataView has spatial axis");
                t.expect(dv.axis(AxisKind::Time) != nullptr, "spatio-temporal DataView resolves time axis definition");
                t.expect(dv.axis(AxisKind::Spatial) != nullptr, "spatio-temporal DataView resolves spatial axis definition");
                t.expect(bundle->time_axis().size() == close.size(), "spatio-temporal AlignedBundle exposes time_axis values");
                t.expect(dv.time().size() == close.size(), "spatio-temporal DataView compatibility time() follows semantic time axis");
                t.expect(dv.time_axis().size() == close.size(), "spatio-temporal DataView exposes semantic time_axis values");

                std::ostringstream mixed_log;
                BacktestResult mixed_out{};
                const bool mixed_ok = run_custom_plan_spec(dv, primary, mixed_axis_spec, opt, mixed_log, &mixed_out);
                t.expect(mixed_ok, "mixed-axis contract plan executes when DataView has both time and spatial axes");
            }
        }

        return true;
    }

    bool test_planspec_axis_validation(ContractAsserts& t) {
        t.out << "[axis_validation] PlanSpec axis validation against DataView index_schema\n";

        const std::vector<double> close = {
            100.0, 101.0, 102.0, 103.0, 104.0, 105.0, 106.0, 107.0
        };
        std::shared_ptr<align::AlignedBundle> bundle;
        ColumnRef primary{};
        const bool okdv = make_single_series_dataview_from_close(close, bundle, primary);
        t.expect(okdv && bundle != nullptr, "axis validation test creates DataView input");
        if (!okdv || !bundle) return true;

        PlanSpec spec{};
        spec.version = 1;
        spec.name = "contract_axis_validation";
        spec.trade_mode = TradeMode::LongOnly;

        NodeSpec n_close{};
        n_close.id = "c";
        n_close.op = "core.close";

        NodeSpec n_zero{};
        n_zero.id = "z";
        n_zero.op = "core.const";
        n_zero.params["value"] = 0.0;

        spec.numeric_nodes = { n_close, n_zero };

        NodeSpec b_entry{};
        b_entry.id = "entry";
        b_entry.op = "cmp.gt";
        b_entry.args = { "c", "z" };

        NodeSpec b_exit{};
        b_exit.id = "exit";
        b_exit.op = "cmp.lt";
        b_exit.args = { "c", "z" };

        spec.bool_nodes = { b_entry, b_exit };
        spec.bind.entry_long = "entry";
        spec.bind.exit_long = "exit";

        PlanSweepRuntimeOptions opt{};
        BacktestResult out_ok{};
        std::ostringstream ok_log;
        const DataView dv_ok(*bundle);
        const bool ran_ok = run_custom_plan_spec(dv_ok, primary, spec, opt, ok_log, &out_ok);
        t.expect(ran_ok, "axis validation passes for row-wise plan when DataView has index_schema");

        auto broken = std::make_shared<align::AlignedBundle>(*bundle);
        broken->index_schema.reset();
        const DataView dv_bad(*broken);
        std::ostringstream bad_log;
        BacktestResult out_bad{};
        const bool ran_bad = run_custom_plan_spec(dv_bad, primary, spec, opt, bad_log, &out_bad);
        t.expect(ran_bad, "axis validation allows row-wise plan when DataView index_schema is missing");

        PlanSpec temporal_spec = spec;
        temporal_spec.name = "contract_axis_validation_temporal";
        NodeSpec n_sma{};
        n_sma.id = "s";
        n_sma.op = "ts.sma";
        n_sma.args = { "c" };
        n_sma.params["n"] = static_cast<std::int64_t>(3);
        temporal_spec.numeric_nodes = { n_sma, n_close, n_zero };
        NodeSpec tb_entry{};
        tb_entry.id = "t_entry";
        tb_entry.op = "cmp.gt";
        tb_entry.args = { "s", "z" };
        NodeSpec tb_exit{};
        tb_exit.id = "t_exit";
        tb_exit.op = "cmp.lt";
        tb_exit.args = { "s", "z" };
        temporal_spec.bool_nodes = { tb_entry, tb_exit };
        temporal_spec.bind.entry_long = "t_entry";
        temporal_spec.bind.exit_long = "t_exit";

        std::ostringstream temporal_ok_log;
        BacktestResult temporal_ok_out{};
        const bool temporal_ran_ok = run_custom_plan_spec(dv_ok, primary, temporal_spec, opt, temporal_ok_log, &temporal_ok_out);
        t.expect(temporal_ran_ok, "axis validation passes for temporal-local plan when DataView has time axis");

        std::ostringstream temporal_bad_log;
        BacktestResult temporal_bad_out{};
        const bool temporal_ran_bad = run_custom_plan_spec(dv_bad, primary, temporal_spec, opt, temporal_bad_log, &temporal_bad_out);
        t.expect(!temporal_ran_bad, "axis validation fails for temporal-local plan when DataView index_schema is missing");
        const std::string temporal_msg = temporal_bad_log.str();
        t.expect(temporal_msg.find("required axis 'time'") != std::string::npos,
            "temporal-local validation failure reports missing time axis");

        PlanSpec spatial_spec = spec;
        spatial_spec.name = "contract_axis_validation_spatial";
        NodeSpec n_spatial{};
        n_spatial.id = "s";
        n_spatial.op = "spatial.identity";
        n_spatial.args = { "c" };
        spatial_spec.numeric_nodes.push_back(n_spatial);
        spatial_spec.bool_nodes.clear();

        NodeSpec sb_entry{};
        sb_entry.id = "s_entry";
        sb_entry.op = "cmp.gt";
        sb_entry.args = { "s", "z" };
        NodeSpec sb_exit{};
        sb_exit.id = "s_exit";
        sb_exit.op = "cmp.lt";
        sb_exit.args = { "s", "z" };
        spatial_spec.bool_nodes = { sb_entry, sb_exit };
        spatial_spec.bind.entry_long = "s_entry";
        spatial_spec.bind.exit_long = "s_exit";

        std::ostringstream spatial_log;
        BacktestResult spatial_out{};
        const bool spatial_ok = run_custom_plan_spec(dv_ok, primary, spatial_spec, opt, spatial_log, &spatial_out);
        t.expect(!spatial_ok, "spatial op is rejected on non-spatial DataView");
        const std::string spatial_msg = spatial_log.str();
        t.expect(spatial_msg.find("required axis 'spatial'") != std::string::npos,
            "spatial-axis validation failure is explicit");

        PlanSpec st_spec = spec;
        st_spec.name = "contract_axis_validation_spatiotemporal";
        NodeSpec n_st{};
        n_st.id = "st";
        n_st.op = "st.radius_mean";
        n_st.args = { "c" };
        n_st.params["last_n"] = static_cast<std::int64_t>(2);
        n_st.params["radius"] = 1.0;
        st_spec.numeric_nodes.push_back(n_st);
        st_spec.bool_nodes.clear();

        NodeSpec stb_entry{};
        stb_entry.id = "st_entry";
        stb_entry.op = "cmp.gt";
        stb_entry.args = { "st", "z" };
        NodeSpec stb_exit{};
        stb_exit.id = "st_exit";
        stb_exit.op = "cmp.lt";
        stb_exit.args = { "st", "z" };
        st_spec.bool_nodes = { stb_entry, stb_exit };
        st_spec.bind.entry_long = "st_entry";
        st_spec.bind.exit_long = "st_exit";

        std::ostringstream st_log;
        BacktestResult st_out{};
        const bool st_ok = run_custom_plan_spec(dv_ok, primary, st_spec, opt, st_log, &st_out);
        t.expect(!st_ok, "spatio-temporal op is rejected on non-spatial DataView");
        const std::string st_msg = st_log.str();
        t.expect(st_msg.find("required axis 'spatial'") != std::string::npos,
            "spatio-temporal validation failure reports missing spatial axis explicitly");

        return true;
    }

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
