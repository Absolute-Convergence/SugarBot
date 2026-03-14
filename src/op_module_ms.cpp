#include "op_modules.h"

#include <algorithm>
#include <cstdint>

namespace sugar::op_modules {

void register_ms_ops(OperatorRegistry& r) {
    {
        OpEntry e;
        e.spec = OpSpec{
            "ms.diff",
            OpType::Numeric,
            {ValueType::Num, ValueType::Num},
            ValueType::Num,
            {},
            "ms",
            false,
            "max(child_warmups)",
            OpAxisScope::CrossSeries,
            {}
        };
        e.warmup_num = [](const std::vector<std::size_t>& cw, const ResolvedParamMap&) {
            return std::max(cw.at(0), cw.at(1));
            };
        r.add(std::move(e));
    }

    {
        OpEntry e;
        e.spec = OpSpec{
            "ms.ratio",
            OpType::Numeric,
            {ValueType::Num, ValueType::Num},
            ValueType::Num,
            {},
            "ms",
            false,
            "max(child_warmups)",
            OpAxisScope::CrossSeries,
            {}
        };
        e.warmup_num = [](const std::vector<std::size_t>& cw, const ResolvedParamMap&) {
            return std::max(cw.at(0), cw.at(1));
            };
        r.add(std::move(e));
    }

    {
        OpEntry e;
        OpParamSpec p; p.name = "n"; p.type = "int"; p.required = false; p.default_value = static_cast<std::int64_t>(20); p.min_num = 1;
        e.spec = OpSpec{
            "ms.zscore",
            OpType::Numeric,
            {ValueType::Num},
            ValueType::Num,
            {p},
            "ms",
            false,
            "child_warmup + (n-1)",
            OpAxisScope::TimeSeries,
            {AxisKind::Time}
        };
        e.warmup_num = [](const std::vector<std::size_t>& cw, const ResolvedParamMap& pm) {
            const std::size_t n = static_cast<std::size_t>(std::max<std::int64_t>(1, as_i64(pm, "n").value_or(20)));
            return cw.at(0) + (n - 1);
            };
        r.add(std::move(e));
    }
}

} // namespace sugar::op_modules
