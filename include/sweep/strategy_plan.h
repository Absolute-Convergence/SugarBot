#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace sugar {

    struct StrategyPlanStorage;

    enum class TradeMode : std::uint8_t {
        LongOnly = 0,
        LongShort = 1
    };

    enum class FillTiming : std::uint8_t {
        SameBarClose = 0,
        NextBarOpen = 1
    };

    enum class BarEvalMode : std::uint8_t {
        SameBar = 0,
        NextBar = 1
    };

    struct ExecCosts {
        double fee_bps{ 0.0 };
        double slippage_bps{ 0.0 };
    };

    struct ExecSettings {
        double initial_equity{ 1.0 };
        BarEvalMode eval_mode{ BarEvalMode::SameBar };
        FillTiming fill_timing{ FillTiming::SameBarClose };
        ExecCosts costs{};
        bool exits_first{ true };
        bool allow_same_bar_reversal{ false };
        bool close_out_at_end{ true };
    };

    // Public execution contract:
    // - declarative identity + execution metadata
    // - opaque storage for the active compiled backend
    // - no graph-object types in stable headers
    // - `first_valid_index` is the execution warmup boundary derived from the
    //   bound strategy signal slots, not a blanket max over every compiled node
    struct StrategyPlan {
        static constexpr std::size_t npos = static_cast<std::size_t>(-1);

        std::string name{};
        TradeMode mode{ TradeMode::LongShort };
        std::size_t first_valid_index{ npos };
        std::shared_ptr<const StrategyPlanStorage> storage{};

        bool has_storage() const { return static_cast<bool>(storage); }
    };

} // namespace sugar
