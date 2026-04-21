#pragma once

#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

namespace sugar::service {

    class JobScheduler {
    public:
        void enqueue(const std::string& session_id, const std::string& job_id);
        bool dequeue_next(std::string& out_job_id);
        bool wait_dequeue_next(std::string& out_job_id);
        void remove(const std::string& job_id);
        void shutdown();

    private:
        bool try_dequeue_next_unlocked(std::string& out_job_id);

        std::unordered_map<std::string, std::deque<std::string>> queues_by_session_{};
        std::unordered_map<std::string, std::string> session_by_job_{};
        std::deque<std::string> session_order_{};
        mutable std::mutex mutex_{};
        std::condition_variable cv_{};
        bool shutdown_{ false };
    };

} // namespace sugar::service
