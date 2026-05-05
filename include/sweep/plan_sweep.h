#pragma once

#include <functional>
#include <iosfwd>

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

    struct PlanSweepReport {
        PlanSweepSpec requested_sweep{};
        RankingObjectiveSpec resolved_objective{};
        RunProvenanceSummary run_provenance{};
        std::vector<SweepParameterRangeSummary> parameter_ranges{};
        std::vector<SweepStageSummary> stage_summaries{};
        std::vector<SweepRankedCandidate> top_candidates{};
        SweepResultDiversitySummary result_diversity{};
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

    void print_plan_sweep_report(
        const PlanSweepReport& report,
        std::ostream& out);

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
