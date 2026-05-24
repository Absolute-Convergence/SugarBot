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

} // namespace sugar::contract_suite_internal
