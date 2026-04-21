#include "service/job_worker_pool.h"

#include "service/job_scheduler.h"

namespace sugar::service {

    JobWorkerPool::~JobWorkerPool() {
        stop();
    }

    void JobWorkerPool::start(std::size_t worker_count, JobScheduler& scheduler, WorkHandler handler) {
        if (!threads_.empty() || worker_count == 0 || !handler) {
            return;
        }

        stop_requested_.store(false, std::memory_order_release);
        scheduler_ = &scheduler;
        handler_ = std::move(handler);
        threads_.reserve(worker_count);
        for (std::size_t i = 0; i < worker_count; ++i) {
            threads_.emplace_back([this] { worker_loop(); });
        }
    }

    void JobWorkerPool::stop() {
        if (!scheduler_ && threads_.empty()) {
            return;
        }

        stop_requested_.store(true, std::memory_order_release);
        if (scheduler_) {
            scheduler_->shutdown();
        }
        for (auto& thread : threads_) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        threads_.clear();
        handler_ = WorkHandler{};
        scheduler_ = nullptr;
    }

    void JobWorkerPool::worker_loop() {
        JobScheduler* const scheduler = scheduler_;
        if (!scheduler) {
            return;
        }

        while (!stop_requested_.load(std::memory_order_acquire)) {
            std::string job_id;
            if (!scheduler->wait_dequeue_next(job_id)) {
                break;
            }
            if (!stop_requested_.load(std::memory_order_acquire) && !job_id.empty() && handler_) {
                handler_(job_id);
            }
        }
    }

} // namespace sugar::service
