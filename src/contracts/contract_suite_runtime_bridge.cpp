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
#include "sweep/sweep.h"
#include "engine/plan_compiler.h"
#include "sweep/plan_sweep.h"
#include "engine/program_evaluator.h"
#include "ingest/utils.h"
#include "ingest/run_manifest.h"

namespace sugar::contract_suite_internal {

    bool test_compile_bridge_design_load_contracts(ContractAsserts& t) {
        t.out << "[compile_bridge_design_loads] current compile-bridge runtime design loads\n";

        PlanSweepRuntimeOptions opt{};

        {
            const std::vector<double> spatial_close = { 10.0, 20.0, 30.0 };
            const std::vector<double> spatial_x = { 0.0, 1.0, 10.0 };
            const std::vector<double> spatial_y = { 0.0, 0.0, 0.0 };
            std::shared_ptr<align::AlignedBundle> spatial_bundle;
            ColumnRef spatial_primary{};
            const bool spatial_dv_ok = make_single_series_dataview_with_index_schema(
                spatial_close,
                make_spatial_only_index_schema(),
                spatial_bundle,
                spatial_primary,
                static_cast<TimeKey>(0),
                spatial_x,
                spatial_y);
            t.expect(spatial_dv_ok && spatial_bundle != nullptr,
                "compile-bridge design-load spatial fixture creates spatial-only DataView input");
            if (spatial_dv_ok && spatial_bundle) {
                auto spatial_spec = make_spatial_radius_mean_plan(
                    "contract_compile_bridge_design_spatial_runtime",
                    1.1,
                    14.0,
                    std::string(builtin_spatial_topology_name()));
                add_standard_exit_binding(spatial_spec);

                const DataView spatial_dv(*spatial_bundle);
                std::ostringstream spatial_log;
                BacktestResult spatial_run{};
                const bool spatial_ok = run_custom_plan_spec(
                    spatial_dv, spatial_primary, spatial_spec, opt, spatial_log, &spatial_run);
                t.expect(spatial_ok,
                    "compile-bridge design-load executes spatial.radius_mean plan on spatial-only DataView");
                if (!spatial_ok) {
                    t.out << "    compile-bridge design-load spatial runtime error: " << spatial_log.str() << "\n";
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
            std::shared_ptr<align::AlignedBundle> multi_bundle;
            ColumnRef multi_primary{};
            const bool multi_dv_ok = make_two_series_dataview_with_index_schema(
                primary_close,
                secondary_close,
                make_time_spatial_index_schema(),
                multi_bundle,
                multi_primary,
                static_cast<TimeKey>(20240101),
                primary_x,
                primary_y,
                secondary_x,
                secondary_y);
            t.expect(multi_dv_ok && multi_bundle != nullptr,
                "compile-bridge design-load locality-subject fixture creates aligned DataView input");
            if (multi_dv_ok && multi_bundle) {
                auto default_spec = make_spatial_radius_mean_plan(
                    "contract_compile_bridge_design_subject_default",
                    0.2,
                    50.0,
                    std::string(builtin_spatial_topology_name()));
                add_standard_exit_binding(default_spec);
                auto subject_spec = make_spatial_radius_mean_plan(
                    "contract_compile_bridge_design_subject_secondary",
                    0.2,
                    50.0,
                    std::string(builtin_spatial_topology_name()),
                    std::string("B"));
                add_standard_exit_binding(subject_spec);

                const DataView multi_dv(*multi_bundle);
                std::ostringstream default_log;
                std::ostringstream subject_log;
                BacktestResult default_run{};
                BacktestResult subject_run{};
                const bool default_ok = run_custom_plan_spec(
                    multi_dv, multi_primary, default_spec, opt, default_log, &default_run);
                const bool subject_ok = run_custom_plan_spec(
                    multi_dv, multi_primary, subject_spec, opt, subject_log, &subject_run);
                t.expect(default_ok,
                    "compile-bridge design-load default locality-subject plan executes through bridge");
                t.expect(subject_ok,
                    "compile-bridge design-load explicit locality-subject plan executes through bridge");
                if (!default_ok) {
                    t.out << "    compile-bridge design-load default-subject runtime error: " << default_log.str() << "\n";
                }
                if (!subject_ok) {
                    t.out << "    compile-bridge design-load explicit-subject runtime error: " << subject_log.str() << "\n";
                }
                if (default_ok && subject_ok) {
                    t.expect(default_run.trades != subject_run.trades ||
                            default_run.trade_hash != subject_run.trade_hash ||
                            !nearly_equal(default_run.pnl, subject_run.pnl),
                        "compile-bridge design-load explicit subject_series changes bridged execution outcome");
                }
            }
        }

        {
            const std::vector<double> st_close = { 10.0, 20.0, 30.0, 40.0 };
            const std::vector<double> st_x = { 0.0, 0.4, 10.0, 10.4 };
            const std::vector<double> st_y = { 0.0, 0.0, 0.0, 0.0 };
            std::shared_ptr<align::AlignedBundle> st_bundle;
            ColumnRef st_primary{};
            const bool st_dv_ok = make_single_series_dataview_with_index_schema(
                st_close,
                make_time_spatial_index_schema(),
                st_bundle,
                st_primary,
                static_cast<TimeKey>(20240101),
                st_x,
                st_y);
            t.expect(st_dv_ok && st_bundle != nullptr,
                "compile-bridge design-load spatiotemporal fixture creates DataView input");
            if (st_dv_ok && st_bundle) {
                auto st_spec = make_spatiotemporal_radius_mean_plan(
                    "contract_compile_bridge_design_st_runtime",
                    2,
                    0.5,
                    20.0,
                    std::string(builtin_spatiotemporal_topology_name()));
                add_standard_exit_binding(st_spec);

                const DataView st_dv(*st_bundle);
                std::ostringstream st_log;
                BacktestResult st_run{};
                const bool st_ok = run_custom_plan_spec(
                    st_dv, st_primary, st_spec, opt, st_log, &st_run);
                t.expect(st_ok,
                    "compile-bridge design-load executes st.radius_mean plan on spatio-temporal DataView");
                if (!st_ok) {
                    t.out << "    compile-bridge design-load spatio-temporal runtime error: " << st_log.str() << "\n";
                }
            }
        }

        return true;
    }

    bool test_compile_bridge_overload_rejection_contracts(ContractAsserts& t) {
        t.out << "[compile_bridge_overload_rejection] current compile-bridge runtime overload rejection contracts\n";

        PlanSweepRuntimeOptions opt{};

        {
            const std::vector<double> spatial_close = { 10.0, 20.0, 30.0 };
            const std::vector<double> spatial_x = { 0.0, 1.0, 10.0 };
            const std::vector<double> spatial_y = { 0.0, 0.0, 0.0 };
            std::shared_ptr<align::AlignedBundle> spatial_bundle;
            ColumnRef spatial_primary{};
            const bool spatial_dv_ok = make_single_series_dataview_with_index_schema(
                spatial_close,
                make_spatial_only_index_schema(),
                spatial_bundle,
                spatial_primary,
                static_cast<TimeKey>(0),
                spatial_x,
                spatial_y);
            t.expect(spatial_dv_ok && spatial_bundle != nullptr,
                "compile-bridge overload spatial fixture creates spatial-only DataView input");
            if (spatial_dv_ok && spatial_bundle) {
                auto spatial_bad_topology = make_spatial_radius_mean_plan(
                    "contract_compile_bridge_overload_spatial_bad_topology",
                    1.1,
                    14.0,
                    "missing.topology");
                add_standard_exit_binding(spatial_bad_topology);

                const DataView spatial_dv(*spatial_bundle);
                std::ostringstream spatial_bad_log;
                BacktestResult spatial_bad_run{};
                const bool spatial_bad_ok = run_custom_plan_spec(
                    spatial_dv, spatial_primary, spatial_bad_topology, opt, spatial_bad_log, &spatial_bad_run);
                t.expect(!spatial_bad_ok,
                    "compile-bridge overload rejects unavailable named topology for spatial.radius_mean");
                t.expect(spatial_bad_log.str().find("required topology 'missing.topology' is not available on DataView") != std::string::npos,
                    "compile-bridge overload preserves missing named topology reason");

                auto spatial_bad_subject = make_spatial_radius_mean_plan(
                    "contract_compile_bridge_overload_spatial_bad_subject",
                    1.1,
                    14.0,
                    std::string(builtin_spatial_topology_name()),
                    std::string("missing.series"));
                add_standard_exit_binding(spatial_bad_subject);

                std::ostringstream spatial_bad_subject_log;
                BacktestResult spatial_bad_subject_run{};
                const bool spatial_bad_subject_ok = run_custom_plan_spec(
                    spatial_dv, spatial_primary, spatial_bad_subject, opt, spatial_bad_subject_log, &spatial_bad_subject_run);
                t.expect(!spatial_bad_subject_ok,
                    "compile-bridge overload rejects unresolved locality subject for spatial.radius_mean");
                const std::string spatial_bad_subject_msg = spatial_bad_subject_log.str();
                t.expect(spatial_bad_subject_msg.find("locality subject could not be resolved") != std::string::npos,
                    "compile-bridge overload preserves locality-subject resolution failure");
                t.expect(spatial_bad_subject_msg.find("series not found") != std::string::npos,
                    "compile-bridge overload preserves missing-series detail");
            }
        }

        {
            const std::vector<double> st_close = { 10.0, 20.0, 30.0, 40.0 };
            const std::vector<double> st_x = { 0.0, 0.4, 10.0, 10.4 };
            const std::vector<double> st_y = { 0.0, 0.0, 0.0, 0.0 };
            std::shared_ptr<align::AlignedBundle> st_bundle;
            ColumnRef st_primary{};
            const bool st_dv_ok = make_single_series_dataview_with_index_schema(
                st_close,
                make_time_spatial_index_schema(),
                st_bundle,
                st_primary,
                static_cast<TimeKey>(20240101),
                st_x,
                st_y);
            t.expect(st_dv_ok && st_bundle != nullptr,
                "compile-bridge overload spatiotemporal fixture creates DataView input");
            if (st_dv_ok && st_bundle) {
                auto st_bad_topology = make_spatiotemporal_radius_mean_plan(
                    "contract_compile_bridge_overload_st_bad_topology",
                    2,
                    0.5,
                    20.0,
                    std::string(builtin_temporal_topology_name()));
                add_standard_exit_binding(st_bad_topology);

                const DataView st_dv(*st_bundle);
                std::ostringstream st_bad_log;
                BacktestResult st_bad_run{};
                const bool st_bad_ok = run_custom_plan_spec(
                    st_dv, st_primary, st_bad_topology, opt, st_bad_log, &st_bad_run);
                t.expect(!st_bad_ok,
                    "compile-bridge overload rejects topology family mismatch for st.radius_mean");
                const std::string st_bad_msg = st_bad_log.str();
                t.expect(st_bad_msg.find("has family 'temporal_window'") != std::string::npos,
                    "compile-bridge overload preserves actual mismatched topology family");
                t.expect(st_bad_msg.find("spatiotemporal_composite") != std::string::npos,
                    "compile-bridge overload preserves required topology family");
            }
        }

        {
            std::vector<double> close;
            close.reserve(16);
            for (std::size_t i = 0; i < 16; ++i) {
                close.push_back(100.0 + static_cast<double>(i));
            }
            std::shared_ptr<align::AlignedBundle> bundle;
            ColumnRef primary{};
            const bool okdv = make_single_series_dataview_from_close(close, bundle, primary);
            t.expect(okdv && bundle != nullptr,
                "compile-bridge overload unsupported-op fixture creates DataView input");
            if (okdv && bundle) {
                PlanSpec bad{};
                bad.version = 1;
                bad.name = "contract_compile_bridge_overload_bad_op";
                bad.trade_mode = TradeMode::LongOnly;

                NodeSpec n_close{};
                n_close.id = "c";
                n_close.op = "core.close";

                NodeSpec n_mid{};
                n_mid.id = "mid";
                n_mid.op = "bad.unsupported_op";
                n_mid.args = { "c" };

                NodeSpec n_thr{};
                n_thr.id = "thr";
                n_thr.op = "core.const";
                n_thr.params["value"] = 100.0;

                bad.numeric_nodes = { n_close, n_mid, n_thr };

                NodeSpec bad_entry{};
                bad_entry.id = "entry_bad";
                bad_entry.op = "cmp.gt";
                bad_entry.args = { "c", "thr" };

                NodeSpec bad_exit{};
                bad_exit.id = "exit_bad";
                bad_exit.op = "cmp.lt";
                bad_exit.args = { "c", "thr" };

                bad.bool_nodes = { bad_entry, bad_exit };
                bad.bind.entry_long = "entry_bad";
                bad.bind.exit_long = "exit_bad";

                const DataView dv(*bundle);
                std::ostringstream bad_log;
                BacktestResult bad_r{};
                const bool bad_ok = run_custom_plan_spec(dv, primary, bad, opt, bad_log, &bad_r);
                t.expect(!bad_ok,
                    "compile-bridge overload hard-fails when compile bridge cannot compile node op");
                const std::string bad_msg = bad_log.str();
                const bool explicit_fail_msg =
                    (bad_msg.find("PlanSpec compile failed:") != std::string::npos) ||
                    (bad_msg.find("PlanSpec runtime validation failed:") != std::string::npos);
                t.expect(explicit_fail_msg,
                    "compile-bridge overload keeps compile/validation failure explicit");
                t.expect(bad_msg.find("Unsupported numeric op in multi-series mode") == std::string::npos,
                    "compile-bridge overload does not regress to legacy fallback evaluator errors");
            }
        }

        return true;
    }
bool test_operator_axis_contracts(ContractAsserts& t) {
        t.out << "[operator_axes] operator axis-contract classification\n";

        const auto reg = make_default_registry();

        auto expect_op = [&](const char* name,
                             OpAxisScope scope,
                             std::vector<AxisKind> required,
                             const char* label) {
            const auto* op = reg.get(name);
            t.expect(op != nullptr, std::string(label) + " is registered");
            if (!op) return;
            t.expect(op->spec.axis_scope == scope, std::string(label) + " has expected axis_scope");
            t.expect(op->spec.required_axes == required, std::string(label) + " has expected required_axes");
        };

        auto expect_topology = [&](const char* name,
                                   std::vector<TopologyFamily> families,
                                   std::vector<TopologyQueryKind> queries,
                                   std::string default_topology,
                                   std::string binding_param,
                                   const char* label) {
            const auto* op = reg.get(name);
            t.expect(op != nullptr, std::string(label) + " is registered for topology hooks");
            if (!op) return;
            t.expect(op->spec.required_topology_families == families,
                std::string(label) + " has expected required_topology_families");
            t.expect(op->spec.required_topology_query_kinds == queries,
                std::string(label) + " has expected required_topology_query_kinds");
            t.expect(op->spec.default_topology_name == default_topology,
                std::string(label) + " has expected default_topology_name");
            t.expect(op->spec.topology_binding_param == binding_param,
                std::string(label) + " has expected topology_binding_param");
        };
        auto expect_locality_subject = [&](const char* name,
                                           LocalitySubjectKind default_kind,
                                           std::string default_value,
                                           LocalitySubjectKind bound_kind,
                                           std::string binding_param,
                                           const char* label) {
            const auto* op = reg.get(name);
            t.expect(op != nullptr, std::string(label) + " is registered for locality-subject hooks");
            if (!op) return;
            t.expect(op->spec.default_locality_subject_kind == default_kind,
                std::string(label) + " has expected default_locality_subject_kind");
            t.expect(op->spec.default_locality_subject_value == default_value,
                std::string(label) + " has expected default_locality_subject_value");
            t.expect(op->spec.bound_locality_subject_kind == bound_kind,
                std::string(label) + " has expected bound_locality_subject_kind");
            t.expect(op->spec.locality_subject_binding_param == binding_param,
                std::string(label) + " has expected locality_subject_binding_param");
        };

        expect_op("core.close", OpAxisScope::RowWise, {}, "core.close");
        expect_op("core.const", OpAxisScope::RowWise, {}, "core.const");
        expect_op("core.ref", OpAxisScope::RowWise, {}, "core.ref");
        expect_op("core.select", OpAxisScope::RowWise, {}, "core.select");
        expect_op("core.bool_const", OpAxisScope::RowWise, {}, "core.bool_const");
        expect_op("logic.and", OpAxisScope::RowWise, {}, "logic.and");
        expect_op("logic.not", OpAxisScope::RowWise, {}, "logic.not");
        expect_op("cmp.gt", OpAxisScope::RowWise, {}, "cmp.gt");
        expect_op("cmp.cross_up", OpAxisScope::TimeSeries, {AxisKind::Time}, "cmp.cross_up");
        expect_op("cmp.cross_down", OpAxisScope::TimeSeries, {AxisKind::Time}, "cmp.cross_down");
        expect_op("ts.sma", OpAxisScope::TimeSeries, {AxisKind::Time}, "ts.sma");
        expect_op("ts.percentile", OpAxisScope::TimeSeries, {AxisKind::Time}, "ts.percentile");
        expect_op("ts.entropy", OpAxisScope::TimeSeries, {AxisKind::Time}, "ts.entropy");
        expect_op("ts.autocorrelation", OpAxisScope::TimeSeries, {AxisKind::Time}, "ts.autocorrelation");
        expect_op("ts.hurst", OpAxisScope::TimeSeries, {AxisKind::Time}, "ts.hurst");
        expect_op("ts.macd", OpAxisScope::TimeSeries, {AxisKind::Time}, "ts.macd");
        expect_op("ts.bollinger_width", OpAxisScope::TimeSeries, {AxisKind::Time}, "ts.bollinger_width");
        expect_op("ts.regime_change", OpAxisScope::TimeSeries, {AxisKind::Time}, "ts.regime_change");
        expect_op("ts.volatility_ratio", OpAxisScope::TimeSeries, {AxisKind::Time}, "ts.volatility_ratio");
        expect_op("ts.neg", OpAxisScope::RowWise, {}, "ts.neg");
        expect_op("ts.add", OpAxisScope::RowWise, {}, "ts.add");
        expect_op("ts.clamp", OpAxisScope::RowWise, {}, "ts.clamp");
        expect_op("ts.ratio_of_ma", OpAxisScope::RowWise, {}, "ts.ratio_of_ma");
        expect_op("ts.true_range", OpAxisScope::TimeSeries, {AxisKind::Time}, "ts.true_range");
        expect_op("ms.diff", OpAxisScope::CrossSeries, {}, "ms.diff");
        expect_op("ms.ratio", OpAxisScope::CrossSeries, {}, "ms.ratio");
        expect_op("ms.zscore", OpAxisScope::TimeSeries, {AxisKind::Time}, "ms.zscore");
        expect_op("ms.correlation", OpAxisScope::TimeSeries, {AxisKind::Time}, "ms.correlation");
        expect_op("spatial.identity", OpAxisScope::SpatialLocal, {AxisKind::Spatial}, "spatial.identity");
        expect_op("spatial.radius_mean", OpAxisScope::SpatialLocal, {AxisKind::Spatial}, "spatial.radius_mean");
        expect_op("st.radius_mean", OpAxisScope::SpatioTemporal, {AxisKind::Time, AxisKind::Spatial}, "st.radius_mean");
        expect_topology("core.close", {}, {}, std::string{}, std::string{}, "core.close");
        expect_topology("spatial.identity", {}, {}, std::string{}, std::string{}, "spatial.identity");
        expect_topology("spatial.radius_mean",
            { TopologyFamily::SpatialMetric2D },
            { TopologyQueryKind::MetricRadius },
            std::string(builtin_spatial_topology_name()),
            std::string("topology"),
            "spatial.radius_mean");
        expect_topology("st.radius_mean",
            { TopologyFamily::SpatioTemporalComposite },
            { TopologyQueryKind::Intersection },
            std::string(builtin_spatiotemporal_topology_name()),
            std::string("topology"),
            "st.radius_mean");
        expect_locality_subject("core.close",
            LocalitySubjectKind::PrimarySeries,
            std::string{},
            LocalitySubjectKind::SeriesKey,
            std::string{},
            "core.close");
        expect_locality_subject("spatial.radius_mean",
            LocalitySubjectKind::PrimarySeries,
            std::string{},
            LocalitySubjectKind::SeriesKey,
            std::string("subject_series"),
            "spatial.radius_mean");
        expect_locality_subject("st.radius_mean",
            LocalitySubjectKind::PrimarySeries,
            std::string{},
            LocalitySubjectKind::SeriesKey,
            std::string("subject_series"),
            "st.radius_mean");

        {
            OpEntry subject_op{};
            subject_op.spec.name = "contract.locality_subject";
            subject_op.spec.default_locality_subject_kind = LocalitySubjectKind::PrimarySeries;
            subject_op.spec.bound_locality_subject_kind = LocalitySubjectKind::SeriesKey;
            subject_op.spec.locality_subject_binding_param = "subject_series";

            ResolvedLocalitySubject subject{};
            std::string err;
            ParamMap node_params{};
            ParamMap plan_params{};
            const bool default_ok = resolve_bound_locality_subject(
                subject_op, node_params, plan_params, nullptr, subject, err);
            t.expect(default_ok, "locality-subject resolver accepts missing explicit binding");
            t.expect(subject.kind == LocalitySubjectKind::PrimarySeries &&
                subject.value.empty() &&
                !subject.explicit_binding,
                "locality-subject resolver preserves primary-series default");

            node_params["subject_series"] = std::string("BBB");
            const bool literal_ok = resolve_bound_locality_subject(
                subject_op, node_params, plan_params, nullptr, subject, err);
            t.expect(literal_ok, "locality-subject resolver accepts literal subject binding");
            t.expect(subject.kind == LocalitySubjectKind::SeriesKey &&
                subject.value == "BBB" &&
                subject.explicit_binding,
                "locality-subject resolver upgrades to explicit series-key binding");

            node_params["subject_series"] = ParamRef{ "subject_param" };
            plan_params["subject_param"] = std::string("CCC");
            const bool ref_ok = resolve_bound_locality_subject(
                subject_op, node_params, plan_params, nullptr, subject, err);
            t.expect(ref_ok, "locality-subject resolver accepts param-ref subject binding");
            t.expect(subject.kind == LocalitySubjectKind::SeriesKey &&
                subject.value == "CCC" &&
                subject.explicit_binding,
                "locality-subject resolver resolves param-ref subject binding");

            ResolvedParamMap resolved_params{};
            resolved_params.emplace("subject_series", std::string("DDD"));
            const bool resolved_ok = resolve_bound_locality_subject(subject_op, resolved_params, subject, err);
            t.expect(resolved_ok, "locality-subject resolver accepts resolved-param subject binding");
            t.expect(subject.kind == LocalitySubjectKind::SeriesKey &&
                subject.value == "DDD" &&
                subject.explicit_binding,
                "locality-subject resolver preserves resolved-param explicit subject");
        }

        return true;
    }


bool test_compile_bridge_registry_surface_contracts(ContractAsserts& t) {
        t.out << "[compile_bridge_registry_surface] compile-bridge registry surface contracts\n";

        const auto reg = make_default_registry();
        const std::vector<std::string> ts_phase1_ops = {
            "core.select",
            "ms.correlation",
            "ts.abs",
            "ts.add",
            "ts.atr",
            "ts.atr_wilder",
            "ts.autocorrelation",
            "ts.bollinger_width",
            "ts.clamp",
            "ts.clamp_const",
            "ts.delta",
            "ts.donchian_lower",
            "ts.donchian_mid",
            "ts.donchian_upper",
            "ts.donchian_width",
            "ts.div",
            "ts.ema",
            "ts.entropy",
            "ts.exp",
            "ts.hurst",
            "ts.log",
            "ts.macd",
            "ts.max",
            "ts.min",
            "ts.mul",
            "ts.neg",
            "ts.normalize_minmax",
            "ts.normalize_minmax_global",
            "ts.percentile",
            "ts.pow_const",
            "ts.ratio_of_ma",
            "ts.regime_change",
            "ts.roc",
            "ts.roll_max",
            "ts.roll_min",
            "ts.rsi",
            "ts.shift",
            "ts.sma",
            "ts.stddev",
            "ts.stddev_ema",
            "ts.sqrt",
            "ts.sub",
            "ts.true_range",
            "ts.true_range_ohlc",
            "ts.volatility_ratio",
            "ts.zscore_ema",
            "ts.zscore_sma",
            "spatial.radius_mean",
            "st.radius_mean",
        };
        for (const auto& op : ts_phase1_ops) {
            t.expect(reg.has(op), "registry exposes " + op);
        }

        return true;
    }

    bool test_compile_bridge_temporal_runtime_contracts(ContractAsserts& t) {
        t.out << "[compile_bridge_temporal_runtime] compile-bridge temporal runtime contracts\n";

        std::vector<double> close;
        close.reserve(16);
        for (std::size_t i = 0; i < 16; ++i) {
            close.push_back(100.0 + static_cast<double>(i));
        }
        std::shared_ptr<align::AlignedBundle> bundle;
        ColumnRef primary{};
        const bool okdv = make_single_series_dataview_from_close(close, bundle, primary);
        t.expect(okdv && bundle != nullptr, "compile-bridge test creates DataView input");
        if (!okdv || !bundle) return true;

        PlanSpec spec{};
        spec.version = 1;
        spec.name = "contract_compile_bridge_runtime";
        spec.trade_mode = TradeMode::LongOnly;

        NodeSpec n_close{};
        n_close.id = "c";
        n_close.op = "core.close";

        NodeSpec n_sma{};
        n_sma.id = "sma";
        n_sma.op = "ts.sma";
        n_sma.args = { "c" };
        n_sma.params["n"] = static_cast<std::int64_t>(3);

        NodeSpec n_thr{};
        n_thr.id = "thr";
        n_thr.op = "core.const";
        n_thr.params["value"] = 100.0;

        spec.numeric_nodes = { n_close, n_sma, n_thr };

        NodeSpec b_entry{};
        b_entry.id = "entry";
        b_entry.op = "cmp.gt";
        b_entry.args = { "sma", "thr" };

        NodeSpec b_exit{};
        b_exit.id = "exit";
        b_exit.op = "cmp.lt";
        b_exit.args = { "sma", "thr" };

        spec.bool_nodes = { b_entry, b_exit };
        spec.bind.entry_long = "entry";
        spec.bind.exit_long = "exit";

        PlanSweepRuntimeOptions opt{};
        const DataView dv(*bundle);
        std::ostringstream run_log;
        BacktestResult r{};
        const bool ok = run_custom_plan_spec(dv, primary, spec, opt, run_log, &r);
        t.expect(ok, "custom PlanSpec runtime executes ts.sma plan via compile bridge");
        if (!ok) {
            t.out << "    compile-bridge runtime error: " << run_log.str() << "\n";
        }

        return true;
    }

    bool test_compile_bridge_spatial_runtime_contracts(ContractAsserts& t) {
        t.out << "[compile_bridge_spatial_runtime] compile-bridge spatial runtime contracts\n";

        const std::vector<double> spatial_close = { 10.0, 20.0, 30.0 };
        const std::vector<double> spatial_x = { 0.0, 1.0, 10.0 };
        const std::vector<double> spatial_y = { 0.0, 0.0, 0.0 };
        std::shared_ptr<align::AlignedBundle> spatial_bundle;
        ColumnRef spatial_primary{};
        const bool spatial_dv_ok = make_single_series_dataview_with_index_schema(
            spatial_close,
            make_spatial_only_index_schema(),
            spatial_bundle,
            spatial_primary,
            static_cast<TimeKey>(0),
            spatial_x,
            spatial_y);
        t.expect(spatial_dv_ok && spatial_bundle != nullptr,
            "compile-bridge spatial-op test creates spatial-only DataView input");
        if (!spatial_dv_ok || !spatial_bundle) return true;

        PlanSpec spatial_spec = make_spatial_radius_mean_plan(
            "contract_compile_bridge_spatial_runtime",
            1.1,
            14.0,
            std::string(builtin_spatial_topology_name()));
        add_standard_exit_binding(spatial_spec);

        PlanSweepRuntimeOptions opt{};
        const DataView spatial_dv(*spatial_bundle);
        std::ostringstream spatial_log;
        BacktestResult spatial_run{};
        const bool spatial_ok = run_custom_plan_spec(
            spatial_dv, spatial_primary, spatial_spec, opt, spatial_log, &spatial_run);
        t.expect(spatial_ok,
            "custom PlanSpec runtime executes spatial.radius_mean plan on spatial-only DataView");
        if (!spatial_ok) {
            t.out << "    compile-bridge spatial runtime error: " << spatial_log.str() << "\n";
        }

        return true;
    }

    bool test_compile_bridge_subject_routing_contracts(ContractAsserts& t) {
        t.out << "[compile_bridge_subject_routing] compile-bridge locality subject-routing contracts\n";

        const std::vector<double> primary_close = { 1.0, 100.0, 1.0 };
        const std::vector<double> secondary_close = { 0.0, 0.0, 0.0 };
        const std::vector<double> primary_x = { 0.0, 0.1, 10.0 };
        const std::vector<double> primary_y = { 0.0, 0.0, 0.0 };
        const std::vector<double> secondary_x = { 0.0, 10.0, 10.1 };
        const std::vector<double> secondary_y = { 0.0, 0.0, 0.0 };
        std::shared_ptr<align::AlignedBundle> multi_bundle;
        ColumnRef multi_primary{};
        const bool multi_dv_ok = make_two_series_dataview_with_index_schema(
            primary_close,
            secondary_close,
            make_time_spatial_index_schema(),
            multi_bundle,
            multi_primary,
            static_cast<TimeKey>(20240101),
            primary_x,
            primary_y,
            secondary_x,
            secondary_y);
        t.expect(multi_dv_ok && multi_bundle != nullptr,
            "compile-bridge multi-series locality-subject test creates aligned DataView input");
        if (!multi_dv_ok || !multi_bundle) return true;

        PlanSpec default_spec = make_spatial_radius_mean_plan(
            "contract_compile_bridge_subject_default",
            0.2,
            50.0,
            std::string(builtin_spatial_topology_name()));
        add_standard_exit_binding(default_spec);

        PlanSpec subject_spec = make_spatial_radius_mean_plan(
            "contract_compile_bridge_subject_secondary",
            0.2,
            50.0,
            std::string(builtin_spatial_topology_name()),
            std::string("B"));
        add_standard_exit_binding(subject_spec);

        const auto registry = make_default_registry();
        CompileEnv env{};
        CompileResult default_compiled{};
        CompileResult subject_compiled{};
        std::string compile_err;
        t.expect(compile_plan(default_spec, registry, env, default_compiled, compile_err),
            "compile-bridge default locality-subject plan compiles");
        t.expect(compile_plan(subject_spec, registry, env, subject_compiled, compile_err),
            "compile-bridge explicit locality-subject plan compiles");

        const DataView multi_dv(*multi_bundle);
        ProgramSignalOutputs default_signals{};
        ProgramSignalOutputs subject_signals{};
        std::string eval_err;
        t.expect(evaluate_strategy_plan_program(
                multi_dv,
                multi_primary,
                default_compiled.plan,
                default_signals,
                eval_err),
            "compile-bridge default locality-subject plan evaluates");
        t.expect(evaluate_strategy_plan_program(
                multi_dv,
                multi_primary,
                subject_compiled.plan,
                subject_signals,
                eval_err),
            "compile-bridge explicit locality-subject plan evaluates");
        t.expect(!default_signals.entry_long.empty() && !subject_signals.entry_long.empty(),
            "compile-bridge locality-subject evaluation produces entry buffers");
        if (!default_signals.entry_long.empty() && !subject_signals.entry_long.empty()) {
            t.expect(default_signals.entry_long[0] == kBoolTrue,
                "default locality subject keeps first-row entry true on primary-series topology");
            t.expect(subject_signals.entry_long[0] == kBoolFalse,
                "explicit subject_series redirects topology lookup and changes first-row entry result");
        }

        return true;
    }

    bool test_compile_bridge_spatiotemporal_runtime_contracts(ContractAsserts& t) {
        t.out << "[compile_bridge_spatiotemporal_runtime] compile-bridge spatiotemporal runtime contracts\n";

        const std::vector<double> st_close = { 10.0, 20.0, 30.0, 40.0 };
        const std::vector<double> st_x = { 0.0, 0.4, 10.0, 10.4 };
        const std::vector<double> st_y = { 0.0, 0.0, 0.0, 0.0 };
        std::shared_ptr<align::AlignedBundle> st_bundle;
        ColumnRef st_primary{};
        const bool st_dv_ok = make_single_series_dataview_with_index_schema(
            st_close,
            make_time_spatial_index_schema(),
            st_bundle,
            st_primary,
            static_cast<TimeKey>(20240101),
            st_x,
            st_y);
        t.expect(st_dv_ok && st_bundle != nullptr,
            "compile-bridge spatio-temporal-op test creates spatio-temporal DataView input");
        if (!st_dv_ok || !st_bundle) return true;

        PlanSpec st_spec = make_spatiotemporal_radius_mean_plan(
            "contract_compile_bridge_st_runtime",
            2,
            0.5,
            20.0,
            std::string(builtin_spatiotemporal_topology_name()));
        st_spec.params["last_n"] = static_cast<std::int64_t>(2);
        st_spec.params["topology_name"] = std::string(builtin_spatiotemporal_topology_name());
        st_spec.numeric_nodes[1].params["last_n"] = ParamRef{ "last_n" };
        st_spec.numeric_nodes[1].params["topology"] = ParamRef{ "topology_name" };
        add_standard_exit_binding(st_spec);

        PlanSweepRuntimeOptions opt{};
        const DataView st_dv(*st_bundle);
        std::ostringstream st_log;
        BacktestResult st_run{};
        const bool st_ok = run_custom_plan_spec(
            st_dv, st_primary, st_spec, opt, st_log, &st_run);
        t.expect(st_ok,
            "custom PlanSpec runtime executes st.radius_mean plan on spatio-temporal DataView");
        if (!st_ok) {
            t.out << "    compile-bridge spatio-temporal runtime error: " << st_log.str() << "\n";
        }

        return true;
    }

    bool test_compile_bridge_sweep_runtime_contracts(ContractAsserts& t) {
        t.out << "[compile_bridge_sweep_runtime] compile-bridge sweep runtime contracts\n";

        {
            const auto direct = make_drange(0.0, 0.3, 0.1);
            t.expect(direct.size() == 4,
                "indexed make_drange preserves the inclusive decimal endpoint without epsilon guards");
            if (direct.size() == 4) {
                t.expect(nearly_equal(direct.front(), 0.0) && nearly_equal(direct.back(), 0.3),
                    "indexed make_drange preserves the expected start and stop values");
            }

            PlanSweepSpec sweep_cfg{};
            sweep_cfg.max_evals = 4;
            SweepParamRange alpha{};
            alpha.min = 0.0;
            alpha.max = 0.3;
            alpha.step = 0.1;
            sweep_cfg.params["alpha"] = alpha;

            PlanSpecSweepDims dims{};
            std::string err;
            const bool dims_ok = build_planspec_sweep_dims(sweep_cfg, dims, err);
            t.expect(dims_ok, "PlanSpec sweep dim expansion accepts the same inclusive decimal endpoint");
            if (dims_ok) {
                t.expect(dims.values.size() == 1 && dims.values.front().size() == 4,
                    "PlanSpec sweep dim expansion keeps the expected indexed cardinality");
                if (dims.values.size() == 1 && dims.values.front().size() == 4) {
                    t.expect(nearly_equal(dims.values.front().front(), 0.0) &&
                        nearly_equal(dims.values.front().back(), 0.3),
                        "PlanSpec sweep dim expansion preserves the expected start and stop values");
                }
            }

            sweep_cfg.max_evals = 3;
            dims = PlanSpecSweepDims{};
            err.clear();
            const bool uncapped_ok = build_planspec_sweep_dims(sweep_cfg, dims, err);
            t.expect(uncapped_ok, "PlanSpec sweep dim expansion stays valid when max_evals is smaller than the declared range");
            if (uncapped_ok) {
                t.expect(dims.values.size() == 1 && dims.values.front().size() == 4,
                    "PlanSpec sweep dim expansion preserves the full declared range instead of capping at max_evals");
            }
        }

        {
            const std::vector<double> st_close = { 10.0, 20.0, 30.0, 40.0 };
            const std::vector<double> st_x = { 0.0, 0.4, 10.0, 10.4 };
            const std::vector<double> st_y = { 0.0, 0.0, 0.0, 0.0 };
            std::shared_ptr<align::AlignedBundle> st_bundle;
            ColumnRef st_primary{};
            const bool st_dv_ok = make_single_series_dataview_with_index_schema(
                st_close,
                make_time_spatial_index_schema(),
                st_bundle,
                st_primary,
                static_cast<TimeKey>(20240101),
                st_x,
                st_y);
            t.expect(st_dv_ok && st_bundle != nullptr,
                "compile-bridge sweep test creates spatio-temporal DataView input");
            if (st_dv_ok && st_bundle) {
                PlanSpec st_sweep = make_spatiotemporal_radius_mean_plan(
                    "contract_compile_bridge_st_sweep_runtime",
                    2,
                    0.5,
                    20.0,
                    std::string(builtin_spatiotemporal_topology_name()));
                add_standard_exit_binding(st_sweep);

                PlanSweepSpec sweep_cfg{};
                sweep_cfg.enabled = true;
                sweep_cfg.engine = PlanSweepEngineKind::Exhaustive;
                sweep_cfg.top_k = 2;
                sweep_cfg.max_evals = 2;
                SweepParamRange last_n_range{};
                last_n_range.values = { 2.0, 3.0 };
                last_n_range.is_int = true;
                sweep_cfg.params["last_n"] = last_n_range;
                st_sweep.sweep = sweep_cfg;

                PlanSweepRuntimeOptions opt{};
                const DataView st_dv(*st_bundle);
                std::ostringstream sweep_log;
                BacktestResult sweep_best{};
                const bool sweep_ok = run_custom_plan_spec(
                    st_dv, st_primary, st_sweep, opt, sweep_log, &sweep_best);
                t.expect(sweep_ok,
                    "custom runtime executes exhaustive sweep for st.radius_mean through compiled-program template path");
                const std::string sweep_msg = sweep_log.str();
                t.expect(sweep_msg.find("Compile failures: 0") != std::string::npos,
                    "st.radius_mean sweep reports zero compile failures for valid param overrides");
            }
        }

        {
            std::vector<double> close;
            close.reserve(16);
            for (std::size_t i = 0; i < 16; ++i) {
                close.push_back(100.0 + static_cast<double>(i));
            }
            std::shared_ptr<align::AlignedBundle> bundle;
            ColumnRef primary{};
            const bool okdv = make_single_series_dataview_from_close(close, bundle, primary);
            t.expect(okdv && bundle != nullptr, "compile-bridge sweep test creates temporal DataView input");
            if (!okdv || !bundle) return true;

            PlanSpec sweep_spec{};
            sweep_spec.version = 1;
            sweep_spec.name = "contract_compile_bridge_sweep_runtime";
            sweep_spec.trade_mode = TradeMode::LongOnly;
            sweep_spec.params["n_fast"] = static_cast<std::int64_t>(2);

            NodeSpec sw_close{};
            sw_close.id = "c";
            sw_close.op = "core.close";

            NodeSpec sw_sma{};
            sw_sma.id = "sma";
            sw_sma.op = "ts.sma";
            sw_sma.args = { "c" };
            sw_sma.params["n"] = ParamRef{ "n_fast" };

            NodeSpec sw_thr{};
            sw_thr.id = "thr";
            sw_thr.op = "core.const";
            sw_thr.params["value"] = 102.5;

            sweep_spec.numeric_nodes = { sw_close, sw_sma, sw_thr };

            NodeSpec sw_entry{};
            sw_entry.id = "entry";
            sw_entry.op = "cmp.gt";
            sw_entry.args = { "sma", "thr" };

            NodeSpec sw_exit{};
            sw_exit.id = "exit";
            sw_exit.op = "cmp.lt";
            sw_exit.args = { "sma", "thr" };

            sweep_spec.bool_nodes = { sw_entry, sw_exit };
            sweep_spec.bind.entry_long = "entry";
            sweep_spec.bind.exit_long = "exit";

            PlanSweepSpec sweep_cfg{};
            sweep_cfg.enabled = true;
            sweep_cfg.engine = PlanSweepEngineKind::Exhaustive;
            sweep_cfg.top_k = 3;
            sweep_cfg.max_evals = 3;
            SweepParamRange n_fast_range{};
            n_fast_range.values = { 2.0, 3.0, 4.0 };
            n_fast_range.is_int = true;
            sweep_cfg.params["n_fast"] = n_fast_range;
            sweep_spec.sweep = sweep_cfg;

            PlanSweepRuntimeOptions opt{};
            const DataView dv(*bundle);
            BacktestResult manual_best{};
            double manual_best_score = -std::numeric_limits<double>::infinity();
            for (const std::int64_t n_fast : { static_cast<std::int64_t>(2), static_cast<std::int64_t>(3), static_cast<std::int64_t>(4) }) {
                PlanSpec single = sweep_spec;
                single.sweep.reset();
                single.params["n_fast"] = n_fast;

                std::ostringstream single_log;
                BacktestResult single_result{};
                const bool single_ok = run_custom_plan_spec(dv, primary, single, opt, single_log, &single_result);
                t.expect(single_ok, "manual sweep candidate compiles/runs for n_fast=" + std::to_string(n_fast));
                if (!single_ok) {
                    t.out << "    manual sweep candidate error (" << n_fast << "): " << single_log.str() << "\n";
                    continue;
                }
                if (single_result.pnl > manual_best_score) {
                    manual_best_score = single_result.pnl;
                    manual_best = single_result;
                }
            }

            std::ostringstream sweep_log;
            BacktestResult sweep_best{};
            const bool sweep_ok = run_custom_plan_spec(dv, primary, sweep_spec, opt, sweep_log, &sweep_best);
            t.expect(sweep_ok, "custom runtime executes exhaustive sweep through compiled-program template path");
            const std::string sweep_msg = sweep_log.str();
            t.expect(sweep_msg.find("Compile failures: 0") != std::string::npos,
                "sweep reports zero compile failures for valid param overrides");
            if (sweep_ok && std::isfinite(manual_best_score)) {
                t.expect(nearly_equal(sweep_best.pnl, manual_best_score),
                    "sweep best objective matches manual best candidate score");
            }
        }

        {
            const std::string objective_json = R"JSON(
{
  "version": 1,
  "name": "contract_phase6_numeric_objective",
  "trade_mode": "LongOnly",
  "params": {
    "alpha": 1
  },
  "numeric_nodes": [
    { "id": "zero", "op": "core.const", "params": { "value": 0.0 } },
    { "id": "one", "op": "core.const", "params": { "value": 1.0 } },
    { "id": "score_value", "op": "core.const", "params": { "value": { "ref": "alpha" } } }
  ],
  "bool_nodes": [
    { "id": "entry", "op": "cmp.gt", "args": ["one", "zero"] }
  ],
  "bindings": {
    "entry_long": "entry",
    "exit_long": "",
    "entry_short": "",
    "exit_short": "",
    "allow_trade": ""
  },
  "sweep": {
    "enabled": true,
    "top_k": 3,
    "max_evals": 3,
    "objective": {
      "value": {
        "source": "numeric_node_output",
        "name": "score_value",
        "summary": "last_finite"
      },
      "direction": "maximize",
      "tie_breaks": [
        {
          "value": {
            "source": "reported_metric",
            "name": "pnl"
          },
          "direction": "maximize"
        }
      ]
    },
    "params": {
      "alpha": {
        "values": [1, 5, 3],
        "is_int": true
      }
    }
  }
}
)JSON";

            PlanSpec sweep_spec{};
            std::string parse_err;
            const bool parse_ok = parse_plan_spec_json(objective_json, sweep_spec, parse_err);
            t.expect(parse_ok, "phase 6 sweep objective object form parses");
            if (!parse_ok) {
                t.out << "    phase 6 objective parse error: " << parse_err << "\n";
                return true;
            }
            t.expect(sweep_spec.sweep.has_value() && sweep_spec.sweep->objective_spec.has_value(),
                "phase 6 sweep objective object form populates objective_spec");
            if (sweep_spec.sweep && sweep_spec.sweep->objective_spec) {
                t.expect(sweep_spec.sweep->objective_spec->primary.source == ObjectiveValueSource::NumericNodeOutput,
                    "phase 6 objective spec preserves numeric node source");
                t.expect(sweep_spec.sweep->objective_spec->primary.name == "score_value",
                    "phase 6 objective spec preserves named numeric node output");
                t.expect(sweep_spec.sweep->objective_spec->direction == ObjectiveDirection::Maximize,
                    "phase 6 objective spec preserves direction");
                t.expect(sweep_spec.sweep->objective_spec->tie_breaks.size() == 1,
                    "phase 6 objective spec preserves tie-break chain");
            }

            const std::string run_objective_json = R"JSON(
{
  "version": 1,
  "trade_mode": "LongOnly",
  "numeric_nodes": [
    { "id": "c", "op": "core.close" },
    { "id": "hold_days", "op": "core.const", "params": { "value": 7 } }
  ],
  "bool_nodes": [
    { "id": "entry", "op": "cmp.gt", "args": ["c", "c"] }
  ],
  "bindings": {
    "entry_long": "entry",
    "exit_long": "",
    "entry_short": "",
    "exit_short": "",
    "allow_trade": ""
  },
  "run": {
    "scope": "per_entity_ranked",
    "top_k": 2,
    "objective": {
      "value": {
        "source": "reported_metric",
        "name": "pnl"
      },
      "direction": "maximize",
      "tie_breaks": [
        {
          "value": {
            "source": "reported_metric",
            "name": "trades"
          },
          "direction": "minimize"
        }
      ]
    }
  }
}
)JSON";

            PlanSpec run_objective_spec{};
            parse_err.clear();
            const bool run_parse_ok = parse_plan_spec_json(run_objective_json, run_objective_spec, parse_err);
            t.expect(run_parse_ok, "phase 6 run objective object form parses");
            if (run_parse_ok && run_objective_spec.run && run_objective_spec.run->objective_spec) {
                t.expect(run_objective_spec.run->objective_spec->primary.source == ObjectiveValueSource::ReportedMetric,
                    "phase 6 run objective spec preserves reported metric source");
                t.expect(run_objective_spec.run->objective_spec->primary.name == "pnl",
                    "phase 6 run objective spec preserves primary metric name");
                t.expect(run_objective_spec.run->objective_spec->tie_breaks.size() == 1 &&
                        run_objective_spec.run->objective_spec->tie_breaks.front().value.name == "trades",
                    "phase 6 run objective spec preserves tie-break metadata");
            }

            std::vector<double> close;
            close.reserve(8);
            for (std::size_t i = 0; i < 8; ++i) {
                close.push_back(100.0 + static_cast<double>(i));
            }

            std::shared_ptr<align::AlignedBundle> bundle;
            ColumnRef primary{};
            const bool okdv = make_single_series_dataview_from_close(close, bundle, primary);
            t.expect(okdv && bundle != nullptr,
                "phase 6 numeric objective sweep fixture creates temporal DataView input");
            if (!okdv || !bundle) return true;

            const DataView dv(*bundle);
            PlanSweepRuntimeOptions opt{};
            std::vector<SweepProgress> progress_log;
            opt.progress_cb = [&](const SweepProgress& progress) {
                progress_log.push_back(progress);
            };
            RunManifest run_manifest{};
            run_manifest.preprocess.series_keys = { "A" };
            run_manifest.preprocess.has_time_axis = true;
            run_manifest.preprocess.time_kind = TimeKind::YyyyMmDd;
            run_manifest.preprocess.time_resolution = "daily";
            run_manifest.preprocess.alignment_applied = true;
            run_manifest.preprocess.alignment_calendar = align::CalendarPolicy::Intersection;
            run_manifest.preprocess.alignment_missing = align::MissingPolicy::NaN;
            run_manifest.preprocess.alignment_non_temporal_multi_series =
                align::NonTemporalMultiSeriesPolicy::Reject;
            run_manifest.preprocess.input_warnings.push_back("contract warning");
            run_manifest.primary_series_key = "A";
            run_manifest.primary_selection_mode = RuntimePrimarySelectionMode::ExplicitSeries;
            TopologySpec topology{};
            topology.name = builtin_temporal_topology_name();
            run_manifest.topologies.push_back(std::move(topology));
            run_manifest.topology_usages.push_back(TopologyUsageSnapshot{});
            const RunProvenanceSummary run_provenance =
                build_run_provenance_summary(run_manifest);
            std::ostringstream sweep_log;
            BacktestResult sweep_best{};
            PlanSweepReport sweep_report{};
            const bool sweep_ok = run_custom_plan_spec(
                dv,
                primary,
                sweep_spec,
                opt,
                sweep_log,
                &sweep_best,
                &sweep_report,
                &run_provenance);
            t.expect(sweep_ok, "phase 6 numeric objective sweep executes through custom runtime");
            if (sweep_ok) {
                double score_value = std::numeric_limits<double>::quiet_NaN();
                const bool score_ok = std::any_of(
                    sweep_best.reported_values.begin(),
                    sweep_best.reported_values.end(),
                    [&](const NamedReportedValue& value) {
                        if (value.name != "score_value") {
                            return false;
                        }
                        score_value = value.value;
                        return true;
                    });
                t.expect(score_ok, "phase 6 numeric objective sweep preserves the named reported value on the best candidate");
                if (score_ok) {
                    t.expect(nearly_equal(score_value, 5.0),
                        "phase 6 numeric objective sweep ranks by the named numeric node output");
                }
                const std::string sweep_msg = sweep_log.str();
                t.expect(sweep_msg.find("numeric_node_output:score_value [last_finite]") != std::string::npos,
                    "phase 6 sweep report prints the structured numeric objective description");
                t.expect(sweep_msg.find("Run provenance:") != std::string::npos,
                    "phase 6 sweep report prints shared run provenance when provided");
                t.expect(sweep_msg.find("primary_selection_mode: explicit_series") != std::string::npos,
                    "phase 6 sweep report prints human-readable primary-selection provenance");
                t.expect(sweep_msg.find("alpha: 5") != std::string::npos,
                    "phase 6 sweep report ranks the highest objective-valued parameter first");
                t.expect(sweep_report.parameter_count == 1,
                    "phase 6 structured sweep report preserves parameter dimension count");
                t.expect(sweep_report.total_combinations == 3 && !sweep_report.total_combinations_overflow,
                    "phase 6 structured sweep report preserves exact total combination count");
                t.expect(sweep_report.candidate_space_size == 3 && !sweep_report.candidate_space_truncated,
                    "phase 6 structured sweep report preserves candidate space size without false truncation");
                t.expect(sweep_report.evaluations_run == 3,
                    "phase 6 structured sweep report preserves executed evaluation count");
                t.expect(sweep_report.run_provenance.primary_series_key == "A" &&
                        sweep_report.run_provenance.primary_selection_mode == "explicit_series",
                    "phase 6 structured sweep report preserves human-readable run provenance");
                t.expect(sweep_report.candidate_outcomes.completed == 3 &&
                        sweep_report.candidate_outcomes.completed_with_non_finite_metrics == 0 &&
                        sweep_report.candidate_outcomes.failed == 0 &&
                        sweep_report.candidate_outcomes.canceled == 0,
                    "phase 6 structured sweep report preserves candidate outcome counts");
                t.expect(sweep_report.candidate_rankability.rankable == 3 &&
                        sweep_report.candidate_rankability.not_evaluated == 0,
                    "phase 6 structured sweep report preserves candidate rankability counts");
                const bool has_alpha_range =
                    sweep_report.parameter_ranges.size() == 1 &&
                    sweep_report.parameter_ranges.front().name == "alpha" &&
                    sweep_report.parameter_ranges.front().expanded_count == 3;
                t.expect(has_alpha_range,
                    "phase 6 structured sweep report preserves named parameter range summaries");
                const bool has_ranking_stage = std::any_of(
                    sweep_report.stage_summaries.begin(),
                    sweep_report.stage_summaries.end(),
                    [](const SweepStageSummary& stage) {
                        return stage.stage == SweepProgressStage::Ranking &&
                            stage.done == 1 &&
                            stage.total == 1;
                    });
                t.expect(has_ranking_stage,
                    "phase 6 structured sweep report preserves ranking stage completion");
                const bool has_report_stage = std::any_of(
                    sweep_report.stage_summaries.begin(),
                    sweep_report.stage_summaries.end(),
                    [](const SweepStageSummary& stage) {
                        return stage.stage == SweepProgressStage::ReportAssembly &&
                            stage.done == 1 &&
                            stage.total == 1;
                    });
                t.expect(has_report_stage,
                    "phase 6 structured sweep report preserves report-assembly stage completion");
                const bool best_has_alpha_value =
                    !sweep_report.top_candidates.empty() &&
                    !sweep_report.top_candidates.front().parameter_values.empty() &&
                    sweep_report.top_candidates.front().parameter_values.front().name == "alpha" &&
                    std::holds_alternative<std::int64_t>(sweep_report.top_candidates.front().parameter_values.front().value) &&
                    std::get<std::int64_t>(sweep_report.top_candidates.front().parameter_values.front().value) == 5;
                t.expect(best_has_alpha_value,
                    "phase 6 structured sweep report preserves human-readable best-candidate parameter values");
                const RunReport best_report = build_run_report(
                    sweep_best,
                    &sweep_report.resolved_objective,
                    &run_provenance);
                t.expect(best_report.objective.requested,
                    "phase 6 run report preserves requested ranking objective");
                t.expect(best_report.objective.rankability == RunRankabilityState::Rankable,
                    "phase 6 run report classifies finite objective results as rankable");
                t.expect(nearly_equal(best_report.objective.score, 5.0),
                    "phase 6 run report preserves evaluated objective score");
                t.expect(best_report.provenance.primary_series_key == "A" &&
                        best_report.provenance.topology_usage_count == 1,
                    "phase 6 run report preserves shared provenance without pushing it into BacktestResult");
                const auto* reported_metric = find_metric_value(best_report, "score_value");
                t.expect(
                    reported_metric != nullptr &&
                    reported_metric->kind == ResultMetricKind::ReportedValue &&
                    nearly_equal(reported_metric->value, 5.0),
                    "phase 6 result metric catalog preserves reported numeric-node values");
                const auto* row_metric = find_metric_value(best_report, "rows_evaluated");
                t.expect(
                    row_metric != nullptr &&
                    row_metric->kind == ResultMetricKind::CoreMetric &&
                    nearly_equal(row_metric->value, 8.0),
                    "phase 6 result metric catalog preserves executor row coverage");
                const auto* signal_metric = find_metric_value(best_report, "entry_long_signals");
                t.expect(
                    signal_metric != nullptr &&
                    signal_metric->kind == ResultMetricKind::SignalCount &&
                    signal_metric->value > 0.0,
                    "phase 6 result metric catalog preserves signal-count metrics");
                const bool progress_has_live_leader = std::any_of(
                    progress_log.begin(),
                    progress_log.end(),
                    [](const SweepProgress& progress) {
                        return progress.stage == SweepProgressStage::CandidateEvaluation &&
                            !progress.current_best.empty() &&
                            !progress.current_best.front().parameter_values.empty();
                    });
                t.expect(progress_has_live_leader,
                    "phase 6 sweep progress exposes a live ranked leader snapshot with parameter values");
            }

            PlanSpec monte_carlo_spec{};
            monte_carlo_spec.version = 1;
            monte_carlo_spec.name = "contract_phase6_monte_carlo_budget";
            monte_carlo_spec.trade_mode = TradeMode::LongOnly;
            monte_carlo_spec.params["alpha"] = static_cast<std::int64_t>(1);
            monte_carlo_spec.numeric_nodes = {
                NodeSpec{ "zero", "core.const", {}, ParamMap{ {"value", 0.0} } },
                NodeSpec{ "one", "core.const", {}, ParamMap{ {"value", 1.0} } },
                NodeSpec{ "score_value", "core.const", {}, ParamMap{ {"value", ParamRef{ "alpha" }} } }
            };
            monte_carlo_spec.bool_nodes = {
                NodeSpec{ "entry", "cmp.gt", { "one", "zero" }, {} }
            };
            monte_carlo_spec.bind.entry_long = "entry";

            PlanSweepSpec monte_carlo_cfg{};
            monte_carlo_cfg.enabled = true;
            monte_carlo_cfg.engine = PlanSweepEngineKind::MonteCarlo;
            monte_carlo_cfg.top_k = 2;
            monte_carlo_cfg.max_evals = 3;
            monte_carlo_cfg.monte_carlo.draws = 10;
            monte_carlo_cfg.monte_carlo.seed = 12345;
            monte_carlo_cfg.monte_carlo.dedupe = true;
            monte_carlo_cfg.objective_spec = RankingObjectiveSpec{};
            monte_carlo_cfg.objective_spec->primary.source = ObjectiveValueSource::NumericNodeOutput;
            monte_carlo_cfg.objective_spec->primary.name = "score_value";
            monte_carlo_cfg.objective_spec->direction = ObjectiveDirection::Maximize;
            SweepParamRange monte_carlo_alpha_range{};
            monte_carlo_alpha_range.min = 1.0;
            monte_carlo_alpha_range.max = 10.0;
            monte_carlo_alpha_range.step = 1.0;
            monte_carlo_alpha_range.is_int = true;
            monte_carlo_cfg.params["alpha"] = monte_carlo_alpha_range;
            monte_carlo_spec.sweep = monte_carlo_cfg;

            std::vector<SweepProgress> monte_carlo_progress_log;
            PlanSweepRuntimeOptions monte_carlo_opt{};
            monte_carlo_opt.progress_cb = [&](const SweepProgress& progress) {
                monte_carlo_progress_log.push_back(progress);
            };

            std::ostringstream monte_carlo_log;
            BacktestResult monte_carlo_best{};
            PlanSweepReport monte_carlo_report{};
            const bool monte_carlo_ok = run_custom_plan_spec(
                dv,
                primary,
                monte_carlo_spec,
                monte_carlo_opt,
                monte_carlo_log,
                &monte_carlo_best,
                &monte_carlo_report);
            t.expect(monte_carlo_ok, "phase 6 monte carlo objective sweep executes through custom runtime");
            if (monte_carlo_ok) {
                const bool saw_monte_carlo_progress = std::any_of(
                    monte_carlo_progress_log.begin(),
                    monte_carlo_progress_log.end(),
                    [](const SweepProgress& progress) {
                        return progress.stage == SweepProgressStage::MonteCarloSampling &&
                            progress.stage_done == 3 &&
                            progress.stage_total == 3;
                    });
                t.expect(saw_monte_carlo_progress,
                    "phase 6 progress stream reports effective monte carlo draws after max_evals capping");
                t.expect(monte_carlo_report.candidate_space_size == 10 && !monte_carlo_report.candidate_space_truncated,
                    "phase 6 structured sweep report preserves the full monte carlo search space");
                t.expect(monte_carlo_report.evaluations_run == 3,
                    "phase 6 structured sweep report preserves capped monte carlo evaluation count");
                const bool has_full_alpha_range =
                    monte_carlo_report.parameter_ranges.size() == 1 &&
                    monte_carlo_report.parameter_ranges.front().name == "alpha" &&
                    monte_carlo_report.parameter_ranges.front().expanded_count == 10;
                t.expect(has_full_alpha_range,
                    "phase 6 structured sweep report preserves full declared range expansion under monte carlo");
                const bool report_has_monte_carlo_stage = std::any_of(
                    monte_carlo_report.stage_summaries.begin(),
                    monte_carlo_report.stage_summaries.end(),
                    [](const SweepStageSummary& stage) {
                        return stage.stage == SweepProgressStage::MonteCarloSampling &&
                            stage.done == 3 &&
                            stage.total == 3;
                    });
                t.expect(report_has_monte_carlo_stage,
                    "phase 6 structured sweep report preserves effective monte carlo draw totals");
            }

            const std::string default_draws_json = R"JSON(
{
  "version": 1,
  "name": "contract_monte_carlo_default_draws_follow_max_evals",
  "trade_mode": "LongOnly",
  "params": { "alpha": 1 },
  "numeric_nodes": [
    { "id": "zero", "op": "core.const", "params": { "value": 0.0 } },
    { "id": "one", "op": "core.const", "params": { "value": 1.0 } },
    { "id": "score_value", "op": "core.const", "params": { "value": { "ref": "alpha" } } }
  ],
  "bool_nodes": [
    { "id": "entry", "op": "cmp.gt", "args": ["one", "zero"] }
  ],
  "bindings": { "entry_long": "entry", "exit_long": "", "entry_short": "", "exit_short": "", "allow_trade": "" },
  "sweep": {
    "enabled": true,
    "engine": "monte_carlo",
    "top_k": 2,
    "max_evals": 7,
    "objective": { "value": { "source": "numeric_node_output", "name": "score_value" }, "direction": "maximize" },
    "params": {
      "alpha": { "min": 1, "max": 10, "step": 1, "is_int": true }
    }
  }
}
)JSON";
            PlanSpec default_draws_spec{};
            std::string default_draws_err{};
            const bool default_draws_parsed =
                parse_plan_spec_json(default_draws_json, default_draws_spec, default_draws_err);
            t.expect(default_draws_parsed,
                "PlanSpec parser accepts monte_carlo sweep with omitted draws");
            if (default_draws_parsed) {
                t.expect(default_draws_spec.sweep.has_value() &&
                        default_draws_spec.sweep->monte_carlo.draws == 7 &&
                        !default_draws_spec.sweep->monte_carlo.draws_explicit,
                    "omitted monte_carlo draws defaults to the user-facing max_evals budget");

                std::ostringstream default_draws_log;
                BacktestResult default_draws_best{};
                PlanSweepReport default_draws_report{};
                const bool default_draws_ok = run_custom_plan_spec(
                    dv,
                    primary,
                    default_draws_spec,
                    PlanSweepRuntimeOptions{},
                    default_draws_log,
                    &default_draws_best,
                    &default_draws_report);
                t.expect(default_draws_ok,
                    "monte_carlo sweep with omitted draws executes through custom runtime");
                if (default_draws_ok) {
                    t.expect(default_draws_report.evaluations_run == 7,
                        "omitted monte_carlo draws runs max_evals evaluations instead of the old 10000 default ceiling");
                    t.expect(default_draws_report.requested_sweep.monte_carlo.draws == 7,
                        "structured monte_carlo report shows the effective default draw count");
                }
            }

            const std::string coarse_multiplier_json = R"JSON(
{
  "version": 1,
  "name": "contract_coarse_step_multiplier_budget",
  "trade_mode": "LongOnly",
  "params": { "alpha": 1, "beta": 1, "gamma": 1 },
  "numeric_nodes": [
    { "id": "zero", "op": "core.const", "params": { "value": 0.0 } },
    { "id": "one", "op": "core.const", "params": { "value": 1.0 } },
    { "id": "score_value", "op": "core.const", "params": { "value": { "ref": "alpha" } } }
  ],
  "bool_nodes": [
    { "id": "entry", "op": "cmp.gt", "args": ["one", "zero"] }
  ],
  "bindings": { "entry_long": "entry", "exit_long": "", "entry_short": "", "exit_short": "", "allow_trade": "" },
  "sweep": {
    "enabled": true,
    "engine": "coarse_fine",
    "top_k": 2,
    "max_evals": 40,
    "objective": { "value": { "source": "numeric_node_output", "name": "score_value" }, "direction": "maximize" },
    "params": {
      "alpha": { "min": 1, "max": 4, "step": 1, "is_int": true },
      "beta": { "min": 1, "max": 4, "step": 1, "is_int": true },
      "gamma": { "min": 1, "max": 4, "step": 1, "is_int": true }
    },
    "engine_config": {
      "coarse_fine": {
        "k_stage1": 4,
        "k_stage2": 2,
        "coarse_step_multiplier": 2,
        "depollute_stage1": false,
        "dedupe_stage2": true
      }
    }
  }
}
)JSON";
            PlanSpec coarse_multiplier_spec{};
            std::string coarse_multiplier_err{};
            const bool coarse_multiplier_parsed =
                parse_plan_spec_json(coarse_multiplier_json, coarse_multiplier_spec, coarse_multiplier_err);
            t.expect(coarse_multiplier_parsed,
                "PlanSpec parser accepts coarse_step_multiplier for coarse_fine sweeps");
            if (coarse_multiplier_parsed) {
                std::vector<SweepProgress> coarse_multiplier_progress;
                PlanSweepRuntimeOptions coarse_multiplier_opt{};
                coarse_multiplier_opt.progress_cb = [&](const SweepProgress& progress) {
                    coarse_multiplier_progress.push_back(progress);
                };

                std::ostringstream coarse_multiplier_log;
                BacktestResult coarse_multiplier_best{};
                PlanSweepReport coarse_multiplier_report{};
                const bool coarse_multiplier_ok = run_custom_plan_spec(
                    dv,
                    primary,
                    coarse_multiplier_spec,
                    coarse_multiplier_opt,
                    coarse_multiplier_log,
                    &coarse_multiplier_best,
                    &coarse_multiplier_report);
                t.expect(coarse_multiplier_ok,
                    "coarse_fine sweep with coarse_step_multiplier executes through custom runtime");
                if (coarse_multiplier_ok) {
                    t.expect(coarse_multiplier_report.requested_sweep.coarse_fine.coarse_step_multiplier == 2,
                        "structured coarse_fine report preserves the configured coarse_step_multiplier");
                    t.expect(coarse_multiplier_report.evaluations_run == 40,
                        "coarse_fine sweep uses the remaining max_evals budget for local refinement");
                    t.expect(coarse_multiplier_report.requested_sweep.coarse_fine.stage2_hops > 1,
                        "coarse_fine sweep derives a wider refinement radius when the default would underuse max_evals");
                    const bool coarse_stage_was_strided = std::any_of(
                        coarse_multiplier_report.stage_summaries.begin(),
                        coarse_multiplier_report.stage_summaries.end(),
                        [](const SweepStageSummary& stage) {
                            return stage.stage == SweepProgressStage::CoarseSearch &&
                                stage.done == 8 &&
                                stage.total == 8;
                        });
                    t.expect(coarse_stage_was_strided,
                        "coarse_step_multiplier samples every Nth value per dimension instead of scanning the full fine grid");
                    t.expect(coarse_multiplier_log.str().find("coarse step multiplier: 2") != std::string::npos,
                        "coarse_fine sweep output makes the effective coarse multiplier visible");
                }
            }

            const std::string trade_hash_diversity_json = R"JSON(
{
  "version": 1,
  "name": "contract_trade_hash_result_diversity",
  "trade_mode": "LongOnly",
  "params": { "alpha": 1, "beta": 1 },
  "numeric_nodes": [
    { "id": "zero", "op": "core.const", "params": { "value": 0.0 } },
    { "id": "one", "op": "core.const", "params": { "value": 1.0 } },
    { "id": "score_value", "op": "core.const", "params": { "value": { "ref": "alpha" } } }
  ],
  "bool_nodes": [
    { "id": "entry", "op": "cmp.gt", "args": ["one", "zero"] }
  ],
  "bindings": { "entry_long": "entry", "exit_long": "", "entry_short": "", "exit_short": "", "allow_trade": "" },
  "sweep": {
    "enabled": true,
    "top_k": 3,
    "max_evals": 16,
    "objective": { "value": { "source": "numeric_node_output", "name": "score_value" }, "direction": "maximize" },
    "result_diversity": {
      "dedupe_trade_hash": true,
      "oversample_multiplier": 8,
      "min_trades": 1
    },
    "params": {
      "alpha": { "min": 1, "max": 4, "step": 1, "is_int": true },
      "beta": { "min": 1, "max": 4, "step": 1, "is_int": true }
    }
  }
}
)JSON";
            PlanSpec trade_hash_diversity_spec{};
            std::string trade_hash_diversity_err{};
            const bool trade_hash_diversity_parsed =
                parse_plan_spec_json(
                    trade_hash_diversity_json,
                    trade_hash_diversity_spec,
                    trade_hash_diversity_err);
            t.expect(trade_hash_diversity_parsed,
                "PlanSpec parser accepts final result-diversity controls");
            if (trade_hash_diversity_parsed) {
                std::ostringstream diversity_log;
                BacktestResult diversity_best{};
                PlanSweepReport diversity_report{};
                const bool diversity_ok = run_custom_plan_spec(
                    dv,
                    primary,
                    trade_hash_diversity_spec,
                    PlanSweepRuntimeOptions{},
                    diversity_log,
                    &diversity_best,
                    &diversity_report);
                t.expect(diversity_ok,
                    "trade-hash result-diversity sweep executes through custom runtime");
                if (diversity_ok) {
                    t.expect(diversity_report.result_diversity.requested &&
                            diversity_report.result_diversity.dedupe_trade_hash,
                        "structured sweep report records requested trade-hash result diversity");
                    t.expect(diversity_report.result_diversity.retained_candidate_pool > trade_hash_diversity_spec.sweep->top_k,
                        "result diversity retains an oversampled candidate pool before final Top-K filtering");
                    t.expect(diversity_report.top_candidates.size() == 1 &&
                            diversity_report.result_diversity.returned_unique_trade_hashes == 1,
                        "trade-hash result diversity collapses repeated final outcomes to one representative");
                    t.expect(diversity_report.result_diversity.suppressed_duplicate_trade_hash > 0,
                        "structured sweep report counts suppressed duplicate trade-hash candidates");
                    t.expect(diversity_log.str().find("dedupe trade hash: true") != std::string::npos,
                        "sweep report makes trade-hash result diversity visible");
                }
            }

            const std::string basin_diversity_json = R"JSON(
{
  "version": 1,
  "name": "contract_basin_result_diversity",
  "trade_mode": "LongOnly",
  "params": { "alpha": 1, "beta": 1 },
  "numeric_nodes": [
    { "id": "zero", "op": "core.const", "params": { "value": 0.0 } },
    { "id": "one", "op": "core.const", "params": { "value": 1.0 } },
    { "id": "score_value", "op": "core.const", "params": { "value": { "ref": "alpha" } } }
  ],
  "bool_nodes": [
    { "id": "entry", "op": "cmp.gt", "args": ["one", "zero"] }
  ],
  "bindings": { "entry_long": "entry", "exit_long": "", "entry_short": "", "exit_short": "", "allow_trade": "" },
  "sweep": {
    "enabled": true,
    "top_k": 4,
    "max_evals": 16,
    "objective": { "value": { "source": "numeric_node_output", "name": "score_value" }, "direction": "maximize" },
    "result_diversity": {
      "dedupe_trade_hash": false,
      "basin_aware": true,
      "oversample_multiplier": 8,
      "max_per_basin": 1,
      "param_bucket_size": 2
    },
    "params": {
      "alpha": { "min": 1, "max": 4, "step": 1, "is_int": true },
      "beta": { "min": 1, "max": 4, "step": 1, "is_int": true }
    }
  }
}
)JSON";
            PlanSpec basin_diversity_spec{};
            std::string basin_diversity_err{};
            const bool basin_diversity_parsed =
                parse_plan_spec_json(
                    basin_diversity_json,
                    basin_diversity_spec,
                    basin_diversity_err);
            t.expect(basin_diversity_parsed,
                "PlanSpec parser accepts basin-aware result-diversity controls");
            if (basin_diversity_parsed) {
                std::ostringstream basin_log;
                BacktestResult basin_best{};
                PlanSweepReport basin_report{};
                const bool basin_ok = run_custom_plan_spec(
                    dv,
                    primary,
                    basin_diversity_spec,
                    PlanSweepRuntimeOptions{},
                    basin_log,
                    &basin_best,
                    &basin_report);
                t.expect(basin_ok,
                    "basin-aware result-diversity sweep executes through custom runtime");
                if (basin_ok) {
                    t.expect(basin_report.result_diversity.requested &&
                            basin_report.result_diversity.basin_aware &&
                            !basin_report.result_diversity.dedupe_trade_hash,
                        "structured sweep report records basin-aware result diversity separately from trade-hash dedupe");
                    t.expect(basin_report.result_diversity.returned_basin_count == basin_report.top_candidates.size() &&
                            basin_report.top_candidates.size() == 4,
                        "basin-aware result diversity returns at most one candidate per bucketed parameter basin");
                    t.expect(basin_report.result_diversity.suppressed_basin > 0,
                        "structured sweep report counts candidates suppressed by basin diversity");
                    t.expect(basin_log.str().find("basin aware: true") != std::string::npos,
                        "sweep report makes basin-aware result diversity visible");
                }
            }
        }

        {
            std::vector<double> close;
            close.reserve(12);
            for (std::size_t i = 0; i < 12; ++i) {
                close.push_back(100.0 + static_cast<double>(i));
            }

            std::shared_ptr<align::AlignedBundle> bundle;
            ColumnRef primary{};
            const bool okdv = make_single_series_dataview_from_close(close, bundle, primary);
            t.expect(okdv && bundle != nullptr,
                "phase 6 coarse-fine progress fixture creates temporal DataView input");
            if (!okdv || !bundle) return true;

            PlanSpec sweep_spec{};
            sweep_spec.version = 1;
            sweep_spec.name = "contract_phase6_stage_progress";
            sweep_spec.trade_mode = TradeMode::LongOnly;
            sweep_spec.params["alpha"] = static_cast<std::int64_t>(1);

            NodeSpec zero{};
            zero.id = "zero";
            zero.op = "core.const";
            zero.params["value"] = 0.0;

            NodeSpec one{};
            one.id = "one";
            one.op = "core.const";
            one.params["value"] = 1.0;

            NodeSpec score_value{};
            score_value.id = "score_value";
            score_value.op = "core.const";
            score_value.params["value"] = ParamRef{ "alpha" };

            sweep_spec.numeric_nodes = { zero, one, score_value };

            NodeSpec entry{};
            entry.id = "entry";
            entry.op = "cmp.gt";
            entry.args = { "one", "zero" };
            sweep_spec.bool_nodes = { entry };
            sweep_spec.bind.entry_long = "entry";

            PlanSweepSpec sweep_cfg{};
            sweep_cfg.enabled = true;
            sweep_cfg.engine = PlanSweepEngineKind::CoarseFine;
            sweep_cfg.top_k = 2;
            sweep_cfg.max_evals = 3;
            sweep_cfg.coarse_fine.k_stage1 = 2;
            sweep_cfg.coarse_fine.k_stage2 = 2;
            sweep_cfg.coarse_fine.depollute_stage1 = false;
            sweep_cfg.coarse_fine.stage1_stride = 0;
            sweep_cfg.objective_spec = RankingObjectiveSpec{};
            sweep_cfg.objective_spec->primary.source = ObjectiveValueSource::NumericNodeOutput;
            sweep_cfg.objective_spec->primary.name = "score_value";
            sweep_cfg.objective_spec->direction = ObjectiveDirection::Maximize;
            SweepParamRange alpha_range{};
            alpha_range.values = { 1.0, 5.0, 3.0 };
            alpha_range.is_int = true;
            sweep_cfg.params["alpha"] = alpha_range;
            sweep_spec.sweep = sweep_cfg;

            std::vector<SweepProgress> progress_log;
            PlanSweepRuntimeOptions opt{};
            opt.progress_cb = [&](const SweepProgress& progress) {
                progress_log.push_back(progress);
            };

            std::ostringstream sweep_log;
            BacktestResult sweep_best{};
            PlanSweepReport sweep_report{};
            const bool sweep_ok = run_custom_plan_spec(
                DataView(*bundle),
                primary,
                sweep_spec,
                opt,
                sweep_log,
                &sweep_best,
                &sweep_report);
            t.expect(sweep_ok, "phase 6 coarse-fine objective sweep executes through custom runtime");
            if (sweep_ok) {
                const bool saw_coarse_search = std::any_of(
                    progress_log.begin(),
                    progress_log.end(),
                    [](const SweepProgress& progress) {
                        return progress.stage == SweepProgressStage::CoarseSearch;
                    });
                t.expect(saw_coarse_search,
                    "phase 6 progress stream distinguishes the coarse-search stage");
                const bool saw_local_refinement = std::any_of(
                    progress_log.begin(),
                    progress_log.end(),
                    [](const SweepProgress& progress) {
                        return progress.stage == SweepProgressStage::LocalRefinement;
                    });
                t.expect(saw_local_refinement,
                    "phase 6 progress stream distinguishes the local-refinement stage");
                const bool report_has_coarse_stage = std::any_of(
                    sweep_report.stage_summaries.begin(),
                    sweep_report.stage_summaries.end(),
                    [](const SweepStageSummary& stage) {
                        return stage.stage == SweepProgressStage::CoarseSearch;
                    });
                t.expect(report_has_coarse_stage,
                    "phase 6 structured sweep report preserves coarse-search stage totals");
                const bool report_has_refinement_stage = std::any_of(
                    sweep_report.stage_summaries.begin(),
                    sweep_report.stage_summaries.end(),
                    [](const SweepStageSummary& stage) {
                        return stage.stage == SweepProgressStage::LocalRefinement;
                    });
                t.expect(report_has_refinement_stage,
                    "phase 6 structured sweep report preserves local-refinement stage totals");
            }
        }

        {
            const std::string metric_json = R"JSON(
{
  "version": 1,
  "name": "contract_phase6_non_finite_metrics",
  "trade_mode": "LongOnly",
  "params": {
    "dummy": 1
  },
  "numeric_nodes": [
    { "id": "zero", "op": "core.const", "params": { "value": 0.0 } },
    { "id": "one", "op": "core.const", "params": { "value": 1.0 } },
    { "id": "nan_value", "op": "ts.div", "args": ["zero", "zero"] }
  ],
  "bool_nodes": [
    { "id": "entry", "op": "cmp.gt", "args": ["one", "zero"] }
  ],
  "bindings": {
    "entry_long": "entry",
    "exit_long": "",
    "entry_short": "",
    "exit_short": "",
    "allow_trade": ""
  },
  "sweep": {
    "enabled": true,
    "top_k": 1,
    "max_evals": 1,
    "objective": {
      "value": {
        "source": "reported_metric",
        "name": "pnl"
      },
      "direction": "maximize",
      "tie_breaks": [
        {
          "value": {
            "source": "numeric_node_output",
            "name": "nan_value",
            "summary": "last_finite"
          },
          "direction": "maximize"
        }
      ]
    },
    "params": {
      "dummy": {
        "values": [1],
        "is_int": true
      }
    }
  }
}
)JSON";

            PlanSpec metric_spec{};
            std::string parse_err;
            const bool parse_ok = parse_plan_spec_json(metric_json, metric_spec, parse_err);
            t.expect(parse_ok, "phase 6 non-finite metric objective form parses");
            if (!parse_ok) {
                t.out << "    phase 6 non-finite metric parse error: " << parse_err << "\n";
                return true;
            }

            std::vector<double> close;
            close.reserve(8);
            for (std::size_t i = 0; i < 8; ++i) {
                close.push_back(100.0 + static_cast<double>(i));
            }

            std::shared_ptr<align::AlignedBundle> bundle;
            ColumnRef primary{};
            const bool okdv = make_single_series_dataview_from_close(close, bundle, primary);
            t.expect(okdv && bundle != nullptr,
                "phase 6 non-finite metric fixture creates temporal DataView input");
            if (!okdv || !bundle) return true;

            std::ostringstream sweep_log;
            BacktestResult sweep_best{};
            PlanSweepReport sweep_report{};
            const bool sweep_ok = run_custom_plan_spec(
                DataView(*bundle),
                primary,
                metric_spec,
                PlanSweepRuntimeOptions{},
                sweep_log,
                &sweep_best,
                &sweep_report);
            t.expect(sweep_ok, "phase 6 non-finite metric sweep executes through custom runtime");
            if (sweep_ok) {
                const RunReport best_report = build_run_report(sweep_best);
                const auto* non_finite_metric = find_metric_value(best_report, "nan_value");
                t.expect(
                    non_finite_metric != nullptr &&
                    non_finite_metric->kind == ResultMetricKind::ReportedValue &&
                    !non_finite_metric->is_finite,
                    "phase 6 result metric catalog marks non-finite reported metrics explicitly");
                const bool has_non_finite_name = std::find(
                    best_report.metric_catalog.non_finite_metric_names.begin(),
                    best_report.metric_catalog.non_finite_metric_names.end(),
                    "nan_value") != best_report.metric_catalog.non_finite_metric_names.end();
                t.expect(has_non_finite_name,
                    "phase 6 result metric catalog preserves non-finite metric names");
                t.expect(sweep_log.str().find("Non-finite metrics:") != std::string::npos,
                    "phase 6 backtest reporting surfaces explicit non-finite metric summaries");
                t.expect(sweep_report.candidate_outcomes.completed_with_non_finite_metrics == 1 &&
                        sweep_report.candidate_outcomes.failed == 0,
                    "phase 6 structured sweep report counts completed non-finite candidates separately from failures");
                t.expect(sweep_report.candidate_rankability.rankable == 1,
                    "phase 6 structured sweep report preserves primary-objective rankability even when other metrics are non-finite");
            }

            RankingObjectiveSpec non_finite_objective{};
            non_finite_objective.primary.source = ObjectiveValueSource::NumericNodeOutput;
            non_finite_objective.primary.name = "nan_value";
            non_finite_objective.primary.summary_mode = NumericNodeSummaryMode::LastFiniteValue;
            non_finite_objective.direction = ObjectiveDirection::Maximize;

            BacktestResult non_finite_result{};
            non_finite_result.reported_values.push_back(
                NamedReportedValue{ "nan_value", std::numeric_limits<double>::quiet_NaN() });
            const RunReport non_finite_objective_report =
                build_run_report(non_finite_result, &non_finite_objective);
            t.expect(non_finite_objective_report.objective.requested,
                "phase 6 run report preserves requested non-finite objective metadata");
            t.expect(non_finite_objective_report.objective.rankability == RunRankabilityState::NonFinitePrimaryValue,
                "phase 6 run report classifies non-finite primary objectives as non-rankable");
            t.expect(non_finite_objective_report.outcome == RunOutcomeState::CompletedWithNonFiniteMetrics,
                "phase 6 run report classifies preserved non-finite metrics in run outcome state");
        }

        return true;
    }

    bool test_compile_bridge_failure_path_contracts(ContractAsserts& t) {
        t.out << "[compile_bridge_failure_path] compile-bridge explicit failure-path contracts\n";

        PlanSweepRuntimeOptions opt{};

        {
            const std::vector<double> spatial_close = { 10.0, 20.0, 30.0 };
            const std::vector<double> spatial_x = { 0.0, 1.0, 10.0 };
            const std::vector<double> spatial_y = { 0.0, 0.0, 0.0 };
            std::shared_ptr<align::AlignedBundle> spatial_bundle;
            ColumnRef spatial_primary{};
            const bool spatial_dv_ok = make_single_series_dataview_with_index_schema(
                spatial_close,
                make_spatial_only_index_schema(),
                spatial_bundle,
                spatial_primary,
                static_cast<TimeKey>(0),
                spatial_x,
                spatial_y);
            t.expect(spatial_dv_ok && spatial_bundle != nullptr,
                "compile-bridge failure-path spatial fixture builds");
            if (spatial_dv_ok && spatial_bundle) {
                PlanSpec spatial_bad_topology = make_spatial_radius_mean_plan(
                    "contract_compile_bridge_spatial_runtime_bad_topology",
                    1.1,
                    14.0,
                    "missing.topology");
                add_standard_exit_binding(spatial_bad_topology);

                const DataView spatial_dv(*spatial_bundle);
                std::ostringstream spatial_bad_log;
                BacktestResult spatial_bad_run{};
                const bool spatial_bad_ok = run_custom_plan_spec(
                    spatial_dv, spatial_primary, spatial_bad_topology, opt, spatial_bad_log, &spatial_bad_run);
                t.expect(!spatial_bad_ok,
                    "custom PlanSpec runtime rejects unavailable named topology for spatial.radius_mean");
                t.expect(spatial_bad_log.str().find("required topology 'missing.topology' is not available on DataView") != std::string::npos,
                    "spatial runtime validation reports missing named topology explicitly");

                PlanSpec spatial_bad_subject = make_spatial_radius_mean_plan(
                    "contract_compile_bridge_spatial_runtime_bad_subject",
                    1.1,
                    14.0,
                    std::string(builtin_spatial_topology_name()),
                    std::string("missing.series"));
                add_standard_exit_binding(spatial_bad_subject);

                std::ostringstream spatial_bad_subject_log;
                BacktestResult spatial_bad_subject_run{};
                const bool spatial_bad_subject_ok = run_custom_plan_spec(
                    spatial_dv, spatial_primary, spatial_bad_subject, opt, spatial_bad_subject_log, &spatial_bad_subject_run);
                t.expect(!spatial_bad_subject_ok,
                    "custom PlanSpec runtime rejects unresolved locality subject for spatial.radius_mean");
                const std::string spatial_bad_subject_msg = spatial_bad_subject_log.str();
                t.expect(spatial_bad_subject_msg.find("locality subject could not be resolved") != std::string::npos,
                    "spatial runtime validation reports locality-subject resolution failure explicitly");
                t.expect(spatial_bad_subject_msg.find("series not found") != std::string::npos,
                    "spatial runtime validation preserves missing-series detail for locality subject");
            }
        }

        {
            const std::vector<double> st_close = { 10.0, 20.0, 30.0, 40.0 };
            const std::vector<double> st_x = { 0.0, 0.4, 10.0, 10.4 };
            const std::vector<double> st_y = { 0.0, 0.0, 0.0, 0.0 };
            std::shared_ptr<align::AlignedBundle> st_bundle;
            ColumnRef st_primary{};
            const bool st_dv_ok = make_single_series_dataview_with_index_schema(
                st_close,
                make_time_spatial_index_schema(),
                st_bundle,
                st_primary,
                static_cast<TimeKey>(20240101),
                st_x,
                st_y);
            t.expect(st_dv_ok && st_bundle != nullptr,
                "compile-bridge failure-path spatiotemporal fixture builds");
            if (st_dv_ok && st_bundle) {
                PlanSpec st_bad_topology = make_spatiotemporal_radius_mean_plan(
                    "contract_compile_bridge_st_runtime_bad_topology",
                    2,
                    0.5,
                    20.0,
                    std::string(builtin_temporal_topology_name()));
                add_standard_exit_binding(st_bad_topology);

                const DataView st_dv(*st_bundle);
                std::ostringstream st_bad_log;
                BacktestResult st_bad_run{};
                const bool st_bad_ok = run_custom_plan_spec(
                    st_dv, st_primary, st_bad_topology, opt, st_bad_log, &st_bad_run);
                t.expect(!st_bad_ok,
                    "custom PlanSpec runtime rejects topology family mismatches for st.radius_mean");
                const std::string st_bad_msg = st_bad_log.str();
                t.expect(st_bad_msg.find("has family 'temporal_window'") != std::string::npos,
                    "spatio-temporal runtime validation reports actual mismatched topology family");
                t.expect(st_bad_msg.find("spatiotemporal_composite") != std::string::npos,
                    "spatio-temporal runtime validation reports required topology family");
            }
        }

        {
            std::vector<double> close;
            close.reserve(16);
            for (std::size_t i = 0; i < 16; ++i) {
                close.push_back(100.0 + static_cast<double>(i));
            }
            std::shared_ptr<align::AlignedBundle> bundle;
            ColumnRef primary{};
            const bool okdv = make_single_series_dataview_from_close(close, bundle, primary);
            t.expect(okdv && bundle != nullptr, "compile-bridge failure-path temporal fixture builds");
            if (!okdv || !bundle) return true;

            PlanSpec bad{};
            bad.version = 1;
            bad.name = "contract_compile_bridge_no_fallback";
            bad.trade_mode = TradeMode::LongOnly;

            NodeSpec n_close{};
            n_close.id = "c";
            n_close.op = "core.close";

            NodeSpec n_bad{};
            n_bad.id = "sma";
            n_bad.op = "bad.unsupported_op";
            n_bad.args = { "c" };

            NodeSpec n_thr{};
            n_thr.id = "thr";
            n_thr.op = "core.const";
            n_thr.params["value"] = 100.0;

            bad.numeric_nodes = { n_close, n_bad, n_thr };

            NodeSpec bad_entry{};
            bad_entry.id = "entry_bad";
            bad_entry.op = "cmp.gt";
            bad_entry.args = { "c", "thr" };

            NodeSpec bad_exit{};
            bad_exit.id = "exit_bad";
            bad_exit.op = "cmp.lt";
            bad_exit.args = { "c", "thr" };

            bad.bool_nodes = { bad_entry, bad_exit };
            bad.bind.entry_long = "entry_bad";
            bad.bind.exit_long = "exit_bad";

            const DataView dv(*bundle);
            std::ostringstream bad_log;
            BacktestResult bad_r{};
            const bool bad_ok = run_custom_plan_spec(dv, primary, bad, opt, bad_log, &bad_r);
            t.expect(!bad_ok, "custom runtime hard-fails when compile bridge cannot compile node op");
            const std::string bad_msg = bad_log.str();
            const bool explicit_fail_msg =
                (bad_msg.find("PlanSpec compile failed:") != std::string::npos) ||
                (bad_msg.find("PlanSpec runtime validation failed:") != std::string::npos);
            t.expect(explicit_fail_msg,
                "compile/validation failure path is explicit in custom runtime output");
            t.expect(bad_msg.find("Unsupported numeric op in multi-series mode") == std::string::npos,
                "legacy evaluator error signature is absent (fallback path unreachable)");
        }

        return true;
    }


} // namespace sugar::contract_suite_internal
