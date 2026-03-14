#pragma once

#include <iosfwd>
#include "data_view.h"
#include "strategy_plan.h" // TradeMode, ExecSettings

#include <string>

#include "plan_sweep.h"
#include "plan_spec.h"
#include "plan_compiler.h"
#include "op_registry.h"
#include "strategy_executor.h"

namespace sugar {

    enum class StrategySource {
        TemplateList = 1,
        CustomPlanSpec = 2,
    };

    enum class StrategyTemplate {
        Donchian_Breakout = 1,
        Gated_Diff_Cross = 2,
        ZScore_MeanReversion = 3,
        Swing_Breakout_Validation = 4,
    };

    struct StrategyRunOptions {
        TradeMode mode{ TradeMode::LongShort };

        ExecSettings donchian_exec{};
        ExecSettings gated_diff_exec{};
        ExecSettings zscore_exec{};
        ExecSettings swing_exec{};
    };

    struct CustomPlanSpecRunOptions {
        std::string plan_spec_path{};
        bool override_mode{ false };
        TradeMode mode{ TradeMode::LongShort };
        bool override_eval_mode{ false };
        ExecSettings exec{};
    };

    StrategySource prompt_strategy_source(std::istream& in, std::ostream& out);

    StrategyTemplate prompt_strategy_template(std::istream& in, std::ostream& out);

    TradeMode prompt_trade_mode(std::istream& in,
        std::ostream& out,
        TradeMode def);

    void prompt_donchian_sweep(std::istream& in,
        std::ostream& out,
        StrategyRunOptions& opt);

    void prompt_gated_diff_cross_sweep(std::istream& in,
        std::ostream& out,
        StrategyRunOptions& opt);

    void prompt_zscore_mean_reversion_sweep_ranges(std::istream& in,
        std::ostream& out,
        StrategyRunOptions& opt);

    void prompt_swing_breakout_validation_sweep(std::istream& in,
        std::ostream& out,
        StrategyRunOptions& opt);

    void prompt_custom_user_planspec(std::istream& in,
        std::ostream& out,
        CustomPlanSpecRunOptions& opt);

    bool load_custom_planspec_runtime(
        const CustomPlanSpecRunOptions& opt,
        PlanSpec& out_spec,
        PlanSweepRuntimeOptions& out_rt,
        std::string& err);

    // Load default PlanSpec reference for a built-in template from graphs/templates.
    // Returns false with explicit error if the template is not mapped or file loading fails.
    bool load_template_planspec(StrategyTemplate templ,
        PlanSpec& out_spec,
        std::string& err);

    bool run_strategy_template(const DataView& dv,
        const ColumnRef& primary,
        StrategyTemplate templ,
        const StrategyRunOptions& opt,
        std::ostream& out);

} // namespace sugar
