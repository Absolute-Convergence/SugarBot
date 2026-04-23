#include "service/session_manager.h"

namespace sugar::service {

    SessionManager::SessionManager(const QuotaConfig& quotas)
        : quotas_(quotas) {}

    bool SessionManager::create_session(SessionInfo& out, std::string& err) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (quotas_.max_live_sessions > 0 &&
            sessions_.size() >= quotas_.max_live_sessions) {
            err = "server exceeded max live sessions";
            return false;
        }
        err.clear();
        SessionInfo session{};
        session.session_id = next_session_id();
        sessions_.emplace(session.session_id, session);
        out = session;
        return true;
    }

    bool SessionManager::ensure_session_exists(const std::string& session_id, std::string& err) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (sessions_.find(session_id) == sessions_.end()) {
            err = "unknown session id";
            return false;
        }
        err.clear();
        return true;
    }

    bool SessionManager::get_session(const std::string& session_id, SessionInfo& out, std::string& err) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = sessions_.find(session_id);
        if (it == sessions_.end()) {
            err = "unknown session id";
            return false;
        }
        out = it->second;
        err.clear();
        return true;
    }

    bool SessionManager::close_session(const std::string& session_id, std::string& err) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = sessions_.find(session_id);
        if (it == sessions_.end()) {
            err = "unknown session id";
            return false;
        }
        if (it->second.active_jobs != 0) {
            err = "session has active jobs and cannot disconnect yet";
            return false;
        }
        sessions_.erase(it);
        err.clear();
        return true;
    }

    bool SessionManager::begin_job(const std::string& session_id, std::string& err) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = sessions_.find(session_id);
        if (it == sessions_.end()) {
            err = "unknown session id";
            return false;
        }
        if (it->second.active_jobs >= quotas_.max_concurrent_jobs_per_session) {
            err = "session exceeded max concurrent jobs";
            return false;
        }
        ++it->second.active_jobs;
        err.clear();
        return true;
    }

    void SessionManager::finish_job(const std::string& session_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = sessions_.find(session_id);
        if (it == sessions_.end()) {
            return;
        }
        if (it->second.active_jobs > 0) {
            --it->second.active_jobs;
        }
    }

    std::string SessionManager::next_session_id() {
        return make_counter_id("sess", next_session_seq_++);
    }

} // namespace sugar::service
