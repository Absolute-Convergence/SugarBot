#include "service/service_types.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace sugar::service {

    std::string make_counter_id(std::string_view prefix, std::size_t seq) {
        char buf[32]{};
        std::snprintf(buf, sizeof(buf), "%.*s-%06zu",
            static_cast<int>(prefix.size()),
            prefix.data(),
            seq);
        return std::string(buf);
    }

    std::string_view job_intent_name(JobIntent intent) {
        switch (intent) {
        case JobIntent::ValidateOnly:
            return "validate_only";
        case JobIntent::Execute:
            return "execute";
        }
        return "unknown";
    }

    std::string_view job_state_name(JobState state) {
        switch (state) {
        case JobState::Queued:
            return "queued";
        case JobState::Validating:
            return "validating";
        case JobState::Rejected:
            return "rejected";
        case JobState::Running:
            return "running";
        case JobState::CancelRequested:
            return "cancel_requested";
        case JobState::Canceled:
            return "canceled";
        case JobState::Completed:
            return "completed";
        case JobState::Failed:
            return "failed";
        }
        return "unknown";
    }

    std::string_view artifact_kind_name(ArtifactKind kind) {
        switch (kind) {
        case ArtifactKind::LayoutSpec:
            return "layout";
        case ArtifactKind::LogicSpec:
            return "logic";
        case ArtifactKind::DataCsv:
            return "data";
        case ArtifactKind::SummaryText:
            return "summary";
        }
        return "unknown";
    }

    std::string_view resolved_execution_mode_name(ResolvedExecutionMode mode) {
        switch (mode) {
        case ResolvedExecutionMode::Unknown:
            return "unknown";
        case ResolvedExecutionMode::ValidationOnly:
            return "validation_only";
        case ResolvedExecutionMode::PrimarySeriesRun:
            return "primary_series_run";
        case ResolvedExecutionMode::PrimarySeriesSweep:
            return "primary_series_sweep";
        case ResolvedExecutionMode::PerEntityRankedRun:
            return "per_entity_ranked_run";
        case ResolvedExecutionMode::PerEntityRankedSweep:
            return "per_entity_ranked_sweep";
        }
        return "unknown";
    }

    std::string_view service_profile_name(ServiceProfile profile) {
        switch (profile) {
        case ServiceProfile::LocalAi:
            return "local-ai";
        case ServiceProfile::HostedPrivate:
            return "hosted-private";
        case ServiceProfile::HostedPublic:
            return "hosted-public";
        }
        return "unknown";
    }

    bool parse_service_profile(std::string_view text, ServiceProfile& out) {
        std::string normalized(text);
        std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        std::replace(normalized.begin(), normalized.end(), '_', '-');

        if (normalized == "local-ai" ||
            normalized == "local" ||
            normalized == "dev" ||
            normalized == "development") {
            out = ServiceProfile::LocalAi;
            return true;
        }
        if (normalized == "hosted-private" ||
            normalized == "private") {
            out = ServiceProfile::HostedPrivate;
            return true;
        }
        if (normalized == "hosted-public" ||
            normalized == "public") {
            out = ServiceProfile::HostedPublic;
            return true;
        }
        return false;
    }

    QuotaConfig make_quota_config_for_service_profile(ServiceProfile profile) {
        QuotaConfig quotas{};
        switch (profile) {
        case ServiceProfile::LocalAi:
            quotas.max_upload_size_bytes = 16 * 1024 * 1024;
            quotas.max_parsed_row_count = 1000000;
            quotas.max_sweep_candidate_count = 500000;
            quotas.max_live_sessions = 32;
            quotas.max_staged_uploads_per_session = 128;
            quotas.max_staged_upload_bytes_per_session = 256 * 1024 * 1024;
            quotas.max_concurrent_jobs_per_session = 16;
            quotas.max_total_active_jobs = 32;
            quotas.max_retained_result_bytes = 16 * 1024 * 1024;
            quotas.max_retained_results = 2048;
            quotas.server_worker_limit = 2;
            quotas.sweep_warning_minutes = 60;
            quotas.sweep_reject_minutes = 240;
            quotas.expensive_sweep_reject_disconnect_streak = 0;
            quotas.expensive_sweep_reject_pair_disconnect_count = 0;
            quotas.expensive_sweep_reject_total_before_disconnect = 0;
            break;
        case ServiceProfile::HostedPrivate:
            quotas.max_upload_size_bytes = 8 * 1024 * 1024;
            quotas.max_parsed_row_count = 500000;
            quotas.max_sweep_candidate_count = 50000;
            quotas.max_live_sessions = 16;
            quotas.max_staged_uploads_per_session = 64;
            quotas.max_staged_upload_bytes_per_session = 64 * 1024 * 1024;
            quotas.max_concurrent_jobs_per_session = 8;
            quotas.max_total_active_jobs = 16;
            quotas.max_retained_result_bytes = 8 * 1024 * 1024;
            quotas.max_retained_results = 1024;
            quotas.server_worker_limit = 2;
            quotas.sweep_warning_minutes = 30;
            quotas.sweep_reject_minutes = 120;
            quotas.expensive_sweep_reject_disconnect_streak = 0;
            quotas.expensive_sweep_reject_pair_disconnect_count = 0;
            quotas.expensive_sweep_reject_total_before_disconnect = 0;
            break;
        case ServiceProfile::HostedPublic:
            break;
        }
        return quotas;
    }

} // namespace sugar::service
