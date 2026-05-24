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
#include <string_view>
#include <utility>
#include <vector>

#include "op_registry_internal.h"
#include "ops/op_registry.h"
#include "sweep/evaluation_report.h"
#include "sweep/panel_runner.h"
#include "sweep/plan_capability.h"
#include "sweep/sweep.h"
#include "engine/plan_compiler.h"
#include "sweep/plan_sweep.h"
#include "engine/program_evaluator.h"
#include "ingest/utils.h"
#include "ingest/run_manifest.h"

namespace sugar::contract_suite_internal {

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
