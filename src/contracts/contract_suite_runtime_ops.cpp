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
#include "ingest/utils.h"
#include "ingest/run_manifest.h"
#include "ops/op_kernels_math.h"
#include "ops/op_kernels_ms.h"

namespace sugar::contract_suite_internal {

bool test_numeric_portability_contracts(ContractAsserts& t) {
        t.out << "[numeric_portability] cross-architecture numeric portability contracts\n";

        auto within_one_ulp = [](double actual, double expected) {
            if (!std::isfinite(actual) || !std::isfinite(expected)) {
                return actual == expected;
            }
            const double lower = std::nextafter(expected, -std::numeric_limits<double>::infinity());
            const double upper = std::nextafter(expected, std::numeric_limits<double>::infinity());
            return actual >= lower && actual <= upper;
        };
        auto exact_series_match = [](const std::vector<double>& lhs, const std::vector<double>& rhs) {
            if (lhs.size() != rhs.size()) return false;
            for (std::size_t i = 0; i < lhs.size(); ++i) {
                if (!equal_or_both_nonfinite(lhs[i], rhs[i], 0.0)) {
                    return false;
                }
            }
            return true;
        };

        const auto decimal_grid = make_drange(0.1, 0.5, 0.1, 16);
        const auto decimal_grid_repeat = make_drange(0.1, 0.5, 0.1, 16);
        t.expect(decimal_grid.size() == 5, "decimal indexed drange includes the expected endpoints");
        t.expect(exact_series_match(decimal_grid, decimal_grid_repeat),
            "decimal indexed drange is deterministic across repeated runs");
        if (decimal_grid.size() == 5) {
            t.expect(within_one_ulp(decimal_grid.front(), 0.1), "decimal indexed drange preserves the first endpoint");
            t.expect(within_one_ulp(decimal_grid.back(), 0.5), "decimal indexed drange preserves the inclusive stop");
        }

        const std::vector<double> monotonic = { 1.0, 2.0, 3.0, 4.0, 5.0 };
        const auto sma = op_kernels_math::sma(monotonic, 3);
        t.expect(sma.size() == monotonic.size(), "sma portability fixture returns the expected row count");
        if (sma.size() == monotonic.size()) {
            t.expect(std::isnan(sma[0]) && std::isnan(sma[1]), "sma warmup remains NaN before the window is filled");
            t.expect(within_one_ulp(sma[2], 2.0), "sma matches the expected first finite window within one ULP");
            t.expect(within_one_ulp(sma[3], 3.0), "sma matches the expected rolling midpoint within one ULP");
            t.expect(within_one_ulp(sma[4], 4.0), "sma matches the expected trailing window within one ULP");
        }

        const auto sd = op_kernels_math::stddev(monotonic, monotonic.size());
        t.expect(sd.size() == monotonic.size(), "stddev portability fixture returns the expected row count");
        if (sd.size() == monotonic.size()) {
            t.expect(std::isfinite(sd.back()), "stddev produces a finite output for a finite full window");
            if (std::isfinite(sd.back())) {
                t.expect(nearly_equal(sd.back(), std::sqrt(2.0), 1e-12),
                    "stddev matches the expected population standard deviation within epsilon");
            }
        }

        const std::vector<double> awkward = {
            1000000000000.125, 1000000000001.5, 1000000000003.0,
            999999999997.75, 1000000000002.25, 1000000000004.5,
            999999999999.875, 1000000000005.125
        };
        const auto sd_repeat_a = op_kernels_math::stddev(awkward, 4);
        const auto sd_repeat_b = op_kernels_math::stddev(awkward, 4);
        t.expect(exact_series_match(sd_repeat_a, sd_repeat_b),
            "stddev is deterministic across repeated runs on numerically awkward input");

        const auto corr_a = op_kernels::ms_correlation(awkward, awkward, 4);
        const auto corr_b = op_kernels::ms_correlation(awkward, awkward, 4);
        t.expect(exact_series_match(corr_a, corr_b),
            "ms.correlation is deterministic across repeated runs");
        t.expect(std::isfinite(corr_a.back()), "ms.correlation produces a finite self-correlation output");
        if (std::isfinite(corr_a.back())) {
            t.expect(nearly_equal(corr_a.back(), 1.0, 1e-12),
                "ms.correlation stays within epsilon of perfect self-correlation");
        }

        return true;
    }

bool test_rowwise_runtime_branching_contracts(ContractAsserts& t) {
        t.out << "[rowwise_runtime_branching] row-wise numeric branching contracts\n";

        const std::vector<double> close = {
            100.0, 104.0, 101.0, 105.0, 99.0, 106.0, 98.0, 107.0
        };
        std::shared_ptr<align::AlignedBundle> bundle;
        ColumnRef primary{};
        const bool okdv = make_single_series_dataview_from_close(close, bundle, primary);
        t.expect(okdv && bundle != nullptr, "row-wise branching test creates DataView input");
        if (!okdv || !bundle) return true;

        const DataView dv(*bundle);
        PlanSweepRuntimeOptions opt{};

        auto run_plan = [&](PlanSpec spec, BacktestResult& out, std::string& log_out) -> bool {
            std::ostringstream run_log;
            const bool ok = run_custom_plan_spec(dv, primary, spec, opt, run_log, &out);
            log_out = run_log.str();
            return ok;
        };

        PlanSpec select_spec{};
        select_spec.version = 1;
        select_spec.name = "contract_rowwise_core_select";
        select_spec.trade_mode = TradeMode::LongOnly;
        select_spec.numeric_nodes = {
            NodeSpec{ "c", "core.close", {}, {} },
            NodeSpec{ "thr", "core.const", {}, { {"value", 103.0} } },
            NodeSpec{ "pos", "core.const", {}, { {"value", 1.0} } },
            NodeSpec{ "neg", "core.const", {}, { {"value", -1.0} } },
            NodeSpec{ "zero", "core.const", {}, { {"value", 0.0} } },
            NodeSpec{ "x", "core.select", { "cond", "pos", "neg" }, {} },
        };
        select_spec.bool_nodes = {
            NodeSpec{ "cond", "cmp.gt", { "c", "thr" }, {} },
            NodeSpec{ "entry", "cmp.gt", { "x", "zero" }, {} },
            NodeSpec{ "exit", "cmp.lt", { "x", "zero" }, {} },
        };
        select_spec.bind.entry_long = "entry";
        select_spec.bind.exit_long = "exit";

        PlanSpec direct_spec{};
        direct_spec.version = 1;
        direct_spec.name = "contract_rowwise_direct_compare";
        direct_spec.trade_mode = TradeMode::LongOnly;
        direct_spec.numeric_nodes = {
            NodeSpec{ "c", "core.close", {}, {} },
            NodeSpec{ "thr", "core.const", {}, { {"value", 103.0} } },
        };
        direct_spec.bool_nodes = {
            NodeSpec{ "entry", "cmp.gt", { "c", "thr" }, {} },
            NodeSpec{ "exit", "cmp.lt", { "c", "thr" }, {} },
        };
        direct_spec.bind.entry_long = "entry";
        direct_spec.bind.exit_long = "exit";

        BacktestResult select_out{};
        BacktestResult direct_out{};
        std::string select_log;
        std::string direct_log;
        const bool select_ok = run_plan(select_spec, select_out, select_log);
        const bool direct_ok = run_plan(direct_spec, direct_out, direct_log);
        t.expect(select_ok, "core.select runtime contract executes");
        t.expect(direct_ok, "direct compare runtime contract executes");
        if (!select_ok) t.out << "    core.select error: " << select_log << "\n";
        if (!direct_ok) t.out << "    direct compare error: " << direct_log << "\n";
        if (select_ok && direct_ok) {
            t.expect(select_out.trades > 0, "core.select runtime contract produces trades");
            t.expect(backtest_equal(select_out, direct_out),
                "core.select remains runtime-equivalent to direct bool threshold routing");
        }

        return true;
    }

bool test_ts_alias_runtime_parity_contracts(ContractAsserts& t) {
        t.out << "[ts_alias_runtime_parity] TS runtime alias parity smoke contracts\n";

        std::vector<double> close;
        close.reserve(32);
        for (std::size_t i = 0; i < 32; ++i) {
            close.push_back(100.0 + static_cast<double>(i));
        }
        std::shared_ptr<align::AlignedBundle> bundle;
        ColumnRef primary{};
        const bool okdv = make_single_series_dataview_from_close(close, bundle, primary);
        t.expect(okdv && bundle != nullptr, "ts alias parity test creates DataView input");
        if (!okdv || !bundle) return true;

        const DataView dv(*bundle);
        PlanSweepRuntimeOptions opt{};

        auto run_case = [&](std::string_view op_name,
            const std::vector<std::string>& args,
            const ParamMap& params,
            double threshold,
            BacktestResult& out,
            std::string& log_out) -> bool {
                PlanSpec spec{};
                spec.version = 1;
                spec.name = "contract_ts_alias_" + std::string(op_name);
                spec.trade_mode = TradeMode::LongOnly;

                NodeSpec n_close{};
                n_close.id = "c";
                n_close.op = "core.close";

                NodeSpec n_zero{};
                n_zero.id = "z";
                n_zero.op = "core.const";
                n_zero.params["value"] = 0.0;

                NodeSpec n_one{};
                n_one.id = "one";
                n_one.op = "core.const";
                n_one.params["value"] = 1.0;

                NodeSpec n_two{};
                n_two.id = "two";
                n_two.op = "core.const";
                n_two.params["value"] = 2.0;

                NodeSpec n_thr{};
                n_thr.id = "thr";
                n_thr.op = "core.const";
                n_thr.params["value"] = threshold;

                NodeSpec n_out{};
                n_out.id = "x";
                n_out.op = std::string(op_name);
                n_out.args = args;
                n_out.params = params;

                spec.numeric_nodes = { n_close, n_zero, n_one, n_two, n_thr, n_out };

                NodeSpec b_entry{};
                b_entry.id = "entry";
                b_entry.op = "cmp.gt";
                b_entry.args = { "x", "thr" };

                NodeSpec b_exit{};
                b_exit.id = "exit";
                b_exit.op = "cmp.lt";
                b_exit.args = { "x", "thr" };

                spec.bool_nodes = { b_entry, b_exit };
                spec.bind.entry_long = "entry";
                spec.bind.exit_long = "exit";

                std::ostringstream run_log;
                const bool ok = run_custom_plan_spec(dv, primary, spec, opt, run_log, &out);
                log_out = run_log.str();
                return ok;
            };

        struct AliasCase {
            const char* alias;
            const char* canonical;
            std::vector<std::string> args;
            ParamMap params;
            double threshold;
        };

        const std::vector<AliasCase> cases = {
            { "ts.stdev", "ts.stddev", { "c" }, ParamMap{ {"n", static_cast<std::int64_t>(3)} }, 0.5 },
            { "ts.stdev_ema", "ts.stddev_ema", { "c" }, ParamMap{ {"n", static_cast<std::int64_t>(3)} }, 0.5 },
            { "ts.atr_w", "ts.atr_wilder", { "c" }, ParamMap{ {"n", static_cast<std::int64_t>(3)} }, 0.5 },
            { "ts.dc_upper", "ts.donchian_upper", { "c" }, ParamMap{ {"n", static_cast<std::int64_t>(3)} }, 110.5 },
            { "ts.dc_lower", "ts.donchian_lower", { "c" }, ParamMap{ {"n", static_cast<std::int64_t>(3)} }, 108.5 },
            { "ts.dc_mid", "ts.donchian_mid", { "c" }, ParamMap{ {"n", static_cast<std::int64_t>(3)} }, 109.5 },
            { "ts.dc_width", "ts.donchian_width", { "c" }, ParamMap{ {"n", static_cast<std::int64_t>(3)} }, 1.5 },
            { "ts.trange", "ts.true_range", { "c" }, ParamMap{}, 0.5 },
            { "ts.trange_ohlc", "ts.true_range_ohlc", { "c", "c", "c" }, ParamMap{}, 0.5 },
        };

        for (const auto& test_case : cases) {
            BacktestResult alias_result{};
            BacktestResult canonical_result{};
            std::string alias_log;
            std::string canonical_log;

            const bool alias_ok = run_case(
                test_case.alias,
                test_case.args,
                test_case.params,
                test_case.threshold,
                alias_result,
                alias_log);
            const bool canonical_ok = run_case(
                test_case.canonical,
                test_case.args,
                test_case.params,
                test_case.threshold,
                canonical_result,
                canonical_log);

            t.expect(alias_ok, std::string("alias runtime smoke executes '") + test_case.alias + "'");
            t.expect(canonical_ok, std::string("canonical runtime smoke executes '") + test_case.canonical + "'");
            if (!alias_ok) {
                t.out << "    alias runtime error (" << test_case.alias << "): " << alias_log << "\n";
            }
            if (!canonical_ok) {
                t.out << "    canonical runtime error (" << test_case.canonical << "): " << canonical_log << "\n";
            }
            if (alias_ok && canonical_ok) {
                t.expect(alias_result.trades > 0 && canonical_result.trades > 0,
                    std::string("alias parity case is signal-sensitive for '") + test_case.alias + "'");
                t.expect(backtest_equal(alias_result, canonical_result),
                    std::string("alias runtime parity holds for '") + test_case.alias +
                    "' -> '" + test_case.canonical + "'");
            }
        }

        return true;
    }

    bool test_ts_runtime_equivalence_smoke_contracts(ContractAsserts& t) {
        t.out << "[ts_runtime_equivalence_smoke] TS runtime representative equivalence smoke contracts\n";

        std::vector<double> close;
        close.reserve(32);
        for (std::size_t i = 0; i < 32; ++i) {
            close.push_back(100.0 + static_cast<double>(i));
        }
        std::shared_ptr<align::AlignedBundle> bundle;
        ColumnRef primary{};
        const bool okdv = make_single_series_dataview_from_close(close, bundle, primary);
        t.expect(okdv && bundle != nullptr, "ts runtime equivalence smoke creates DataView input");
        if (!okdv || !bundle) return true;

        const DataView dv(*bundle);
        PlanSweepRuntimeOptions opt{};

        auto run_plan = [&](PlanSpec spec, BacktestResult& out, std::string& log_out) -> bool {
            std::ostringstream run_log;
            const bool ok = run_custom_plan_spec(dv, primary, spec, opt, run_log, &out);
            log_out = run_log.str();
            return ok;
        };

        auto add_threshold_bindings = [](PlanSpec& spec, std::string threshold_node = "thr") {
            NodeSpec b_entry{};
            b_entry.id = "entry";
            b_entry.op = "cmp.gt";
            b_entry.args = { "x", std::move(threshold_node) };

            NodeSpec b_exit{};
            b_exit.id = "exit";
            b_exit.op = "cmp.lt";
            b_exit.args = { "x", "thr" };

            spec.bool_nodes = { b_entry, b_exit };
            spec.bind.entry_long = "entry";
            spec.bind.exit_long = "exit";
        };

        {
            PlanSpec neg_spec{};
            neg_spec.version = 1;
            neg_spec.name = "contract_ts_equiv_neg";
            neg_spec.trade_mode = TradeMode::LongOnly;
            neg_spec.numeric_nodes = {
                NodeSpec{ "c", "core.close", {}, {} },
                NodeSpec{ "minus_one", "core.const", {}, { {"value", -1.0} } },
                NodeSpec{ "thr", "core.const", {}, { {"value", -110.0} } },
                NodeSpec{ "x", "ts.neg", { "c" }, {} },
            };
            add_threshold_bindings(neg_spec);

            PlanSpec mul_spec{};
            mul_spec.version = 1;
            mul_spec.name = "contract_ts_equiv_mul";
            mul_spec.trade_mode = TradeMode::LongOnly;
            mul_spec.numeric_nodes = {
                NodeSpec{ "c", "core.close", {}, {} },
                NodeSpec{ "minus_one", "core.const", {}, { {"value", -1.0} } },
                NodeSpec{ "thr", "core.const", {}, { {"value", -110.0} } },
                NodeSpec{ "x", "ts.mul", { "c", "minus_one" }, {} },
            };
            add_threshold_bindings(mul_spec);

            BacktestResult neg_out{};
            BacktestResult mul_out{};
            std::string neg_log;
            std::string mul_log;
            const bool neg_ok = run_plan(neg_spec, neg_out, neg_log);
            const bool mul_ok = run_plan(mul_spec, mul_out, mul_log);
            t.expect(neg_ok, "ts.neg representative runtime equivalence case executes");
            t.expect(mul_ok, "ts.mul representative runtime equivalence case executes");
            if (!neg_ok) t.out << "    ts.neg equivalence error: " << neg_log << "\n";
            if (!mul_ok) t.out << "    ts.mul equivalence error: " << mul_log << "\n";
            if (neg_ok && mul_ok) {
                t.expect(neg_out.trades > 0, "ts.neg representative equivalence case produces trades");
                t.expect(backtest_equal(neg_out, mul_out),
                    "ts.neg remains runtime-equivalent to multiplying by -1");
            }
        }

        {
            PlanSpec clamp_const_spec{};
            clamp_const_spec.version = 1;
            clamp_const_spec.name = "contract_ts_equiv_clamp_const";
            clamp_const_spec.trade_mode = TradeMode::LongOnly;
            clamp_const_spec.numeric_nodes = {
                NodeSpec{ "c", "core.close", {}, {} },
                NodeSpec{ "lo", "core.const", {}, { {"value", -115.0} } },
                NodeSpec{ "hi", "core.const", {}, { {"value", -105.0} } },
                NodeSpec{ "thr", "core.const", {}, { {"value", -110.0} } },
                NodeSpec{ "neg", "ts.neg", { "c" }, {} },
                NodeSpec{ "x", "ts.clamp_const", { "neg" }, { {"lo", -115.0}, {"hi", -105.0} } },
            };
            add_threshold_bindings(clamp_const_spec);

            PlanSpec clamp_spec{};
            clamp_spec.version = 1;
            clamp_spec.name = "contract_ts_equiv_clamp";
            clamp_spec.trade_mode = TradeMode::LongOnly;
            clamp_spec.numeric_nodes = {
                NodeSpec{ "c", "core.close", {}, {} },
                NodeSpec{ "lo", "core.const", {}, { {"value", -115.0} } },
                NodeSpec{ "hi", "core.const", {}, { {"value", -105.0} } },
                NodeSpec{ "thr", "core.const", {}, { {"value", -110.0} } },
                NodeSpec{ "neg", "ts.neg", { "c" }, {} },
                NodeSpec{ "x", "ts.clamp", { "neg", "lo", "hi" }, {} },
            };
            add_threshold_bindings(clamp_spec);

            BacktestResult clamp_const_out{};
            BacktestResult clamp_out{};
            std::string clamp_const_log;
            std::string clamp_log;
            const bool clamp_const_ok = run_plan(clamp_const_spec, clamp_const_out, clamp_const_log);
            const bool clamp_ok = run_plan(clamp_spec, clamp_out, clamp_log);
            t.expect(clamp_const_ok, "ts.clamp_const representative runtime equivalence case executes");
            t.expect(clamp_ok, "ts.clamp representative runtime equivalence case executes");
            if (!clamp_const_ok) t.out << "    ts.clamp_const equivalence error: " << clamp_const_log << "\n";
            if (!clamp_ok) t.out << "    ts.clamp equivalence error: " << clamp_log << "\n";
            if (clamp_const_ok && clamp_ok) {
                t.expect(clamp_const_out.trades > 0, "ts.clamp representative equivalence case produces trades");
                t.expect(backtest_equal(clamp_const_out, clamp_out),
                    "ts.clamp_const remains runtime-equivalent to ts.clamp with constant bounds");
            }
        }

        {
            PlanSpec tr_ohlc_spec{};
            tr_ohlc_spec.version = 1;
            tr_ohlc_spec.name = "contract_ts_equiv_tr_ohlc";
            tr_ohlc_spec.trade_mode = TradeMode::LongOnly;
            tr_ohlc_spec.numeric_nodes = {
                NodeSpec{ "c", "core.close", {}, {} },
                NodeSpec{ "thr", "core.const", {}, { {"value", 0.5} } },
                NodeSpec{ "x", "ts.true_range_ohlc", { "c", "c", "c" }, {} },
            };
            add_threshold_bindings(tr_ohlc_spec);

            PlanSpec abs_delta_spec{};
            abs_delta_spec.version = 1;
            abs_delta_spec.name = "contract_ts_equiv_abs_delta";
            abs_delta_spec.trade_mode = TradeMode::LongOnly;
            abs_delta_spec.numeric_nodes = {
                NodeSpec{ "c", "core.close", {}, {} },
                NodeSpec{ "thr", "core.const", {}, { {"value", 0.5} } },
                NodeSpec{ "delta", "ts.delta", { "c" }, {} },
                NodeSpec{ "x", "ts.abs", { "delta" }, {} },
            };
            add_threshold_bindings(abs_delta_spec);

            BacktestResult tr_ohlc_out{};
            BacktestResult abs_delta_out{};
            std::string tr_ohlc_log;
            std::string abs_delta_log;
            const bool tr_ohlc_ok = run_plan(tr_ohlc_spec, tr_ohlc_out, tr_ohlc_log);
            const bool abs_delta_ok = run_plan(abs_delta_spec, abs_delta_out, abs_delta_log);
            t.expect(tr_ohlc_ok, "ts.true_range_ohlc representative runtime equivalence case executes");
            t.expect(abs_delta_ok, "ts.abs(ts.delta) representative runtime equivalence case executes");
            if (!tr_ohlc_ok) t.out << "    ts.true_range_ohlc equivalence error: " << tr_ohlc_log << "\n";
            if (!abs_delta_ok) t.out << "    ts.abs(ts.delta) equivalence error: " << abs_delta_log << "\n";
            if (tr_ohlc_ok && abs_delta_ok) {
                t.expect(tr_ohlc_out.trades > 0, "ts.true_range_ohlc representative equivalence case produces trades");
                const bool equivalent = backtest_equal(tr_ohlc_out, abs_delta_out);
                t.expect(equivalent,
                    "ts.true_range_ohlc remains runtime-equivalent to abs(delta) on identical OHLC inputs");
                if (!equivalent) {
                    t.out << "    ts.true_range_ohlc summary:\n";
                    print_backtest_summary(tr_ohlc_out, t.out, "      ");
                    t.out << "    ts.abs(ts.delta) summary:\n";
                    print_backtest_summary(abs_delta_out, t.out, "      ");
                }
            }
        }

        {
            PlanSpec mid_spec{};
            mid_spec.version = 1;
            mid_spec.name = "contract_ts_equiv_dc_mid";
            mid_spec.trade_mode = TradeMode::LongOnly;
            mid_spec.numeric_nodes = {
                NodeSpec{ "c", "core.close", {}, {} },
                NodeSpec{ "thr", "core.const", {}, { {"value", 109.5} } },
                NodeSpec{ "x", "ts.donchian_mid", { "c" }, { {"n", static_cast<std::int64_t>(3)} } },
            };
            add_threshold_bindings(mid_spec);

            PlanSpec formula_spec{};
            formula_spec.version = 1;
            formula_spec.name = "contract_ts_equiv_dc_formula";
            formula_spec.trade_mode = TradeMode::LongOnly;
            formula_spec.numeric_nodes = {
                NodeSpec{ "c", "core.close", {}, {} },
                NodeSpec{ "two", "core.const", {}, { {"value", 2.0} } },
                NodeSpec{ "thr", "core.const", {}, { {"value", 109.5} } },
                NodeSpec{ "upper", "ts.donchian_upper", { "c" }, { {"n", static_cast<std::int64_t>(3)} } },
                NodeSpec{ "lower", "ts.donchian_lower", { "c" }, { {"n", static_cast<std::int64_t>(3)} } },
                NodeSpec{ "sum", "ts.add", { "upper", "lower" }, {} },
                NodeSpec{ "x", "ts.div", { "sum", "two" }, {} },
            };
            add_threshold_bindings(formula_spec);

            BacktestResult mid_out{};
            BacktestResult formula_out{};
            std::string mid_log;
            std::string formula_log;
            const bool mid_ok = run_plan(mid_spec, mid_out, mid_log);
            const bool formula_ok = run_plan(formula_spec, formula_out, formula_log);
            t.expect(mid_ok, "ts.donchian_mid representative runtime equivalence case executes");
            t.expect(formula_ok, "donchian formula representative runtime equivalence case executes");
            if (!mid_ok) t.out << "    ts.donchian_mid equivalence error: " << mid_log << "\n";
            if (!formula_ok) t.out << "    donchian formula equivalence error: " << formula_log << "\n";
            if (mid_ok && formula_ok) {
                t.expect(mid_out.trades > 0, "ts.donchian_mid representative equivalence case produces trades");
                t.expect(backtest_equal(mid_out, formula_out),
                    "ts.donchian_mid remains runtime-equivalent to its upper/lower midpoint formula");
            }
        }

        {
            PlanSpec percentile_spec{};
            percentile_spec.version = 1;
            percentile_spec.name = "contract_ts_equiv_percentile";
            percentile_spec.trade_mode = TradeMode::LongOnly;
            percentile_spec.numeric_nodes = {
                NodeSpec{ "c", "core.close", {}, {} },
                NodeSpec{ "thr", "core.const", {}, { {"value", 111.5} } },
                NodeSpec{ "x", "ts.percentile", { "c" }, { {"n", static_cast<std::int64_t>(4)}, {"q", 100.0} } },
            };
            add_threshold_bindings(percentile_spec);

            PlanSpec roll_max_spec{};
            roll_max_spec.version = 1;
            roll_max_spec.name = "contract_ts_equiv_percentile_rollmax";
            roll_max_spec.trade_mode = TradeMode::LongOnly;
            roll_max_spec.numeric_nodes = {
                NodeSpec{ "c", "core.close", {}, {} },
                NodeSpec{ "thr", "core.const", {}, { {"value", 111.5} } },
                NodeSpec{ "x", "ts.roll_max", { "c" }, { {"n", static_cast<std::int64_t>(4)} } },
            };
            add_threshold_bindings(roll_max_spec);

            BacktestResult percentile_out{};
            BacktestResult roll_max_out{};
            std::string percentile_log;
            std::string roll_max_log;
            const bool percentile_ok = run_plan(percentile_spec, percentile_out, percentile_log);
            const bool roll_max_ok = run_plan(roll_max_spec, roll_max_out, roll_max_log);
            t.expect(percentile_ok, "ts.percentile representative runtime equivalence case executes");
            t.expect(roll_max_ok, "ts.roll_max percentile equivalence case executes");
            if (!percentile_ok) t.out << "    ts.percentile equivalence error: " << percentile_log << "\n";
            if (!roll_max_ok) t.out << "    ts.roll_max equivalence error: " << roll_max_log << "\n";
            if (percentile_ok && roll_max_ok) {
                t.expect(percentile_out.trades > 0, "ts.percentile representative equivalence case produces trades");
                t.expect(backtest_equal(percentile_out, roll_max_out),
                    "ts.percentile at q=100 remains runtime-equivalent to ts.roll_max");
            }
        }

        {
            PlanSpec macd_spec{};
            macd_spec.version = 1;
            macd_spec.name = "contract_ts_equiv_macd";
            macd_spec.trade_mode = TradeMode::LongOnly;
            macd_spec.numeric_nodes = {
                NodeSpec{ "c", "core.close", {}, {} },
                NodeSpec{ "thr", "core.const", {}, { {"value", -0.0001} } },
                NodeSpec{ "x", "ts.macd", { "c" }, {
                    {"fast", static_cast<std::int64_t>(3)},
                    {"slow", static_cast<std::int64_t>(6)},
                    {"signal", static_cast<std::int64_t>(3)}
                } },
            };
            add_threshold_bindings(macd_spec);

            PlanSpec formula_spec{};
            formula_spec.version = 1;
            formula_spec.name = "contract_ts_equiv_macd_formula";
            formula_spec.trade_mode = TradeMode::LongOnly;
            formula_spec.numeric_nodes = {
                NodeSpec{ "c", "core.close", {}, {} },
                NodeSpec{ "thr", "core.const", {}, { {"value", -0.0001} } },
                NodeSpec{ "fast", "ts.ema", { "c" }, { {"n", static_cast<std::int64_t>(3)} } },
                NodeSpec{ "slow", "ts.ema", { "c" }, { {"n", static_cast<std::int64_t>(6)} } },
                NodeSpec{ "line", "ts.sub", { "fast", "slow" }, {} },
                NodeSpec{ "signal", "ts.ema", { "line" }, { {"n", static_cast<std::int64_t>(3)} } },
                NodeSpec{ "x", "ts.sub", { "line", "signal" }, {} },
            };
            add_threshold_bindings(formula_spec);

            BacktestResult macd_out{};
            BacktestResult formula_out{};
            std::string macd_log;
            std::string formula_log;
            const bool macd_ok = run_plan(macd_spec, macd_out, macd_log);
            const bool formula_ok = run_plan(formula_spec, formula_out, formula_log);
            t.expect(macd_ok, "ts.macd representative runtime equivalence case executes");
            t.expect(formula_ok, "ts.macd formula equivalence case executes");
            if (!macd_ok) t.out << "    ts.macd equivalence error: " << macd_log << "\n";
            if (!formula_ok) t.out << "    ts.macd formula equivalence error: " << formula_log << "\n";
            if (macd_ok && formula_ok) {
                t.expect(macd_out.trades > 0, "ts.macd representative equivalence case produces trades");
                t.expect(backtest_equal(macd_out, formula_out),
                    "ts.macd remains runtime-equivalent to its EMA-difference histogram formula");
            }
        }

        {
            PlanSpec width_spec{};
            width_spec.version = 1;
            width_spec.name = "contract_ts_equiv_bollinger_width";
            width_spec.trade_mode = TradeMode::LongOnly;
            width_spec.numeric_nodes = {
                NodeSpec{ "c", "core.close", {}, {} },
                NodeSpec{ "thr", "core.const", {}, { {"value", 0.045} } },
                NodeSpec{ "x", "ts.bollinger_width", { "c" }, {
                    {"n", static_cast<std::int64_t>(5)},
                    {"k", 2.0}
                } },
            };
            add_threshold_bindings(width_spec);

            PlanSpec formula_spec{};
            formula_spec.version = 1;
            formula_spec.name = "contract_ts_equiv_bollinger_formula";
            formula_spec.trade_mode = TradeMode::LongOnly;
            formula_spec.numeric_nodes = {
                NodeSpec{ "c", "core.close", {}, {} },
                NodeSpec{ "thr", "core.const", {}, { {"value", 0.045} } },
                NodeSpec{ "mid", "ts.sma", { "c" }, { {"n", static_cast<std::int64_t>(5)} } },
                NodeSpec{ "mid_abs", "ts.abs", { "mid" }, {} },
                NodeSpec{ "sd", "ts.stddev", { "c" }, { {"n", static_cast<std::int64_t>(5)} } },
                NodeSpec{ "scale", "core.const", {}, { {"value", 4.0} } },
                NodeSpec{ "num", "ts.mul", { "sd", "scale" }, {} },
                NodeSpec{ "x", "ts.div", { "num", "mid_abs" }, {} },
            };
            add_threshold_bindings(formula_spec);

            BacktestResult width_out{};
            BacktestResult formula_out{};
            std::string width_log;
            std::string formula_log;
            const bool width_ok = run_plan(width_spec, width_out, width_log);
            const bool formula_ok = run_plan(formula_spec, formula_out, formula_log);
            t.expect(width_ok, "ts.bollinger_width representative runtime equivalence case executes");
            t.expect(formula_ok, "ts.bollinger_width formula equivalence case executes");
            if (!width_ok) t.out << "    ts.bollinger_width equivalence error: " << width_log << "\n";
            if (!formula_ok) t.out << "    ts.bollinger_width formula equivalence error: " << formula_log << "\n";
            if (width_ok && formula_ok) {
                t.expect(width_out.trades > 0, "ts.bollinger_width representative equivalence case produces trades");
                t.expect(backtest_equal(width_out, formula_out),
                    "ts.bollinger_width remains runtime-equivalent to its normalized band-width formula");
            }
        }

        {
            PlanSpec ratio_spec{};
            ratio_spec.version = 1;
            ratio_spec.name = "contract_ts_equiv_volatility_ratio";
            ratio_spec.trade_mode = TradeMode::LongOnly;
            ratio_spec.numeric_nodes = {
                NodeSpec{ "c", "core.close", {}, {} },
                NodeSpec{ "thr", "core.const", {}, { {"value", 0.45} } },
                NodeSpec{ "x", "ts.volatility_ratio", { "c" }, {
                    {"fast_n", static_cast<std::int64_t>(3)},
                    {"slow_n", static_cast<std::int64_t>(6)}
                } },
            };
            add_threshold_bindings(ratio_spec);

            PlanSpec formula_spec{};
            formula_spec.version = 1;
            formula_spec.name = "contract_ts_equiv_volatility_ratio_formula";
            formula_spec.trade_mode = TradeMode::LongOnly;
            formula_spec.numeric_nodes = {
                NodeSpec{ "c", "core.close", {}, {} },
                NodeSpec{ "thr", "core.const", {}, { {"value", 0.45} } },
                NodeSpec{ "fast", "ts.stddev", { "c" }, { {"n", static_cast<std::int64_t>(3)} } },
                NodeSpec{ "slow", "ts.stddev", { "c" }, { {"n", static_cast<std::int64_t>(6)} } },
                NodeSpec{ "x", "ts.div", { "fast", "slow" }, {} },
            };
            add_threshold_bindings(formula_spec);

            BacktestResult ratio_out{};
            BacktestResult formula_out{};
            std::string ratio_log;
            std::string formula_log;
            const bool ratio_ok = run_plan(ratio_spec, ratio_out, ratio_log);
            const bool formula_ok = run_plan(formula_spec, formula_out, formula_log);
            t.expect(ratio_ok, "ts.volatility_ratio representative runtime equivalence case executes");
            t.expect(formula_ok, "ts.volatility_ratio formula equivalence case executes");
            if (!ratio_ok) t.out << "    ts.volatility_ratio equivalence error: " << ratio_log << "\n";
            if (!formula_ok) t.out << "    ts.volatility_ratio formula equivalence error: " << formula_log << "\n";
            if (ratio_ok && formula_ok) {
                t.expect(ratio_out.trades > 0, "ts.volatility_ratio representative equivalence case produces trades");
                t.expect(backtest_equal(ratio_out, formula_out),
                    "ts.volatility_ratio remains runtime-equivalent to fast/slow stddev ratio");
            }
        }

        {
            PlanSpec corr_spec{};
            corr_spec.version = 1;
            corr_spec.name = "contract_ms_correlation_smoke";
            corr_spec.trade_mode = TradeMode::LongOnly;
            corr_spec.numeric_nodes = {
                NodeSpec{ "c", "core.close", {}, {} },
                NodeSpec{ "thr", "core.const", {}, { {"value", 0.9} } },
                NodeSpec{ "x", "ms.correlation", { "c", "c" }, { {"n", static_cast<std::int64_t>(4)} } },
            };
            add_threshold_bindings(corr_spec);

            BacktestResult corr_out{};
            std::string corr_log;
            const bool corr_ok = run_plan(corr_spec, corr_out, corr_log);
            t.expect(corr_ok, "ms.correlation runtime smoke executes");
            if (!corr_ok) {
                t.out << "    ms.correlation smoke error: " << corr_log << "\n";
            }
            else {
                t.expect(corr_out.trades > 0, "ms.correlation smoke produces trades on perfectly correlated windows");
            }
        }

        return true;
    }

} // namespace sugar::contract_suite_internal
