#pragma once
#include <cmath>
#include <cstddef>
#include <ostream>
#include <string_view>
#include <cstdint>
#include <string>
#include <vector>
#include "ingest/time_key.h"

namespace sugar {

    struct RunManifest;

	enum class ObjectiveValueSource {
		ReportedMetric = 0,
		NumericNodeOutput = 1,
	};

	enum class ObjectiveDirection {
		Maximize = 0,
		Minimize = 1,
	};

	enum class NumericNodeSummaryMode {
		LastFiniteValue = 0,
	};

	struct NamedReportedValue {
		std::string name{};
		double value{};
	};

	struct ObjectiveValueSpec {
		ObjectiveValueSource source{ ObjectiveValueSource::ReportedMetric };
		std::string name{};
		NumericNodeSummaryMode summary_mode{ NumericNodeSummaryMode::LastFiniteValue };
	};

	struct ObjectivePenaltySpec {
		ObjectiveValueSpec value{};
		double weight{ 0.0 };
	};

	struct ObjectiveTieBreakSpec {
		ObjectiveValueSpec value{};
		ObjectiveDirection direction{ ObjectiveDirection::Maximize };
	};

    struct ObjectiveVectorComponentSpec {
        std::string label{};
        ObjectiveValueSpec value{};
        ObjectiveDirection direction{ ObjectiveDirection::Maximize };
    };

	struct RankingObjectiveSpec {
		ObjectiveValueSpec primary{};
		ObjectiveDirection direction{ ObjectiveDirection::Maximize };
		std::vector<ObjectivePenaltySpec> penalties{};
		std::vector<ObjectiveTieBreakSpec> tie_breaks{};
	};

	enum class ResultMetricKind {
		CoreMetric = 0,
		SignalCount = 1,
		ReportedValue = 2,
	};

    struct SignalCountSummary {
        std::size_t allow_trade{ 0 };
        std::size_t entry_long{ 0 };
        std::size_t exit_long{ 0 };
        std::size_t entry_short{ 0 };
        std::size_t exit_short{ 0 };
    };

    struct RunDynamicRouteCaseSummary {
        std::string id{};
        bool when{ true };
        std::vector<std::string> activate_subgraphs{};
        std::size_t decisions{ 0 };
    };

    struct RunDynamicRouteSummary {
        std::string id{};
        std::string condition_node_id{};
        bool condition_is_constant{ false };
        bool constant_condition_value{ false };
        std::vector<RunDynamicRouteCaseSummary> cases{};
        std::vector<std::string> fallback_subgraphs{};
        std::size_t true_decisions{ 0 };
        std::size_t false_decisions{ 0 };
        std::size_t fallback_decisions{ 0 };
    };

    struct RunDynamicDecisionSample {
        std::size_t row{ 0 };
        std::string route_id{};
        std::string selected_case_id{};
        bool fallback_used{ false };
        bool condition_value{ false };
        std::vector<std::string> active_subgraphs{};
    };

    struct RunDynamicOutputSummary {
        std::string id{};
        std::string node_id{};
        std::string owning_subgraph{};
        std::size_t active_rows{ 0 };
        std::size_t inactive_rows{ 0 };
    };

    struct RunDynamicExecutionSummary {
        bool enabled{ false };
        std::string mode{};
        std::vector<std::string> subgraph_ids{};
        std::vector<RunDynamicRouteSummary> routes{};
        std::vector<RunDynamicOutputSummary> outputs{};
        std::vector<RunDynamicDecisionSample> decision_samples{};
        std::size_t total_decisions{ 0 };
        std::size_t emitted_decision_samples{ 0 };
        std::size_t suppressed_decisions{ 0 };
        std::size_t decision_sample_limit{ 0 };
        std::size_t fallback_decisions{ 0 };
        std::size_t inactive_output_rows{ 0 };
        std::size_t inactive_node_rows{ 0 };
        std::size_t max_active_subgraphs_per_row{ 0 };
        std::size_t max_active_subgraph_cap{ 0 };
        bool bounded_by_declared_caps{ true };
        std::uint64_t route_decision_digest{ 0 };
    };

	struct ResultMetricValue {
		std::string name{};
		double value{};
		ResultMetricKind kind{ ResultMetricKind::CoreMetric };
		bool is_finite{ true };
        bool has_exact_u64_value{ false };
        std::uint64_t exact_u64_value{ 0 };
	};

	struct BacktestMetricCatalog {
		std::vector<ResultMetricValue> values{};
		std::vector<std::string> non_finite_metric_names{};
	};

	struct BacktestResult {
		double pnl{};											// cumulative % return across closed trades
		std::size_t trades{};									// round-trip trades
		double max_drawdown{};									// max peak-to-trough drawdown (%)
		TimeKey best_start_date{};								// first usable signal TimeKey
		bool has_best_start_date{ false };						// true if best_start_date is set

		std::size_t long_trades{};
		std::size_t short_trades{};

		double pnl_long{};										// sum of % from long trades
		double pnl_short{};										// sum of % from short trades
		std::uint64_t trade_hash{};								// behavior signature
		std::size_t rows_evaluated{};							// rows seen by the executor on the active observation domain
		std::size_t warmup_rows{};								// leading rows skipped before the plan becomes execution-valid
		SignalCountSummary signal_counts{};						// counts of bound bool-signal activations over the row domain
        RunDynamicExecutionSummary dynamic_execution{};          // declared dynamic DAG route/report diagnostics
		std::vector<NamedReportedValue> reported_values{};		// named extra values preserved from the evaluated logic
	};

	enum class RunOutcomeState {
		Completed = 0,
		CompletedWithNonFiniteMetrics = 1,
        Failed = 2,
        Canceled = 3,
	};

	enum class RunRankabilityState {
		NotEvaluated = 0,
		Rankable = 1,
		MissingPrimaryValue = 2,
		NonFinitePrimaryValue = 3,
		MissingPenaltyValue = 4,
		NonFinitePenaltyValue = 5,
	};

    enum class RunObjectiveComponentRole {
        Primary = 0,
        Penalty = 1,
        TieBreak = 2,
    };

    struct RunObjectiveComponent {
        RunObjectiveComponentRole role{ RunObjectiveComponentRole::Primary };
        ObjectiveValueSpec value{};
        ObjectiveDirection direction{ ObjectiveDirection::Maximize };
        double weight{ 0.0 };
        bool present{ false };
        bool finite{ false };
        double raw_value{ 0.0 };
        double score_contribution{ 0.0 };
    };

	struct RunObjectiveSummary {
		bool requested{ false };
		RankingObjectiveSpec objective{};
		RunRankabilityState rankability{ RunRankabilityState::NotEvaluated };
		double score{ 0.0 };
        std::vector<RunObjectiveComponent> components{};
	};

    struct RunOutcomeCounts {
        std::size_t completed{ 0 };
        std::size_t completed_with_non_finite_metrics{ 0 };
        std::size_t failed{ 0 };
        std::size_t canceled{ 0 };
    };

    struct RunRankabilityCounts {
        std::size_t not_evaluated{ 0 };
        std::size_t rankable{ 0 };
        std::size_t missing_primary_value{ 0 };
        std::size_t non_finite_primary_value{ 0 };
        std::size_t missing_penalty_value{ 0 };
        std::size_t non_finite_penalty_value{ 0 };
    };

    struct RunTopologyArtifactSummary {
        bool present{ false };
        std::string stable_id{};
        std::string locator{};
        std::string format{};
        std::string digest{};
        std::string producer{};
    };

    struct RunTopologySummary {
        std::string name{};
        std::string origin{};
        std::string bearing{};
        std::string family{};
        std::string domain_scope{};
        std::vector<std::string> query_kinds{};
        bool directed{ false };
        bool weighted{ false };
        std::size_t node_count{ 0 };
        std::size_t edge_count{ 0 };
        std::size_t time_valid_edge_count{ 0 };
        std::size_t versioned_edge_count{ 0 };
        std::size_t semantic_embedding_count{ 0 };
        RunTopologyArtifactSummary artifact{};
    };

    struct RunTopologyUsageSummary {
        std::string subject_kind{};
        std::string subject_id{};
        std::string op_name{};
        std::string topology_name{};
        std::string topology_origin{};
        std::string topology_bearing{};
        std::string locality_subject_kind{};
        std::string locality_subject_value{};
        bool locality_subject_explicit{ false };
        std::string resolved_series_key{};
        std::string resolved_entity_key{};
        std::vector<std::string> required_topology_families{};
        std::vector<std::string> required_topology_query_kinds{};
        bool catalog_present{ false };
        bool runtime_resolved{ false };
        RunTopologyArtifactSummary artifact{};
    };

    inline constexpr std::size_t kRunTopologyDiagnosticsTopologyLimit = 32;
    inline constexpr std::size_t kRunTopologyDiagnosticsUsageLimit = 64;

    struct RunProvenanceSummary {
        std::string primary_series_key{};
        std::string primary_selection_mode{};
        std::string time_kind{};
        std::string time_resolution{};
        std::size_t series_count{ 0 };
        bool has_time_axis{ false };
        bool has_entity_axis{ false };
        bool has_spatial_axis{ false };
        std::size_t input_warning_count{ 0 };
        bool csv_had_header{ false };
        std::size_t csv_physical_line_count{ 0 };
        std::size_t csv_blank_line_count{ 0 };
        std::size_t csv_data_row_count{ 0 };
        std::size_t dropped_row_count{ 0 };
        std::vector<std::string> input_warnings{};
        bool alignment_applied{ false };
        std::string alignment_calendar{};
        std::string alignment_missing{};
        std::string alignment_non_temporal_multi_series{};
        std::vector<std::string> topology_names{};
        std::size_t topology_usage_count{ 0 };
        std::vector<RunTopologySummary> topologies{};
        std::vector<RunTopologyUsageSummary> topology_usages{};
    };

	struct RunReport {
		BacktestResult execution{};
		BacktestMetricCatalog metric_catalog{};
		RunOutcomeState outcome{ RunOutcomeState::Completed };
		RunObjectiveSummary objective{};
        RunProvenanceSummary provenance{};
	};

	std::string_view objective_value_source_name(ObjectiveValueSource source);
	std::string_view objective_direction_name(ObjectiveDirection direction);
    std::string objective_vector_component_label(
        const ObjectiveVectorComponentSpec& objective);
	std::string_view numeric_node_summary_mode_name(NumericNodeSummaryMode mode);
	std::string_view result_metric_kind_name(ResultMetricKind kind);
	std::string_view run_outcome_state_name(RunOutcomeState state);
	std::string_view run_rankability_state_name(RunRankabilityState state);
    std::string_view run_objective_component_role_name(RunObjectiveComponentRole role);
    RunProvenanceSummary build_run_provenance_summary(const RunManifest& manifest);

	RunReport build_run_report(
		const BacktestResult& result,
		const RankingObjectiveSpec* objective = nullptr,
        const RunProvenanceSummary* provenance = nullptr,
        RunOutcomeState outcome = RunOutcomeState::Completed);

	const ResultMetricValue* find_metric_value(
		const RunReport& report,
		std::string_view name);

	bool try_lookup_reported_value(
		const BacktestResult& result,
		const ObjectiveValueSpec& value,
		double& out);

	bool ranking_objective_prefers_left(
		const RankingObjectiveSpec& objective,
		const BacktestResult& left,
		const BacktestResult& right);

	double evaluate_ranking_objective(
		const RankingObjectiveSpec& objective,
		const BacktestResult& result);

	std::string describe_ranking_objective(const RankingObjectiveSpec& objective);

    void increment_run_outcome_counts(
        RunOutcomeCounts& counts,
        RunOutcomeState state);

    void increment_run_rankability_counts(
        RunRankabilityCounts& counts,
        RunRankabilityState state);

    void print_run_provenance_summary(
        const RunProvenanceSummary& provenance,
        std::ostream& os,
        std::string_view indent = {});

	void print_run_report_summary(
		const RunReport& report,
		std::ostream& os,
		std::string_view indent = {});

	// Compatibility formatter while reporting callers are migrated to RunReport.
	void print_backtest_summary(
		const BacktestResult& r,
		std::ostream& os,
		std::string_view indent = {});

} // namespace sugar
