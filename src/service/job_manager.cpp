#include "service/job_manager.h"

namespace sugar::service {

    bool JobManager::create_job(
        const std::string& job_id,
        const std::string& session_id,
        const SubmitRequest& request,
        const std::vector<ArtifactRecord>& staged_artifacts,
        std::size_t max_total_active_jobs,
        std::string& err) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (max_total_active_jobs > 0 && active_job_count_unlocked() >= max_total_active_jobs) {
            err = "server exceeded max active jobs";
            return false;
        }

        JobRecord job{};
        job.request = request;
        job.status.job_id = job_id;
        job.status.owner_session_id = session_id;
        job.status.staged_artifacts = staged_artifacts;
        append_progress_unlocked(job.status, JobState::Queued, "upload staged", 3, 3);
        append_progress_unlocked(job.status, JobState::Queued, "job queued", 0, 0);

        jobs_.emplace(job_id, std::move(job));
        err.clear();
        return true;
    }

    bool JobManager::ensure_owned(
        const std::string& session_id,
        const std::string& job_id,
        std::string& err) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = jobs_.find(job_id);
        if (it == jobs_.end()) {
            err = "unknown job id";
            return false;
        }
        if (it->second.status.owner_session_id != session_id) {
            err = "job does not belong to the requesting session";
            return false;
        }
        err.clear();
        return true;
    }

    bool JobManager::get_owned_status(
        const std::string& session_id,
        const std::string& job_id,
        JobStatus& out,
        std::string& err) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = jobs_.find(job_id);
        if (it == jobs_.end()) {
            err = "unknown job id";
            return false;
        }
        if (it->second.status.owner_session_id != session_id) {
            err = "job does not belong to the requesting session";
            return false;
        }
        out = it->second.status;
        err.clear();
        return true;
    }

    bool JobManager::get_execution_context(const std::string& job_id, JobExecutionContext& out, std::string& err) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = jobs_.find(job_id);
        if (it == jobs_.end()) {
            err = "unknown job id";
            return false;
        }

        out.request = it->second.request;
        out.status = it->second.status;
        out.cancel_requested = it->second.cancel_requested;
        err.clear();
        return true;
    }

    bool JobManager::begin_execution(
        const std::string& job_id,
        JobExecutionContext& out,
        bool& out_started,
        std::string& err) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = jobs_.find(job_id);
        if (it == jobs_.end()) {
            err = "unknown job id";
            return false;
        }

        out.request = it->second.request;
        out.status = it->second.status;
        out.cancel_requested = it->second.cancel_requested;

        if (it->second.status.state == JobState::Canceled) {
            out_started = false;
            err.clear();
            return true;
        }
        if (it->second.status.state != JobState::Queued) {
            err = "job is not queued";
            return false;
        }

        append_progress_unlocked(it->second.status, JobState::Validating, "validation started", 0, 0);
        out.status = it->second.status;
        out_started = true;
        err.clear();
        return true;
    }

    bool JobManager::record_progress(
        const std::string& job_id,
        JobState state,
        std::string_view message,
        std::size_t completed,
        std::size_t total,
        std::string& err) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = jobs_.find(job_id);
        if (it == jobs_.end()) {
            err = "unknown job id";
            return false;
        }

        append_progress_unlocked(it->second.status, state, message, completed, total);
        err.clear();
        return true;
    }

    bool JobManager::request_cancel(
        const std::string& session_id,
        const std::string& job_id,
        JobState& out_state,
        std::string& err) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = jobs_.find(job_id);
        if (it == jobs_.end()) {
            err = "unknown job id";
            return false;
        }
        if (it->second.status.owner_session_id != session_id) {
            err = "job does not belong to the requesting session";
            return false;
        }

        if (it->second.completion_reserved) {
            err = "job is not cancelable in its current state";
            return false;
        }

        switch (it->second.status.state) {
        case JobState::Queued:
            it->second.cancel_requested->store(true);
            append_progress_unlocked(it->second.status, JobState::CancelRequested, "cancellation requested", 0, 0);
            append_progress_unlocked(it->second.status, JobState::Canceled, "job canceled", 0, 0);
            out_state = JobState::Canceled;
            err.clear();
            return true;
        case JobState::Validating:
        case JobState::Running:
            it->second.cancel_requested->store(true);
            if (it->second.status.state != JobState::CancelRequested) {
                append_progress_unlocked(it->second.status, JobState::CancelRequested, "cancellation requested", 0, 0);
            }
            out_state = JobState::CancelRequested;
            err.clear();
            return true;
        case JobState::CancelRequested:
        case JobState::Canceled:
            it->second.cancel_requested->store(true);
            out_state = it->second.status.state;
            err.clear();
            return true;
        case JobState::Rejected:
        case JobState::Completed:
        case JobState::Failed:
            out_state = it->second.status.state;
            err.clear();
            return true;
        default:
            err = "job is not cancelable in its current state";
            return false;
        }
    }

    bool JobManager::finalize_rejected(
        const std::string& job_id,
        const std::string& result_id,
        std::string_view detail,
        std::string& err) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = jobs_.find(job_id);
        if (it == jobs_.end()) {
            err = "unknown job id";
            return false;
        }
        append_progress_unlocked(it->second.status, JobState::Rejected, "validation rejected", 0, 0);
        it->second.completion_reserved = false;
        it->second.status.failure_message = std::string(detail);
        it->second.status.result_id = result_id;
        err.clear();
        return true;
    }

    bool JobManager::finalize_failed(
        const std::string& job_id,
        std::string_view detail,
        const std::optional<std::string>& result_id,
        std::string& err) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = jobs_.find(job_id);
        if (it == jobs_.end()) {
            err = "unknown job id";
            return false;
        }
        append_progress_unlocked(it->second.status, JobState::Failed, "job failed", 0, 0);
        it->second.completion_reserved = false;
        it->second.status.failure_message = std::string(detail);
        it->second.status.result_id = result_id;
        err.clear();
        return true;
    }

    bool JobManager::reserve_completion(
        const std::string& job_id,
        std::string_view cancel_detail,
        bool& out_canceled,
        std::string& err) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = jobs_.find(job_id);
        if (it == jobs_.end()) {
            err = "unknown job id";
            return false;
        }

        if (it->second.cancel_requested->load() ||
            it->second.status.state == JobState::CancelRequested ||
            it->second.status.state == JobState::Canceled) {
            it->second.cancel_requested->store(true);
            if (it->second.status.state != JobState::Canceled) {
                append_progress_unlocked(it->second.status, JobState::Canceled, "job canceled", 0, 0);
            }
            it->second.completion_reserved = false;
            it->second.status.failure_message = std::string(cancel_detail);
            it->second.status.result_id.reset();
            out_canceled = true;
            err.clear();
            return true;
        }

        if (it->second.status.state == JobState::Completed ||
            it->second.status.state == JobState::Rejected ||
            it->second.status.state == JobState::Failed) {
            err = "job is already terminal";
            return false;
        }

        it->second.completion_reserved = true;
        out_canceled = false;
        err.clear();
        return true;
    }

    bool JobManager::finalize_completed(
        const std::string& job_id,
        const std::string& result_id,
        std::string& err) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = jobs_.find(job_id);
        if (it == jobs_.end()) {
            err = "unknown job id";
            return false;
        }
        append_progress_unlocked(it->second.status, JobState::Completed, "job completed", 0, 0);
        it->second.completion_reserved = false;
        it->second.status.failure_message.clear();
        it->second.status.result_id = result_id;
        err.clear();
        return true;
    }

    bool JobManager::finalize_canceled(
        const std::string& job_id,
        std::string_view detail,
        std::string& err) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = jobs_.find(job_id);
        if (it == jobs_.end()) {
            err = "unknown job id";
            return false;
        }
        it->second.cancel_requested->store(true);
        it->second.completion_reserved = false;
        if (it->second.status.state != JobState::Canceled) {
            append_progress_unlocked(it->second.status, JobState::Canceled, "job canceled", 0, 0);
        }
        it->second.status.failure_message = std::string(detail);
        it->second.status.result_id.reset();
        err.clear();
        return true;
    }

    bool JobManager::cleanup_terminal(const std::string& session_id, const std::string& job_id, std::string& err) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = jobs_.find(job_id);
        if (it == jobs_.end()) {
            err = "unknown job id";
            return false;
        }
        if (it->second.status.owner_session_id != session_id) {
            err = "job does not belong to the requesting session";
            return false;
        }

        const auto current_state = it->second.status.state;
        if (current_state == JobState::Queued ||
            current_state == JobState::Validating ||
            current_state == JobState::Running ||
            current_state == JobState::CancelRequested) {
            err = "active jobs cannot be cleaned up";
            return false;
        }

        jobs_.erase(it);
        err.clear();
        return true;
    }

    std::string JobManager::next_job_id() {
        std::lock_guard<std::mutex> lock(mutex_);
        return make_counter_id("job", next_job_seq_++);
    }

    std::string JobManager::next_result_id() {
        std::lock_guard<std::mutex> lock(mutex_);
        return make_counter_id("result", next_result_seq_++);
    }

    void JobManager::append_progress_unlocked(
        JobStatus& status,
        JobState state,
        std::string_view message,
        std::size_t completed,
        std::size_t total) {
        // Once a cancellation request is visible, intermediate progress updates must
        // not "heal" the state back to validating/running before terminal cancellation.
        const bool preserve_cancel_requested =
            status.state == JobState::CancelRequested &&
            (state == JobState::Running || state == JobState::Validating);
        const JobState effective_state = preserve_cancel_requested ? JobState::CancelRequested : state;
        status.state = effective_state;
        status.progress.push_back(
            ProgressEvent{
                effective_state,
                std::string(message),
                completed,
                total,
            });
    }

    bool JobManager::is_active_state(JobState state) {
        switch (state) {
        case JobState::Queued:
        case JobState::Validating:
        case JobState::Running:
        case JobState::CancelRequested:
            return true;
        case JobState::Rejected:
        case JobState::Canceled:
        case JobState::Completed:
        case JobState::Failed:
            return false;
        }
        return false;
    }

    std::size_t JobManager::active_job_count_unlocked() const {
        std::size_t count = 0;
        for (const auto& [job_id, job] : jobs_) {
            (void)job_id;
            if (is_active_state(job.status.state)) {
                ++count;
            }
        }
        return count;
    }

} // namespace sugar::service
