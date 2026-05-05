#include "op_modules.h"

#include <algorithm>
#include <cstdint>
#include <functional>

namespace sugar::op_modules {

void register_ts_ops(OperatorRegistry& r) {
    auto add_unary_n = [&](const char* name,
        std::function<std::size_t(std::size_t, std::size_t)> warm,
        const char* warm_desc,
        const char* param_name) {

            OpEntry e;
            OpParamSpec p; p.name = param_name; p.type = "int"; p.required = true; p.min_num = 1;
            e.spec = OpSpec{
                name,
                OpType::Numeric,
                {ValueType::Num},
                ValueType::Num,
                {p},
                "ts",
                false,
                warm_desc,
                OpAxisScope::TimeSeries,
                {AxisKind::Time}
            };
            e.warmup_num = [warm, param_name](const std::vector<std::size_t>& cw, const ResolvedParamMap& pm) {
                const auto n = static_cast<std::size_t>(std::max<std::int64_t>(1, as_i64(pm, param_name).value_or(1)));
                return warm(cw.at(0), n);
                };
            r.add(std::move(e));
        };

    {
        OpEntry e;
        OpParamSpec p; p.name = "k"; p.type = "int"; p.required = true; p.min_num = 1;
        e.spec = OpSpec{
            "ts.roc",
            OpType::Numeric,
            {ValueType::Num},
            ValueType::Num,
            {p},
            "ts",
            false,
            "child_warmup + k",
            OpAxisScope::TimeSeries,
            {AxisKind::Time}
        };
        e.warmup_num = [](const std::vector<std::size_t>& cw, const ResolvedParamMap& pm) {
            const auto k = static_cast<std::size_t>(std::max<std::int64_t>(1, as_i64(pm, "k").value_or(1)));
            return cw.at(0) + k;
            };
        r.add(std::move(e));
    }

    add_unary_n("ts.sma", [](std::size_t child, std::size_t n) { return child + (n - 1); }, "child_warmup + (n-1)", "n");
    add_unary_n("ts.ema", [](std::size_t child, std::size_t n) { return child + (n - 1); }, "child_warmup + (n-1)", "n");
    add_unary_n("ts.stddev", [](std::size_t child, std::size_t n) { return child + (n - 1); }, "child_warmup + (n-1)", "n");
    add_unary_n("ts.stddev_ema", [](std::size_t child, std::size_t n) { return child + (2 * n - 2); }, "child_warmup + (2n-2)", "n");
    add_unary_n("ts.zscore_sma", [](std::size_t child, std::size_t n) { return child + (n - 1); }, "child_warmup + (n-1)", "n");
    add_unary_n("ts.zscore_ema", [](std::size_t child, std::size_t n) { return child + (2 * n - 2); }, "child_warmup + (2n-2)", "n");
    add_unary_n("ts.roll_max", [](std::size_t child, std::size_t n) { return child + (n - 1); }, "child_warmup + (n-1)", "n");
    add_unary_n("ts.roll_min", [](std::size_t child, std::size_t n) { return child + (n - 1); }, "child_warmup + (n-1)", "n");
    add_unary_n("ts.donchian_upper", [](std::size_t child, std::size_t n) { return child + (n - 1); }, "child_warmup + (n-1)", "n");
    add_unary_n("ts.donchian_lower", [](std::size_t child, std::size_t n) { return child + (n - 1); }, "child_warmup + (n-1)", "n");
    add_unary_n("ts.donchian_mid", [](std::size_t child, std::size_t n) { return child + (n - 1); }, "child_warmup + (n-1)", "n");
    add_unary_n("ts.donchian_width", [](std::size_t child, std::size_t n) { return child + (n - 1); }, "child_warmup + (n-1)", "n");

    {
        OpEntry e;
        OpParamSpec p1; p1.name = "n"; p1.type = "int"; p1.required = true; p1.min_num = 1;
        OpParamSpec p2; p2.name = "q"; p2.type = "float"; p2.required = true; p2.min_num = 0.0; p2.max_num = 100.0;
        e.spec = OpSpec{
            "ts.percentile",
            OpType::Numeric,
            {ValueType::Num},
            ValueType::Num,
            {p1, p2},
            "ts",
            false,
            "child_warmup + (n-1)",
            OpAxisScope::TimeSeries,
            {AxisKind::Time}
        };
        e.warmup_num = [](const std::vector<std::size_t>& cw, const ResolvedParamMap& pm) {
            const auto n = static_cast<std::size_t>(std::max<std::int64_t>(1, as_i64(pm, "n").value_or(1)));
            return cw.at(0) + (n - 1);
        };
        r.add(std::move(e));
    }

    {
        OpEntry e;
        OpParamSpec p1; p1.name = "n"; p1.type = "int"; p1.required = true; p1.min_num = 1;
        OpParamSpec p2; p2.name = "bins"; p2.type = "int"; p2.required = true; p2.min_num = 1;
        e.spec = OpSpec{
            "ts.entropy",
            OpType::Numeric,
            {ValueType::Num},
            ValueType::Num,
            {p1, p2},
            "ts",
            false,
            "child_warmup + (n-1)",
            OpAxisScope::TimeSeries,
            {AxisKind::Time}
        };
        e.warmup_num = [](const std::vector<std::size_t>& cw, const ResolvedParamMap& pm) {
            const auto n = static_cast<std::size_t>(std::max<std::int64_t>(1, as_i64(pm, "n").value_or(1)));
            return cw.at(0) + (n - 1);
        };
        r.add(std::move(e));
    }

    {
        OpEntry e;
        OpParamSpec p1; p1.name = "n"; p1.type = "int"; p1.required = true; p1.min_num = 2;
        OpParamSpec p2; p2.name = "lag"; p2.type = "int"; p2.required = true; p2.min_num = 1;
        e.spec = OpSpec{
            "ts.autocorrelation",
            OpType::Numeric,
            {ValueType::Num},
            ValueType::Num,
            {p1, p2},
            "ts",
            false,
            "child_warmup + (n-1)",
            OpAxisScope::TimeSeries,
            {AxisKind::Time}
        };
        e.warmup_num = [](const std::vector<std::size_t>& cw, const ResolvedParamMap& pm) {
            const auto n = static_cast<std::size_t>(std::max<std::int64_t>(2, as_i64(pm, "n").value_or(2)));
            return cw.at(0) + (n - 1);
        };
        r.add(std::move(e));
    }

    {
        OpEntry e;
        OpParamSpec p; p.name = "n"; p.type = "int"; p.required = true; p.min_num = 2;
        e.spec = OpSpec{
            "ts.hurst",
            OpType::Numeric,
            {ValueType::Num},
            ValueType::Num,
            {p},
            "ts",
            false,
            "child_warmup + (n-1)",
            OpAxisScope::TimeSeries,
            {AxisKind::Time}
        };
        e.warmup_num = [](const std::vector<std::size_t>& cw, const ResolvedParamMap& pm) {
            const auto n = static_cast<std::size_t>(std::max<std::int64_t>(2, as_i64(pm, "n").value_or(2)));
            return cw.at(0) + (n - 1);
        };
        r.add(std::move(e));
    }

    add_unary_n("ts.normalize_minmax", [](std::size_t child, std::size_t n) { return child + (n - 1); }, "child_warmup + (n-1)", "n");
    add_unary_n("ts.atr", [](std::size_t child, std::size_t n) { return child + n; }, "child_warmup + n", "n");
    add_unary_n("ts.rsi", [](std::size_t child, std::size_t n) { return child + n; }, "child_warmup + n", "n");
    add_unary_n("ts.atr_wilder", [](std::size_t child, std::size_t n) { return child + (n - 1); }, "child_warmup + (n-1)", "n");

    {
        OpEntry e;
        OpParamSpec p1; p1.name = "fast"; p1.type = "int"; p1.required = true; p1.min_num = 1;
        OpParamSpec p2; p2.name = "slow"; p2.type = "int"; p2.required = true; p2.min_num = 1;
        OpParamSpec p3; p3.name = "signal"; p3.type = "int"; p3.required = true; p3.min_num = 1;
        e.spec = OpSpec{
            "ts.macd",
            OpType::Numeric,
            {ValueType::Num},
            ValueType::Num,
            {p1, p2, p3},
            "ts",
            false,
            "child_warmup + max(fast,slow) + signal - 2",
            OpAxisScope::TimeSeries,
            {AxisKind::Time}
        };
        e.warmup_num = [](const std::vector<std::size_t>& cw, const ResolvedParamMap& pm) {
            const auto fast = static_cast<std::size_t>(std::max<std::int64_t>(1, as_i64(pm, "fast").value_or(1)));
            const auto slow = static_cast<std::size_t>(std::max<std::int64_t>(1, as_i64(pm, "slow").value_or(1)));
            const auto signal = static_cast<std::size_t>(std::max<std::int64_t>(1, as_i64(pm, "signal").value_or(1)));
            return cw.at(0) + std::max(fast, slow) + signal - 2;
        };
        r.add(std::move(e));
    }

    {
        OpEntry e;
        OpParamSpec p1; p1.name = "n"; p1.type = "int"; p1.required = true; p1.min_num = 1;
        OpParamSpec p2; p2.name = "k"; p2.type = "float"; p2.required = true; p2.min_num = 0.0;
        e.spec = OpSpec{
            "ts.bollinger_width",
            OpType::Numeric,
            {ValueType::Num},
            ValueType::Num,
            {p1, p2},
            "ts",
            false,
            "child_warmup + (n-1)",
            OpAxisScope::TimeSeries,
            {AxisKind::Time}
        };
        e.warmup_num = [](const std::vector<std::size_t>& cw, const ResolvedParamMap& pm) {
            const auto n = static_cast<std::size_t>(std::max<std::int64_t>(1, as_i64(pm, "n").value_or(1)));
            return cw.at(0) + (n - 1);
        };
        r.add(std::move(e));
    }

    {
        OpEntry e;
        OpParamSpec p; p.name = "n"; p.type = "int"; p.required = true; p.min_num = 2;
        e.spec = OpSpec{
            "ts.regime_change",
            OpType::Numeric,
            {ValueType::Num},
            ValueType::Num,
            {p},
            "ts",
            false,
            "child_warmup + (n-1)",
            OpAxisScope::TimeSeries,
            {AxisKind::Time}
        };
        e.warmup_num = [](const std::vector<std::size_t>& cw, const ResolvedParamMap& pm) {
            const auto n = static_cast<std::size_t>(std::max<std::int64_t>(2, as_i64(pm, "n").value_or(2)));
            return cw.at(0) + (n - 1);
        };
        r.add(std::move(e));
    }

    {
        OpEntry e;
        OpParamSpec p1; p1.name = "fast_n"; p1.type = "int"; p1.required = true; p1.min_num = 1;
        OpParamSpec p2; p2.name = "slow_n"; p2.type = "int"; p2.required = true; p2.min_num = 1;
        e.spec = OpSpec{
            "ts.volatility_ratio",
            OpType::Numeric,
            {ValueType::Num},
            ValueType::Num,
            {p1, p2},
            "ts",
            false,
            "child_warmup + (max(fast_n,slow_n)-1)",
            OpAxisScope::TimeSeries,
            {AxisKind::Time}
        };
        e.warmup_num = [](const std::vector<std::size_t>& cw, const ResolvedParamMap& pm) {
            const auto fast = static_cast<std::size_t>(std::max<std::int64_t>(1, as_i64(pm, "fast_n").value_or(1)));
            const auto slow = static_cast<std::size_t>(std::max<std::int64_t>(1, as_i64(pm, "slow_n").value_or(1)));
            return cw.at(0) + (std::max(fast, slow) - 1);
        };
        r.add(std::move(e));
    }

    {
        OpEntry e;
        OpParamSpec p1; p1.name = "k"; p1.type = "int"; p1.required = true; p1.min_num = 0;
        OpParamSpec p2; p2.name = "fill"; p2.type = "float"; p2.required = false;
        e.spec = OpSpec{
            "ts.shift",
            OpType::Numeric,
            {ValueType::Num},
            ValueType::Num,
            {p1, p2},
            "ts",
            false,
            "child_warmup + k",
            OpAxisScope::TimeSeries,
            {AxisKind::Time}
        };
        e.warmup_num = [](const std::vector<std::size_t>& cw, const ResolvedParamMap& pm) {
            const auto k = static_cast<std::size_t>(std::max<std::int64_t>(0, as_i64(pm, "k").value_or(0)));
            return cw.at(0) + k;
            };
        r.add(std::move(e));
    }

    {
        OpEntry e;
        e.spec = OpSpec{
            "ts.delta",
            OpType::Numeric,
            {ValueType::Num},
            ValueType::Num,
            {},
            "ts",
            false,
            "child_warmup + 1",
            OpAxisScope::TimeSeries,
            {AxisKind::Time}
        };
        e.warmup_num = [](const std::vector<std::size_t>& cw, const ResolvedParamMap&) {
            return cw.at(0) + 1;
            };
        r.add(std::move(e));
    }

    {
        OpEntry e;
        e.spec = OpSpec{
            "ts.true_range",
            OpType::Numeric,
            {ValueType::Num},
            ValueType::Num,
            {},
            "ts",
            false,
            "child_warmup + 1",
            OpAxisScope::TimeSeries,
            {AxisKind::Time}
        };
        e.warmup_num = [](const std::vector<std::size_t>& cw, const ResolvedParamMap&) {
            return cw.at(0) + 1;
            };
        r.add(std::move(e));
    }

    {
        OpEntry e;
        e.spec = OpSpec{
            "ts.true_range_ohlc",
            OpType::Numeric,
            {ValueType::Num, ValueType::Num, ValueType::Num},
            ValueType::Num,
            {},
            "ts",
            false,
            "max(child_warmups) + 1",
            OpAxisScope::TimeSeries,
            {AxisKind::Time}
        };
        e.warmup_num = [](const std::vector<std::size_t>& cw, const ResolvedParamMap&) {
            return std::max({ cw.at(0), cw.at(1), cw.at(2) }) + 1;
            };
        r.add(std::move(e));
    }

    auto add_unary0_kernel = [&](const char* name,
        const char* warm_desc) {
            OpEntry e;
            e.spec = OpSpec{
                name,
                OpType::Numeric,
                {ValueType::Num},
                ValueType::Num,
                {},
                "ts",
                false,
                warm_desc,
                OpAxisScope::RowWise,
                {}
            };
            e.warmup_num = [](const std::vector<std::size_t>& cw, const ResolvedParamMap&) { return cw.at(0); };
            r.add(std::move(e));
        };
    add_unary0_kernel("ts.neg", "child_warmup");
    add_unary0_kernel("ts.abs", "child_warmup");
    add_unary0_kernel("ts.log", "child_warmup");
    add_unary0_kernel("ts.exp", "child_warmup");
    add_unary0_kernel("ts.sqrt", "child_warmup");

    {
        OpEntry e;
        OpParamSpec p; p.name = "p"; p.type = "float"; p.required = true;
        e.spec = OpSpec{
            "ts.pow_const",
            OpType::Numeric,
            {ValueType::Num},
            ValueType::Num,
            {p},
            "ts",
            false,
            "child_warmup",
            OpAxisScope::RowWise,
            {}
        };
        e.warmup_num = [](const std::vector<std::size_t>& cw, const ResolvedParamMap&) { return cw.at(0); };
        r.add(std::move(e));
    }

    {
        OpEntry e;
        e.spec = OpSpec{
            "ts.normalize_minmax_global",
            OpType::Numeric,
            {ValueType::Num},
            ValueType::Num,
            {},
            "ts",
            false,
            "child_warmup",
            OpAxisScope::RowWise,
            {}
        };
        e.warmup_num = [](const std::vector<std::size_t>& cw, const ResolvedParamMap&) { return cw.at(0); };
        r.add(std::move(e));
    }

    {
        OpEntry e;
        e.spec = OpSpec{
            "ts.ratio_of_ma",
            OpType::Numeric,
            {ValueType::Num},
            ValueType::Num,
            {},
            "ts",
            false,
            "child_warmup",
            OpAxisScope::RowWise,
            {}
        };
        e.warmup_num = [](const std::vector<std::size_t>& cw, const ResolvedParamMap&) { return cw.at(0); };
        r.add(std::move(e));
    }

    auto add_bin_kernel = [&](const char* name,
        const char* warm_desc = "max(child_warmups)") {
            OpEntry e;
            e.spec = OpSpec{
                name,
                OpType::Numeric,
                {ValueType::Num, ValueType::Num},
                ValueType::Num,
                {},
                "ts",
                false,
                warm_desc,
                OpAxisScope::RowWise,
                {}
            };
            e.warmup_num = [](const std::vector<std::size_t>& cw, const ResolvedParamMap&) { return std::max(cw.at(0), cw.at(1)); };
            r.add(std::move(e));
        };

    add_bin_kernel("ts.add");
    add_bin_kernel("ts.sub");
    add_bin_kernel("ts.mul");
    add_bin_kernel("ts.div");
    add_bin_kernel("ts.min");
    add_bin_kernel("ts.max");

    {
        OpEntry e;
        OpParamSpec lo; lo.name = "lo"; lo.type = "float"; lo.required = true;
        OpParamSpec hi; hi.name = "hi"; hi.type = "float"; hi.required = true;
        e.spec = OpSpec{
            "ts.clamp_const",
            OpType::Numeric,
            {ValueType::Num},
            ValueType::Num,
            {lo, hi},
            "ts",
            false,
            "child_warmup",
            OpAxisScope::RowWise,
            {}
        };
        e.warmup_num = [](const std::vector<std::size_t>& cw, const ResolvedParamMap&) { return cw.at(0); };
        r.add(std::move(e));
    }

    {
        OpEntry e;
        e.spec = OpSpec{
            "ts.clamp",
            OpType::Numeric,
            {ValueType::Num, ValueType::Num, ValueType::Num},
            ValueType::Num,
            {},
            "ts",
            false,
            "max(child_warmups)",
            OpAxisScope::RowWise,
            {}
        };
        e.warmup_num = [](const std::vector<std::size_t>& cw, const ResolvedParamMap&) { return std::max({ cw.at(0), cw.at(1), cw.at(2) }); };
        r.add(std::move(e));
    }
}

} // namespace sugar::op_modules
