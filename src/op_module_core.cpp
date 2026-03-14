#include "op_modules.h"

namespace sugar::op_modules {

void register_core_ops(OperatorRegistry& r) {
    {
        OpEntry e;
        e.spec = OpSpec{
            "core.close",
            OpType::Numeric,
            {},
            ValueType::Num,
            {},
            "core",
            false,
            "0",
            OpAxisScope::RowWise,
            {}
        };
        e.warmup_num = [](const std::vector<std::size_t>&, const ResolvedParamMap&) { return std::size_t(0); };
        r.add(std::move(e));
    }

    {
        OpEntry e;
        OpParamSpec p; p.name = "value"; p.type = "float"; p.required = true;
        e.spec = OpSpec{
            "core.const",
            OpType::Numeric,
            {},
            ValueType::Num,
            {p},
            "core",
            false,
            "0",
            OpAxisScope::RowWise,
            {}
        };
        e.warmup_num = [](const std::vector<std::size_t>&, const ResolvedParamMap&) { return std::size_t(0); };
        r.add(std::move(e));
    }

    {
        OpEntry e;
        OpParamSpec p; p.name = "name"; p.type = "string"; p.required = true;
        e.spec = OpSpec{
            "core.precomputed_num",
            OpType::Numeric,
            {},
            ValueType::Num,
            {p},
            "core",
            false,
            "env-defined (typically 0)",
            OpAxisScope::RowWise,
            {}
        };
        e.warmup_num = [](const std::vector<std::size_t>&, const ResolvedParamMap&) { return std::size_t(0); };
        r.add(std::move(e));
    }

    {
        OpEntry e;
        OpParamSpec p; p.name = "ref"; p.type = "string"; p.required = true;
        e.spec = OpSpec{
            "core.ref",
            OpType::Numeric,
            {},
            ValueType::Num,
            {p},
            "core",
            false,
            "env-defined (typically 0)",
            OpAxisScope::RowWise,
            {}
        };
        e.warmup_num = [](const std::vector<std::size_t>&, const ResolvedParamMap&) { return std::size_t(0); };
        r.add(std::move(e));
    }

    {
        OpEntry e;
        OpParamSpec p; p.name = "value"; p.type = "bool"; p.required = true;
        e.spec = OpSpec{
            "core.bool_const",
            OpType::Bool,
            {},
            ValueType::Bool,
            {p},
            "core",
            false,
            "0",
            OpAxisScope::RowWise,
            {}
        };
        e.warmup_bool = [](const std::vector<std::size_t>&, const std::vector<std::size_t>&, const ResolvedParamMap&) { return std::size_t(0); };
        r.add(std::move(e));
    }

    {
        OpEntry e;
        OpParamSpec p; p.name = "name"; p.type = "string"; p.required = true;
        e.spec = OpSpec{
            "core.precomputed_bool",
            OpType::Bool,
            {},
            ValueType::Bool,
            {p},
            "core",
            false,
            "env-defined (typically 0)",
            OpAxisScope::RowWise,
            {}
        };
        e.warmup_bool = [](const std::vector<std::size_t>&, const std::vector<std::size_t>&, const ResolvedParamMap&) { return std::size_t(0); };
        r.add(std::move(e));
    }
}

} // namespace sugar::op_modules
