#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "sweep/strategy_plan.h" // TradeMode + ExecSettings
#include "ingest/json_min.h"

namespace sugar {

    // -------------------------------------------------------------------------
    // PlanSpec (JSON)
    // -------------------------------------------------------------------------

    struct ParamRef {
        std::string name;
    };

    // ParamValue can be a literal or a reference to top-level params.
    using ParamValue = std::variant<std::nullptr_t, bool, std::int64_t, double, std::string, ParamRef>;

    using ParamMap = std::unordered_map<std::string, ParamValue>;

    struct NodeSpec {
        std::string id;
        std::string op;                 // e.g. "core.close", "ts.sma", "cmp.cross_up"
        std::vector<std::string> args;  // node IDs
        ParamMap params;                // op params
    };

    struct PlanBindings {
        std::string entry_long;
        std::string exit_long;
        std::string entry_short;
        std::string exit_short;
        std::string allow_trade;
    };

    // ---------------------------------------------------------------------
    // Optional sweep extension
    // ---------------------------------------------------------------------
    // A PlanSpec normally represents a single strategy instance.
    // If `sweep` is present, SugarBot will iterate over combinations of
    // parameter values, compile+run the plan for each combination, and report
    // Top-K results.
    //
    // This is intentionally "simple and explicit": ranges live in JSON,
    // and the engine reuses the normal PlanSpec compiler + StrategyExecutor.
    struct SweepParamRange {
        // If non-empty, these discrete values are used.
        // Otherwise, the [min,max] range is expanded by step.
        std::vector<double> values;
        double min{ 0.0 };
        double max{ 0.0 };
        double step{ 1.0 };
        bool is_int{ true }; // if true, values are written as int64_t into params
    };

    enum class PlanSweepEngineKind : std::uint8_t {
        Exhaustive = 0,
        CoarseFine = 1,
        MonteCarlo = 2,
    };

    struct PlanSweepCoarseFineConfig {
        std::size_t k_stage1{ 25 };
        std::size_t k_stage2{ 50 };
        bool depollute_stage1{ true };
        bool dedupe_stage2{ true };
        std::size_t suppression_hops{ 1 };
        std::size_t stage2_hops{ 1 };
        std::size_t stage1_stride{ 0 }; // 0 => no coarse subset
    };

    struct PlanSweepMonteCarloConfig {
        std::size_t draws{ 10000 };
        std::uint64_t seed{ 0 };     // 0 => random_device
        bool dedupe{ true };
        std::size_t top_k{ 0 };      // 0 => use PlanSweepSpec::top_k
    };

    struct PlanSweepSpec {
        bool enabled{ false };
        std::size_t top_k{ 10 };
        std::size_t max_evals{ 5000 };

        // objective:
        //  - "pnl"      : maximize pnl
        //  - "pnl_dd"   : maximize pnl - dd_weight * max_drawdown
        std::string objective{ "pnl" };
        double dd_weight{ 1.0 };

        // Engine selector for custom strategy sweeps.
        PlanSweepEngineKind engine{ PlanSweepEngineKind::Exhaustive };
        PlanSweepCoarseFineConfig coarse_fine{};
        PlanSweepMonteCarloConfig monte_carlo{};

        // Sweep ranges by param name.
        std::unordered_map<std::string, SweepParamRange> params;
    };

    enum class PlanRunScope : std::uint8_t {
        PrimarySeries = 0,
        PerEntityRanked = 1,
    };

    enum class PlanRunRankingMetric : std::uint8_t {
        Pnl = 0,
        PnlDd = 1,
    };

    struct PlanRunSpec {
        PlanRunScope scope{ PlanRunScope::PrimarySeries };
        std::string target_entity{};
        std::optional<std::size_t> top_k{};
        std::optional<PlanRunRankingMetric> ranking_metric{};
        std::optional<double> ranking_dd_weight{};
    };

    struct PlanSpec {
        int version{ 1 };
        std::string name;
        // Optional metadata set by load_plan_spec_file; empty when parsed from raw JSON text.
        std::string source_path;

        // Optional declaration of required registry modules.
        std::vector<std::string> required;

        // Plan-level defaults
        TradeMode trade_mode{ TradeMode::LongShort };
        ExecSettings exec{};

        // Top-level params (used by {"ref": "param"}). Values are JSON scalars.
        ParamMap params;

        // DAG nodes
        std::vector<NodeSpec> numeric_nodes;
        std::vector<NodeSpec> bool_nodes;

        // Which bool nodes map to StrategyPlan signals.
        PlanBindings bind;

        // Optional sweep configuration.
        std::optional<PlanSweepSpec> sweep;

        // Optional noninteractive runtime dispatch hints.
        std::optional<PlanRunSpec> run;
    };

    std::string_view plan_run_scope_name(PlanRunScope scope);
    std::string_view plan_run_ranking_metric_name(PlanRunRankingMetric metric);

    bool parse_plan_spec_json(std::string_view json,
        PlanSpec& out,
        std::string& err);

    bool load_plan_spec_file(const std::string& path,
        PlanSpec& out,
        std::string& err);

    bool param_is_ref(const ParamValue& v);
    std::optional<std::string> param_ref_name(const ParamValue& v);

} // namespace sugar
