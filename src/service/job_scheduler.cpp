#include "service/job_scheduler.h"

#include <algorithm>

namespace sugar::service {

    void JobScheduler::enqueue(const std::string& session_id, const std::string& job_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& queue = queues_by_session_[session_id];
        if (queue.empty()) {
            session_order_.push_back(session_id);
        }
        queue.push_back(job_id);
        session_by_job_[job_id] = session_id;
        cv_.notify_one();
    }

    bool JobScheduler::dequeue_next(std::string& out_job_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        return try_dequeue_next_unlocked(out_job_id);
    }

    bool JobScheduler::wait_dequeue_next(std::string& out_job_id) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] {
            return shutdown_ || !session_order_.empty();
        });
        if (shutdown_) {
            out_job_id.clear();
            return false;
        }
        return try_dequeue_next_unlocked(out_job_id);
    }

    void JobScheduler::remove(const std::string& job_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto session_it = session_by_job_.find(job_id);
        if (session_it == session_by_job_.end()) {
            return;
        }
        const std::string session_id = session_it->second;

        const auto queue_it = queues_by_session_.find(session_id);
        if (queue_it == queues_by_session_.end()) {
            session_by_job_.erase(session_it);
            return;
        }

        auto& queue = queue_it->second;
        queue.erase(std::remove(queue.begin(), queue.end(), job_id), queue.end());
        session_by_job_.erase(session_it);
        if (queue.empty()) {
            queues_by_session_.erase(queue_it);
            session_order_.erase(
                std::remove(session_order_.begin(), session_order_.end(), session_id),
                session_order_.end());
        }
    }

    void JobScheduler::shutdown() {
        std::lock_guard<std::mutex> lock(mutex_);
        shutdown_ = true;
        cv_.notify_all();
    }

    bool JobScheduler::try_dequeue_next_unlocked(std::string& out_job_id) {
        out_job_id.clear();
        while (!session_order_.empty()) {
            const std::string session_id = session_order_.front();
            session_order_.pop_front();

            const auto queue_it = queues_by_session_.find(session_id);
            if (queue_it == queues_by_session_.end() || queue_it->second.empty()) {
                queues_by_session_.erase(session_id);
                continue;
            }

            auto& queue = queue_it->second;
            out_job_id = queue.front();
            queue.pop_front();
            session_by_job_.erase(out_job_id);

            if (!queue.empty()) {
                session_order_.push_back(session_id);
            }
            else {
                queues_by_session_.erase(queue_it);
            }
            return true;
        }
        return false;
    }

} // namespace sugar::service
