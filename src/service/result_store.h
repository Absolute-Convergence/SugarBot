#pragma once

#include <mutex>
#include <string>
#include <unordered_map>

#include "service/service_types.h"

namespace sugar::service {

    class ResultStore {
    public:
        explicit ResultStore(const QuotaConfig& quotas = {});

        bool store(const JobResult& result, std::string& err);
        bool fetch_owned(
            const std::string& session_id,
            const std::string& job_id,
            JobResult& out,
            std::string& err) const;
        // Cleanup is idempotent: missing retained results are treated as already erased.
        bool erase_owned(
            const std::string& session_id,
            const std::string& job_id,
            std::string& err);

    private:
        QuotaConfig quotas_{};
        std::unordered_map<std::string, JobResult> results_{};
        mutable std::mutex mutex_{};
    };

} // namespace sugar::service
