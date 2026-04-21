#pragma once

#include <mutex>
#include <string>
#include <unordered_map>

#include "service/service_types.h"

namespace sugar::service {

    class SessionManager {
    public:
        explicit SessionManager(const QuotaConfig& quotas = {});

        bool create_session(SessionInfo& out, std::string& err);
        bool ensure_session_exists(const std::string& session_id, std::string& err) const;
        bool get_session(const std::string& session_id, SessionInfo& out, std::string& err) const;
        bool close_session(const std::string& session_id, std::string& err);
        bool begin_job(const std::string& session_id, std::string& err);
        void finish_job(const std::string& session_id);

    private:
        std::string next_session_id();

        QuotaConfig quotas_{};
        std::unordered_map<std::string, SessionInfo> sessions_{};
        std::size_t next_session_seq_{ 1 };
        mutable std::mutex mutex_{};
    };

} // namespace sugar::service
