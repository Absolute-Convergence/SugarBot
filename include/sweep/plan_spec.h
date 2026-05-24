#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "sweep/strategy_plan.h" // TradeMode + ExecSettings
#include "sweep/metrics.h"
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
        SuccessiveHalving = 3,
        BayesianPosterior = 4,
    };

    enum class PlanSweepPosteriorSurrogateKind : std::uint8_t {
        GaussianProcess = 0,
    };

    enum class PlanSweepPosteriorAcquisitionKind : std::uint8_t {
        ExpectedImprovement = 0,
        ProbabilityOfImprovement = 1,
        UpperConfidenceBound = 2,
    };

    struct PlanSweepCoarseFineConfig {
        std::size_t k_stage1{ 25 };
        std::size_t k_stage2{ 50 };
        bool depollute_stage1{ true };
        bool dedupe_stage2{ true };
        std::size_t suppression_hops{ 1 };
        std::size_t stage2_hops{ 1 };
        std::size_t coarse_step_multiplier{ 0 }; // 0/1 => no per-dimension coarse mesh
        std::size_t stage1_stride{ 0 }; // 0 => no coarse subset
    };

    struct PlanSweepMonteCarloConfig {
        std::size_t draws{ 10000 };
        bool draws_explicit{ false };
        std::uint64_t seed{ 0 };     // 0 => random_device
        bool dedupe{ true };
        std::size_t top_k{ 0 };      // 0 => use PlanSweepSpec::top_k
    };

    struct PlanSweepSuccessiveHalvingConfig {
        // Number of adaptive rounds, including the initial proposal round.
        std::size_t rounds{ 3 };
        // Keep approximately 1 / reduction_factor of each round for promotion.
        std::size_t reduction_factor{ 2 };
        // 0 means derive a deterministic spread from top_k, rounds, and max_evals.
        std::size_t initial_candidates{ 0 };
        // 0/1 means no explicit flat stride; ignored when initial_candidates > 0.
        std::size_t initial_stride{ 0 };
        // Neighborhood expansion hops around promoted seeds for later rounds.
        std::size_t neighborhood_hops{ 1 };
        bool dedupe_candidates{ true };
        std::size_t top_k{ 0 };      // 0 => use PlanSweepSpec::top_k
    };

    struct PlanSweepBayesianPosteriorConfig {
        PlanSweepPosteriorSurrogateKind surrogate{
            PlanSweepPosteriorSurrogateKind::GaussianProcess
        };
        PlanSweepPosteriorAcquisitionKind acquisition{
            PlanSweepPosteriorAcquisitionKind::ExpectedImprovement
        };
        // 0 means derive the initial design from top_k and max_evals.
        std::size_t initial_design_size{ 0 };
        // 0 means derive the posterior proposal budget from max_evals.
        std::size_t iterations{ 0 };
        std::uint64_t seed{ 0 };     // 0 => deterministic default seed for E4
        double exploration_weight{ 2.0 };
        bool normalize_inputs{ true };
        std::size_t top_k{ 0 };      // 0 => use PlanSweepSpec::top_k
    };

    struct PlanSweepResultDiversityConfig {
        bool enabled{ false };
        bool dedupe_trade_hash{ true };
        bool basin_aware{ false };
        std::size_t oversample_multiplier{ 10 };
        std::size_t max_per_basin{ 1 };
        std::size_t param_bucket_size{ 4 };
        std::size_t min_param_distance{ 0 };
        std::size_t min_trades{ 0 };
    };

    struct PlanSweepParetoConfig {
        bool enabled{ false };
        bool use_result_diversity{ true };
        // Optional emitted-front-row depollution override. When absent, Pareto
        // may inherit PlanSweepSpec::result_diversity via use_result_diversity.
        std::optional<PlanSweepResultDiversityConfig> output_depollution{};
        std::vector<ObjectiveVectorComponentSpec> objectives{};
        std::size_t primary_sort_index{ 0 };
        // Caps emitted Pareto front rows only; 0 means uncapped.
        std::size_t front_row_limit{ 100 };
        // Compatibility alias for older PlanSpec text; parsed into front_row_limit.
        std::size_t retained_candidate_limit{ 100 };
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
        std::optional<RankingObjectiveSpec> objective_spec{};

        // Engine selector for custom strategy sweeps.
        PlanSweepEngineKind engine{ PlanSweepEngineKind::Exhaustive };
        PlanSweepCoarseFineConfig coarse_fine{};
        PlanSweepMonteCarloConfig monte_carlo{};
        PlanSweepSuccessiveHalvingConfig successive_halving{};
        PlanSweepBayesianPosteriorConfig bayesian_posterior{};
        PlanSweepResultDiversityConfig result_diversity{};
        PlanSweepParetoConfig pareto{};

        // Sweep ranges by param name.
        std::unordered_map<std::string, SweepParamRange> params;
    };

    enum class PlanRunScope : std::uint8_t {
        PrimarySeries = 0,
        PerEntityRanked = 1,
    };

    struct PlanRunSpec {
        PlanRunScope scope{ PlanRunScope::PrimarySeries };
        std::string target_entity{};
        std::optional<std::size_t> top_k{};
        std::optional<RankingObjectiveSpec> objective_spec{};
    };

    enum class PlanExecutionMode : std::uint8_t {
        Static = 0,
        DeclaredDynamic = 1,
    };

    struct PlanExecutionBoundsSpec {
        std::size_t max_routes{ 32 };
        std::size_t max_active_subgraphs{ 8 };
        std::size_t max_selected_outputs{ 16 };
        std::size_t decision_sample_limit{ 64 };
    };

    struct PlanExecutionSubgraphSpec {
        std::string id{};
        std::vector<std::string> nodes{};
        std::vector<std::string> depends_on{};
    };

    struct PlanExecutionRouteCaseSpec {
        std::string id{};
        bool when{ true };
        std::vector<std::string> activate_subgraphs{};
    };

    struct PlanExecutionRouteSpec {
        std::string id{};
        std::string condition{};
        std::vector<PlanExecutionRouteCaseSpec> cases{};
        std::vector<std::string> fallback_subgraphs{};
    };

    struct PlanExecutionOutputSpec {
        std::string id{};
        std::string node{};
    };

    struct PlanExecutionSpec {
        PlanExecutionMode mode{ PlanExecutionMode::Static };
        std::vector<PlanExecutionSubgraphSpec> subgraphs{};
        std::vector<PlanExecutionRouteSpec> routes{};
        std::vector<PlanExecutionOutputSpec> outputs{};
        PlanExecutionBoundsSpec bounds{};
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

        // Optional Milestone G declared dynamic execution scaffold. Absent or
        // static execution preserves the existing static PlanSpec behavior.
        std::optional<PlanExecutionSpec> execution;
    };

    std::string_view plan_run_scope_name(PlanRunScope scope);
    std::string_view plan_execution_mode_name(PlanExecutionMode mode);
    std::string_view plan_sweep_engine_kind_name(PlanSweepEngineKind kind);
    std::string_view plan_sweep_posterior_surrogate_kind_name(
        PlanSweepPosteriorSurrogateKind kind);
    std::string_view plan_sweep_posterior_acquisition_kind_name(
        PlanSweepPosteriorAcquisitionKind kind);

    bool parse_plan_spec_json(std::string_view json,
        PlanSpec& out,
        std::string& err);

    bool load_plan_spec_file(const std::string& path,
        PlanSpec& out,
        std::string& err);

    bool param_is_ref(const ParamValue& v);
    std::optional<std::string> param_ref_name(const ParamValue& v);

} // namespace sugar
