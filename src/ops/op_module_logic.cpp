#include "op_modules.h"

#include <algorithm>

namespace sugar::op_modules {

void register_logic_ops(OperatorRegistry& r) {
    auto add_unary_bool = [&](const char* name) {
        OpEntry e;
        e.spec = OpSpec{
            name,
            OpType::Bool,
            {ValueType::Bool},
            ValueType::Bool,
            {},
            "logic",
            false,
            "child_warmup",
            OpAxisScope::RowWise,
            {}
        };
        e.warmup_bool = [](const std::vector<std::size_t>& bw, const std::vector<std::size_t>&, const ResolvedParamMap&) { return bw.at(0); };
        r.add(std::move(e));
        };
    auto add_bin_bool = [&](const char* name, const char* warm_desc) {
        OpEntry e;
        e.spec = OpSpec{
            name,
            OpType::Bool,
            {ValueType::Bool, ValueType::Bool},
            ValueType::Bool,
            {},
            "logic",
            false,
            warm_desc,
            OpAxisScope::RowWise,
            {}
        };
        e.warmup_bool = [](const std::vector<std::size_t>& bw, const std::vector<std::size_t>&, const ResolvedParamMap&) { return std::max(bw.at(0), bw.at(1)); };
        r.add(std::move(e));
        };

    add_unary_bool("logic.not");
    add_bin_bool("logic.and", "max(child_warmups)");
    add_bin_bool("logic.or", "max(child_warmups)");
    add_bin_bool("logic.nand", "max(child_warmups)");
    add_bin_bool("logic.nor", "max(child_warmups)");

    auto add_cmp = [&](const char* name) {
        OpEntry e;
        e.spec = OpSpec{
            name,
            OpType::Bool,
            {ValueType::Num, ValueType::Num},
            ValueType::Bool,
            {},
            "cmp",
            false,
            "max(child_warmups)",
            OpAxisScope::RowWise,
            {}
        };
        e.warmup_bool = [](const std::vector<std::size_t>&, const std::vector<std::size_t>& nw, const ResolvedParamMap&) {
            return std::max(nw.at(0), nw.at(1));
            };
        r.add(std::move(e));
        };

    add_cmp("cmp.lt");
    add_cmp("cmp.le");
    add_cmp("cmp.gt");
    add_cmp("cmp.ge");
    add_cmp("cmp.eq");
    add_cmp("cmp.ne");

    auto add_cross = [&](const char* name) {
        OpEntry e;
        e.spec = OpSpec{
            name,
            OpType::Bool,
            {ValueType::Num, ValueType::Num},
            ValueType::Bool,
            {},
            "cmp",
            false,
            "max(child_warmups) + 1 (needs previous bar)",
            OpAxisScope::TimeSeries,
            {AxisKind::Time}
        };
        e.warmup_bool = [](const std::vector<std::size_t>&, const std::vector<std::size_t>& nw, const ResolvedParamMap&) {
            return std::max(nw.at(0), nw.at(1)) + 1;
            };
        r.add(std::move(e));
        };
    add_cross("cmp.cross_up");
    add_cross("cmp.cross_down");
}

} // namespace sugar::op_modules
