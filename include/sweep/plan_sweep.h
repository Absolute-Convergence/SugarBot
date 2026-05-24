#pragma once

#include <cstdint>
#include <functional>
#include <iosfwd>
#include <string>
#include <vector>

#include "ingest/data_view.h"
#include "sweep/metrics.h"
#include "sweep/plan_spec.h"
#include "sweep/sweep.h"
#include "sweep/strategy_plan.h"

namespace sugar {

    struct PlanSweepRuntimeOptions {
        bool override_mode{ false };
        TradeMode custom_mode{ TradeMode::LongShort };
        bool override_eval_mode{ false };
        BarEvalMode eval_mode{ BarEvalMode::SameBar };
        std::size_t ostream_progress_throttle_ms{ 250 };
        SweepProgressCallback progress_cb{};
        SweepCancellationCallback cancel_requested{};
    };

    struct SweepParameterRangeSummary {
        std::string name{};
        SweepParamRange requested_range{};
        std::size_t expanded_count = 0;
    };

    struct SweepStageSummary {
        SweepProgressStage stage{ SweepProgressStage::CandidateEvaluation };
        std::size_t done = 0;
        std::size_t total = 0;
    };

    struct SweepTraceSeedRecord {
        std::string source{};
        std::string stream_name{ "default" };
        bool requested_seed_explicit{ false };
        std::uint64_t requested_seed{ 0 };
        std::uint64_t effective_seed{ 0 };
        bool deterministic{ true };
    };

    enum class SweepTracePolicyConfigValueKind {
        String = 0,
        Boolean = 1,
        UnsignedInteger = 2,
        SignedInteger = 3,
        FloatingPoint = 4,
    };

    struct SweepTracePolicyConfigEntry {
        std::string name{};
        SweepTracePolicyConfigValueKind value_kind{
            SweepTracePolicyConfigValueKind::String
        };
        std::string value{};
    };

    struct SweepTraceSummary {
        bool captured{ false };
        std::string policy_name{};
        std::vector<SweepTracePolicyConfigEntry> effective_policy_config{};
        std::size_t event_limit{ 0 };
        std::size_t total_event_count{ 0 };
        std::size_t proposal_count{ 0 };
        std::size_t observation_count{ 0 };
        std::size_t promotion_count{ 0 };
        std::size_t rejection_count{ 0 };
        bool events_capped{ false };
        bool full_event_stream_captured{ false };
        bool trace_digest_available{ false };
        std::uint64_t trace_digest{ 0 };
        bool event_order_valid{ false };
        std::vector<std::string> event_order_violations{};
        bool observation_payload_complete{ false };
        std::vector<std::string> observation_payload_gaps{};
        bool exact_replay_from_report{ false };
        std::vector<std::string> non_replayable_reasons{};
        std::vector<SweepTraceSeedRecord> seeds{};
        std::vector<SweepTraceEvent> events{};
    };

    struct SweepResultDiversitySummary {
        bool requested{ false };
        bool dedupe_trade_hash{ false };
        bool basin_aware{ false };
        std::size_t oversample_multiplier{ 1 };
        std::size_t max_per_basin{ 0 };
        std::size_t param_bucket_size{ 0 };
        std::size_t min_param_distance{ 0 };
        std::size_t min_trades{ 0 };
        std::size_t retained_candidate_pool{ 0 };
        std::size_t returned_unique_trade_hashes{ 0 };
        std::size_t returned_duplicate_trade_hash_groups{ 0 };
        std::size_t returned_largest_trade_hash_group{ 0 };
        std::size_t returned_basin_count{ 0 };
        std::size_t suppressed_duplicate_trade_hash{ 0 };
        std::size_t suppressed_basin{ 0 };
        std::size_t suppressed_min_trades{ 0 };
    };

    struct SweepCandidateReport {
        std::size_t rank{ 0 };
        std::size_t candidate_index{ 0 };
        double score{ 0.0 };
        std::vector<SweepParameterSetting> parameter_values{};
        RunReport run{};
        std::vector<std::string> diagnostics{};
    };

    enum class SweepSearchGeometrySource {
        None = 0,
        RetainedCandidatePool = 1,
        ExplicitNeighborhoodProbe = 2,
        CollectorTrace = 3,
    };

    struct SweepBasinSummary {
        std::string key{};
        std::vector<std::size_t> bucket_coords{};
        std::size_t candidate_count{ 0 };
        double retained_pool_density{ 0.0 };
        std::size_t returned_candidate_count{ 0 };
        std::size_t best_candidate_index{ 0 };
        std::size_t best_rank{ 0 };
        double best_score{ 0.0 };
        double mean_score{ 0.0 };
        double worst_score{ 0.0 };
        double score_range{ 0.0 };
        std::uint64_t best_trade_hash{ 0 };
        std::vector<SweepParameterSetting> best_parameter_values{};
    };

    struct SweepDuplicateTradePathGroup {
        std::uint64_t trade_hash{ 0 };
        std::size_t candidate_count{ 0 };
        std::size_t returned_candidate_count{ 0 };
        std::size_t best_candidate_index{ 0 };
        std::size_t best_rank{ 0 };
        double best_score{ 0.0 };
        std::vector<std::size_t> candidate_indices{};
        std::vector<std::size_t> ranks{};
    };

    struct SweepLocalRobustnessSummary {
        std::size_t rank{ 0 };
        std::size_t candidate_index{ 0 };
        std::size_t retained_neighbor_count{ 0 };
        double center_score{ 0.0 };
        double best_neighbor_score{ 0.0 };
        double mean_neighbor_score{ 0.0 };
        double worst_neighbor_score{ 0.0 };
        double score_decay_to_best_neighbor{ 0.0 };
        double score_decay_to_mean_neighbor{ 0.0 };
        bool center_is_retained_local_best{ true };
    };

    struct SweepSearchGeometrySummary {
        SweepSearchGeometrySource source{ SweepSearchGeometrySource::None };
        std::size_t retained_candidate_pool{ 0 };
        std::size_t basin_bucket_size{ 1 };
        std::size_t neighborhood_radius{ 1 };
        std::vector<SweepBasinSummary> basins{};
        std::vector<SweepDuplicateTradePathGroup> duplicate_trade_paths{};
        std::vector<SweepLocalRobustnessSummary> local_robustness{};
    };

    struct SweepParetoObjectiveValue {
        bool present{ false };
        bool finite{ false };
        double value{ 0.0 };
    };

    struct SweepParetoCandidateSummary {
        std::size_t front_rank{ 0 };
        std::size_t candidate_index{ 0 };
        double scalar_score{ 0.0 };
        std::size_t dominates_count{ 0 };
        std::vector<SweepParetoObjectiveValue> objective_values{};
        std::vector<SweepParameterSetting> parameter_values{};
    };

    struct SweepParetoObjectiveRange {
        std::string label{};
        bool raw_non_dominated_has_value{ false };
        double raw_non_dominated_min{ 0.0 };
        double raw_non_dominated_max{ 0.0 };
        double raw_non_dominated_range{ 0.0 };
        bool emitted_front_has_value{ false };
        double emitted_front_min{ 0.0 };
        double emitted_front_max{ 0.0 };
        double emitted_front_range{ 0.0 };
    };

    enum class SweepParetoSummarySource {
        None = 0,
        EvaluatedCandidateObservations = 1,
        RetainedCandidatePool = 2,
    };

    enum class SweepParetoOutputDepollutionSource {
        None = 0,
        SweepResultDiversity = 1,
        ParetoOutputDepollution = 2,
    };

    struct SweepParetoSummary {
        bool requested{ false };
        bool computed{ false };
        SweepParetoSummarySource source{ SweepParetoSummarySource::None };
        bool output_depollution_requested{ false };
        bool output_depollution_used_result_diversity{ false };
        SweepParetoOutputDepollutionSource output_depollution_source{
            SweepParetoOutputDepollutionSource::None
        };
        bool output_depollution_dedupe_trade_hash{ false };
        bool output_depollution_basin_aware{ false };
        std::size_t output_depollution_max_per_basin{ 0 };
        std::size_t output_depollution_param_bucket_size{ 0 };
        std::size_t output_depollution_min_param_distance{ 0 };
        std::size_t output_depollution_min_trades{ 0 };
        std::size_t observed_candidate_count{ 0 };
        std::size_t unique_candidate_count{ 0 };
        std::size_t duplicate_observation_count{ 0 };
        std::size_t retained_candidate_pool{ 0 };
        std::size_t finite_candidate_count{ 0 };
        std::size_t incomplete_candidate_count{ 0 };
        std::size_t dominated_candidate_count{ 0 };
        std::size_t non_dominated_candidate_count{ 0 };
        std::size_t emitted_front_row_count{ 0 };
        std::size_t front_row_limit{ 0 };
        bool front_rows_capped{ false };
        std::size_t emitted_unique_trade_hashes{ 0 };
        std::size_t emitted_duplicate_trade_hash_groups{ 0 };
        std::size_t emitted_largest_trade_hash_group{ 0 };
        std::size_t emitted_basin_count{ 0 };
        std::size_t suppressed_duplicate_trade_hash{ 0 };
        std::size_t suppressed_basin{ 0 };
        std::size_t suppressed_min_trades{ 0 };
        std::size_t suppressed_row_limit{ 0 };
        std::size_t primary_sort_index{ 0 };
        std::vector<ObjectiveVectorComponentSpec> objectives{};
        std::vector<SweepParetoObjectiveRange> objective_ranges{};
        std::vector<SweepParetoCandidateSummary> front{};
    };

    struct PlanSweepReport {
        PlanSweepSpec requested_sweep{};
        RankingObjectiveSpec resolved_objective{};
        RunProvenanceSummary run_provenance{};
        std::vector<SweepParameterRangeSummary> parameter_ranges{};
        std::vector<SweepStageSummary> stage_summaries{};
        SweepTraceSummary proposal_trace{};
        std::vector<SweepRankedCandidate> top_candidates{};
        std::vector<SweepCandidateReport> candidate_reports{};
        SweepResultDiversitySummary result_diversity{};
        SweepSearchGeometrySummary search_geometry{};
        SweepParetoSummary pareto{};
        SweepPosteriorDiagnosticsSummary posterior{};
        std::vector<std::string> warnings{};
        std::size_t parameter_count = 0;
        std::size_t total_combinations = 0;
        bool total_combinations_overflow = false;
        std::size_t candidate_space_size = 0;
        bool candidate_space_truncated = false;
        std::size_t evaluations_run = 0;
        RunOutcomeCounts candidate_outcomes{};
        RunRankabilityCounts candidate_rankability{};
        std::size_t execution_failures = 0;
        std::size_t compile_failures = 0;
        std::string last_execution_failure{};
        std::string last_compile_error{};
        bool canceled = false;
    };

    using SweepReport = PlanSweepReport;

    SweepCandidateReport build_sweep_candidate_report(
        const SweepRankedCandidate& candidate,
        const RankingObjectiveSpec* objective = nullptr,
        const RunProvenanceSummary* provenance = nullptr,
        std::size_t rank = 0);

    std::vector<SweepCandidateReport> build_sweep_candidate_reports(
        const std::vector<SweepRankedCandidate>& candidates,
        const RankingObjectiveSpec* objective = nullptr,
        const RunProvenanceSummary* provenance = nullptr);

    void print_plan_sweep_report(
        const PlanSweepReport& report,
        std::ostream& out);
    void print_plan_sweep_posterior_diagnostics(
        const PlanSweepReport& report,
        std::ostream& out);
    void print_run_topology_diagnostics(
        const RunProvenanceSummary& provenance,
        std::ostream& out);

    const char* sweep_search_geometry_source_name(SweepSearchGeometrySource source);
    const char* sweep_pareto_summary_source_name(SweepParetoSummarySource source);
    const char* sweep_pareto_output_depollution_source_name(
        SweepParetoOutputDepollutionSource source);
    const char* sweep_posterior_diagnostics_status_name(
        SweepPosteriorDiagnosticsStatus status);

    std::vector<std::string> derive_plan_sweep_report_warnings(
        const PlanSweepReport& report);

    // DataView-first custom PlanSpec runtime path.
    bool run_custom_plan_spec(
        const DataView& dv,
        const ColumnRef& primary,
        const PlanSpec& spec,
        const PlanSweepRuntimeOptions& opt,
        std::ostream& out,
        BacktestResult* result_out = nullptr,
        PlanSweepReport* sweep_report_out = nullptr,
        const RunProvenanceSummary* run_provenance = nullptr);

} // namespace sugar
