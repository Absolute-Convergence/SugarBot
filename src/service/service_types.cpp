#include "service/service_types.h"

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

} // namespace sugar::service
