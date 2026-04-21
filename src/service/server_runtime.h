#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "service/artifact_store.h"
#include "service/job_manager.h"
#include "service/job_scheduler.h"
#include "service/job_worker_pool.h"
#include "service/request_validation.h"
#include "service/result_store.h"
#include "service/session_manager.h"

namespace sugar::service {

    class ServerRuntime {
    public:
        explicit ServerRuntime(RuntimeConfig config = {});
        ~ServerRuntime();

        [[nodiscard]] bool create_session(SessionInfo& out, std::string& err);
        [[nodiscard]] bool get_session_info(const std::string& session_id, SessionInfo& out, std::string& err) const;
        [[nodiscard]] bool close_session(const std::string& session_id, std::string& err);
        [[nodiscard]] bool submit_job(
            const std::string& session_id,
            const SubmitRequest& request,
            std::string& out_job_id,
            std::string& err);
        [[nodiscard]] bool process_next_queued_job(std::string& out_job_id, std::string& err);
        [[nodiscard]] bool process_job(const std::string& job_id, std::string& err);
        [[nodiscard]] bool cancel_job(const std::string& session_id, const std::string& job_id, std::string& err);
        [[nodiscard]] bool get_job_status(
            const std::string& session_id,
            const std::string& job_id,
            JobStatus& out,
            std::string& err) const;
        [[nodiscard]] bool get_job_result(
            const std::string& session_id,
            const std::string& job_id,
            JobResult& out,
            std::string& err) const;
        [[nodiscard]] bool cleanup_job(const std::string& session_id, const std::string& job_id, std::string& err);

        [[nodiscard]] const QuotaConfig& quotas() const noexcept {
            return config_.quotas;
        }

        [[nodiscard]] const ArtifactStore& artifact_store() const noexcept {
            return artifact_store_;
        }

    private:
        [[nodiscard]] bool ensure_initialized(std::string& err);
        [[nodiscard]] bool ensure_owned_job(const std::string& session_id, const std::string& job_id, std::string& err) const;
        [[nodiscard]] bool finalize_rejection(
            const std::string& owner_session_id,
            const std::string& job_id,
            const ValidatedRequest& validated,
            const std::string& detail);
        [[nodiscard]] bool finalize_failure(
            const std::string& owner_session_id,
            const std::string& job_id,
            const ValidationMetadata& metadata,
            const std::string& detail);
        [[nodiscard]] bool finalize_completion(
            const std::string& owner_session_id,
            const std::string& job_id,
            JobResult result,
            std::string& err);
        [[nodiscard]] bool finalize_cancellation(
            const std::string& owner_session_id,
            const std::string& job_id,
            const std::string& detail,
            std::string& err);

        RuntimeConfig config_{};
        SessionManager sessions_{};
        ArtifactStore artifact_store_;
        ResultStore results_{};
        JobManager jobs_{};
        JobScheduler scheduler_{};
        JobWorkerPool workers_{};
        bool initialized_{ false };
        mutable std::mutex init_mutex_{};
    };

} // namespace sugar::service
