#include "contract_suite_runtime_bridge_sweep.h"
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

    bool run_compile_bridge_sweep_grid_contracts(ContractAsserts& t) {
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
            if (!okdv || !bundle) return false;

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

        return true;
    }

} // namespace sugar::contract_suite_internal
