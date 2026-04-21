#include "service/result_store.h"

namespace sugar::service {

    ResultStore::ResultStore(const QuotaConfig& quotas)
        : quotas_(quotas) {}

    bool ResultStore::store(const JobResult& result, std::string& err) {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool already_present = results_.find(result.job_id) != results_.end();
        if (result.summary_text.size() > quotas_.max_retained_result_bytes) {
            err = "result summary exceeds max retained result size";
            return false;
        }
        if (!already_present &&
            quotas_.max_retained_results > 0 &&
            results_.size() >= quotas_.max_retained_results) {
            err = "retained result store exceeded max retained result count";
            return false;
        }
        results_[result.job_id] = result;
        err.clear();
        return true;
    }

    bool ResultStore::fetch_owned(
        const std::string& session_id,
        const std::string& job_id,
        JobResult& out,
        std::string& err) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = results_.find(job_id);
        if (it == results_.end()) {
            err = "job result is not available";
            return false;
        }
        if (it->second.owner_session_id != session_id) {
            err = "job result does not belong to the requesting session";
            return false;
        }
        out = it->second;
        err.clear();
        return true;
    }

    bool ResultStore::erase_owned(
        const std::string& session_id,
        const std::string& job_id,
        std::string& err) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = results_.find(job_id);
        if (it == results_.end()) {
            err.clear();
            return true;
        }
        if (it->second.owner_session_id != session_id) {
            err = "job result does not belong to the requesting session";
            return false;
        }
        results_.erase(it);
        err.clear();
        return true;
    }

} // namespace sugar::service
