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

} // namespace sugar::contract_suite_internal
