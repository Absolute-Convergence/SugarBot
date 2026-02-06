#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "metrics.h"
#include "series_view.h"
#include "indicators_composite_bool.h"

namespace sugar {

    // =============================================================
    // StrategyPlan: the "universal plug"
    //
    // A StrategyPlan is a pure description of strategy intent:
    // - Entry/exit signals (bool graphs)
    // - Trade mode (LongOnly / LongShort)
    // - Optional metadata and precomputed warm-up / first-valid index
    //
    // It deliberately contains NO execution logic.
    // Execution is handled by StrategyExecutor (FSM).
    // =============================================================

    class BoolSignal; // defined in indicators_composite_bool.h
    using BoolSignalPtr = std::shared_ptr<BoolSignal>;

    using BoolSeries = std::vector<std::uint8_t>; // 0/1, aligned to input series

    enum class TradeMode : std::uint8_t {
        LongOnly = 0,
        LongShort = 1
    };

    // Expandable bundle: add fields later (stops, sizing, filters, etc.)
    struct SignalSet {
        // Long regime
        BoolSignalPtr entry_long{};
        BoolSignalPtr exit_long{};

        // Short regime
        BoolSignalPtr entry_short{};
        BoolSignalPtr exit_short{};

        // Optional global gate (future ready): when false, suppress entries.
        BoolSignalPtr allow_trade{};
    };

    // Optional debug taps (future-ready). Not required for v1.
    struct DebugTap {
        std::string name;
        // Intentionally opaque here; the executor can compute these later if needed.
    };

    struct StrategyPlan {
        SignalSet signals{};
        TradeMode mode{ TradeMode::LongShort };

        // If set, executor can report "first usable signal date" using this index.
        // This avoids guessing from bool series (which intentionally suppress NaNs).
        static constexpr std::size_t npos = static_cast<std::size_t>(-1);
        std::size_t first_valid_index{ npos };

        std::string name{};
    };

    enum class FillTiming : std::uint8_t {
        SameBarClose = 0,
        NextBarOpen = 1
    };

    // How the executor interprets signal timing vs fill timing.
    // SameBar: evaluate signals at bar i and execute on bar i.
    // NextBar: evaluate signals at bar i (bar close) and execute on bar i+1.
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

        // Execution timing model
        BarEvalMode eval_mode{ BarEvalMode::SameBar };

        // Fill price selection
        FillTiming fill_timing{ FillTiming::SameBarClose };
        ExecCosts costs{};

        // Determinism knobs
        bool exits_first{ true };
        bool allow_same_bar_reversal{ false };

        // If true, force-close any open position on the final bar.
        bool close_out_at_end{ true };
    };

} // namespace sugar