#pragma once

#include <array>
#include <string>
#include <string_view>

namespace sugar {

    struct PlanSpecRuntimeOpAlias {
        std::string_view alias;
        std::string_view canonical;
    };

    inline constexpr std::array<PlanSpecRuntimeOpAlias, 12> kPlanSpecRuntimeOpAliases{ {
        { "diff", "ms.diff" },
        { "ratio", "ms.ratio" },
        { "zscore", "ms.zscore" },
        { "ts.stdev", "ts.stddev" },
        { "ts.stdev_ema", "ts.stddev_ema" },
        { "ts.trange", "ts.true_range" },
        { "ts.trange_ohlc", "ts.true_range_ohlc" },
        { "ts.atr_w", "ts.atr_wilder" },
        { "ts.dc_upper", "ts.donchian_upper" },
        { "ts.dc_lower", "ts.donchian_lower" },
        { "ts.dc_mid", "ts.donchian_mid" },
        { "ts.dc_width", "ts.donchian_width" },
    } };

    inline std::string_view plan_spec_runtime_canonical_op(std::string_view op) {
        for (const auto& alias : kPlanSpecRuntimeOpAliases) {
            if (alias.alias == op) return alias.canonical;
        }
        if (op == "core.col" || op == "ms.col") {
            return "core.ref";
        }
        return {};
    }

    inline std::string normalize_plan_spec_runtime_op(std::string_view op) {
        const auto canonical = plan_spec_runtime_canonical_op(op);
        return canonical.empty() ? std::string(op) : std::string(canonical);
    }

} // namespace sugar
