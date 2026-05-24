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

} // namespace sugar::contract_suite_internal
