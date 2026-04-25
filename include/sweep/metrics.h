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

	struct ResultMetricValue {
		std::string name{};
		double value{};
		ResultMetricKind kind{ ResultMetricKind::CoreMetric };
		bool is_finite{ true };
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

		std::size_t long_trades{};								// 
		std::size_t short_trades{};								// 

		double pnl_long{};										// sum of % from long trades
		double pnl_short{};										// sum of % from short trades
		std::uint64_t trade_hash{};								// behavior signature
		std::size_t rows_evaluated{};							// rows seen by the executor on the active observation domain
		std::size_t warmup_rows{};								// leading rows skipped before the plan becomes execution-valid
		SignalCountSummary signal_counts{};						// counts of bound bool-signal activations over the row domain
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

	struct RunObjectiveSummary {
		bool requested{ false };
		RankingObjectiveSpec objective{};
		RunRankabilityState rankability{ RunRankabilityState::NotEvaluated };
		double score{ 0.0 };
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
        bool alignment_applied{ false };
        std::string alignment_calendar{};
        std::string alignment_missing{};
        std::string alignment_non_temporal_multi_series{};
        std::vector<std::string> topology_names{};
        std::size_t topology_usage_count{ 0 };
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
	std::string_view numeric_node_summary_mode_name(NumericNodeSummaryMode mode);
	std::string_view result_metric_kind_name(ResultMetricKind kind);
	std::string_view run_outcome_state_name(RunOutcomeState state);
	std::string_view run_rankability_state_name(RunRankabilityState state);
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
