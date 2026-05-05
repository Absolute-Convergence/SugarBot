#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "service/service_types.h"

namespace sugar::service {

    class JobManager {
    public:
        struct JobExecutionContext {
            SubmitRequest request{};
            JobStatus status{};
            std::shared_ptr<std::atomic_bool> cancel_requested{};
        };

        bool create_job(
            const std::string& job_id,
            const std::string& session_id,
            const SubmitRequest& request,
            const std::vector<ArtifactRecord>& staged_artifacts,
            std::size_t max_total_active_jobs,
            std::string& err);
        bool ensure_owned(const std::string& session_id, const std::string& job_id, std::string& err) const;
        bool get_owned_status(
            const std::string& session_id,
            const std::string& job_id,
            JobStatus& out,
            std::string& err) const;
        bool get_execution_context(const std::string& job_id, JobExecutionContext& out, std::string& err) const;
        bool begin_execution(
            const std::string& job_id,
            JobExecutionContext& out,
            bool& out_started,
            std::string& err);
        bool record_progress(
            const std::string& job_id,
            JobState state,
            std::string_view message,
            std::size_t completed,
            std::size_t total,
            std::string& err);
        bool request_cancel(
            const std::string& session_id,
            const std::string& job_id,
            JobState& out_state,
            std::string& err);
        bool finalize_rejected(
            const std::string& job_id,
            const std::string& result_id,
            std::string_view detail,
            std::string& err);
        bool finalize_failed(
            const std::string& job_id,
            std::string_view detail,
            const std::optional<std::string>& result_id,
            std::string& err);
        bool reserve_completion(
            const std::string& job_id,
            std::string_view cancel_detail,
            bool& out_canceled,
            std::string& err);
        bool finalize_completed(
            const std::string& job_id,
            const std::string& result_id,
            std::string& err);
        bool finalize_canceled(
            const std::string& job_id,
            std::string_view detail,
            std::string& err);
        bool cleanup_terminal(const std::string& session_id, const std::string& job_id, std::string& err);

        std::string next_job_id();
        std::string next_result_id();

    private:
        struct JobRecord {
            SubmitRequest request{};
            JobStatus status{};
            std::shared_ptr<std::atomic_bool> cancel_requested{
                std::make_shared<std::atomic_bool>(false)
            };
            bool completion_reserved{ false };
        };

        static void append_progress_unlocked(
            JobStatus& status,
            JobState state,
            std::string_view message,
            std::size_t completed,
            std::size_t total);
        static bool is_active_state(JobState state);
        std::size_t active_job_count_unlocked() const;

        mutable std::mutex mutex_{};
        std::size_t next_job_seq_{ 1 };
        std::size_t next_result_seq_{ 1 };
        std::unordered_map<std::string, JobRecord> jobs_{};
    };

} // namespace sugar::service
