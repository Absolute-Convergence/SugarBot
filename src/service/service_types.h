#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "sweep/metrics.h"
#include "sweep/panel_runner.h"
#include "sweep/plan_sweep.h"

namespace sugar::service {

    enum class JobIntent {
        ValidateOnly = 0,
        Execute = 1,
    };

    enum class JobState {
        Queued = 0,
        Validating = 1,
        Rejected = 2,
        Running = 3,
        CancelRequested = 4,
        Canceled = 5,
        Completed = 6,
        Failed = 7,
    };

    enum class ArtifactKind {
        LayoutSpec = 0,
        LogicSpec = 1,
        DataCsv = 2,
        SummaryText = 3,
    };

    enum class ResolvedExecutionMode {
        Unknown = 0,
        ValidationOnly = 1,
        PrimarySeriesRun = 2,
        PrimarySeriesSweep = 3,
        PerEntityRankedRun = 4,
        PerEntityRankedSweep = 5,
    };

    enum class ServiceProfile {
        LocalAi = 0,
        HostedPrivate = 1,
        HostedPublic = 2,
    };

    struct QuotaConfig {
        std::size_t max_upload_size_bytes{ 1024 * 1024 }; // 1 MiB per uploaded artifact.
        std::size_t max_parsed_row_count{ 250000 }; // Conservative hosted-service validation cap.
        std::size_t max_sweep_candidate_count{ 10000 }; // Bound exhaustive sweep expansion on shared servers.
        std::size_t max_live_sessions{ 4 }; // Hosted default matches the intended small-server client budget.
        std::size_t max_staged_uploads_per_session{ 24 }; // Bound retained pre-submit uploads per client session.
        std::size_t max_staged_upload_bytes_per_session{ 8 * 1024 * 1024 }; // Bound retained upload bytes per session.
        std::size_t max_concurrent_jobs_per_session{ 4 }; // Prevent one client from monopolizing workers.
        std::size_t max_total_active_jobs{ 4 }; // Bound total hosted jobs to the intended small-server envelope.
        std::size_t max_retained_result_bytes{ 1024 * 1024 }; // Bound each retained summary payload.
        std::size_t max_retained_results{ 256 }; // Bound terminal result count for long-lived servers.
        std::size_t server_worker_limit{ 1 }; // Serial by default; raise explicitly for hosted concurrency.
        std::size_t sweep_warning_minutes{ 15 }; // Warn before queued work starts to look expensive for hosted mode.
        std::size_t sweep_reject_minutes{ 30 }; // Reject hosted sweeps that exceed the configured candidate budget.
        std::size_t expensive_sweep_reject_disconnect_streak{ 3 }; // Disconnect after 3 over-budget submits in a row.
        std::size_t expensive_sweep_reject_pair_disconnect_count{ 3 }; // Or after three separate 2-in-a-row episodes.
        std::size_t expensive_sweep_reject_total_before_disconnect{ 8 }; // Or after more than 8 over-budget rejects in one session.
    };

    struct RuntimeConfig {
        std::filesystem::path storage_root{};
        QuotaConfig quotas{};
    };

    struct ArtifactPayload {
        std::string client_name{};
        std::string content{};

        [[nodiscard]] std::size_t size_bytes() const noexcept {
            return content.size();
        }
    };

    struct SubmitRequest {
        JobIntent intent{ JobIntent::ValidateOnly };
        ArtifactPayload layout{};
        ArtifactPayload logic{};
        ArtifactPayload data{};
    };

    struct SessionInfo {
        std::string session_id{};
        std::size_t active_jobs{ 0 };
    };

    struct ArtifactRecord {
        std::string artifact_id{};
        ArtifactKind kind{ ArtifactKind::LayoutSpec };
        std::string owner_job_id{};
        std::filesystem::path storage_path{};
        std::size_t byte_count{ 0 };
    };

    struct ProgressEvent {
        JobState state{ JobState::Queued };
        std::string message{};
        std::size_t completed{ 0 };
        std::size_t total{ 0 };
    };

    struct ValidationMetadata {
        bool valid{ false };
        ResolvedExecutionMode resolved_mode{ ResolvedExecutionMode::Unknown };
        bool multi_series{ false };
        std::size_t series_count{ 0 };
        std::size_t row_count{ 0 };
        std::size_t input_warning_count{ 0 };
        std::size_t incompatible_nodes{ 0 };
        std::size_t incompatible_bindings{ 0 };
        std::size_t sweep_candidate_count{ 0 };
        std::size_t estimated_sweep_minutes{ 0 };
        bool long_sweep_warning{ false };
    };

    struct ResultMetadata {
        ValidationMetadata validation{};
        std::optional<BacktestResult> primary_backtest{};
        std::optional<PlanSweepReport> sweep_report{};
        std::optional<PanelResult> panel{};
        std::optional<RunProvenanceSummary> run_provenance{};
    };

    struct JobResult {
        std::string result_id{};
        std::string owner_session_id{};
        std::string job_id{};
        ResultMetadata metadata{};
        std::string summary_text{};
        std::vector<ArtifactRecord> input_artifacts{};
        std::vector<ArtifactRecord> output_artifacts{};
    };

    struct JobStatus {
        std::string job_id{};
        std::string owner_session_id{};
        JobState state{ JobState::Queued };
        std::vector<ArtifactRecord> staged_artifacts{};
        std::vector<ProgressEvent> progress{};
        std::optional<std::string> result_id{};
        std::string failure_message{};
    };

    std::string make_counter_id(std::string_view prefix, std::size_t seq);
    std::string_view job_intent_name(JobIntent intent);
    std::string_view job_state_name(JobState state);
    std::string_view artifact_kind_name(ArtifactKind kind);
    std::string_view resolved_execution_mode_name(ResolvedExecutionMode mode);
    std::string_view service_profile_name(ServiceProfile profile);
    bool parse_service_profile(std::string_view text, ServiceProfile& out);
    QuotaConfig make_quota_config_for_service_profile(ServiceProfile profile);

} // namespace sugar::service
