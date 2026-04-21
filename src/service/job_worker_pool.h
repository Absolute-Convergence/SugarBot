#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace sugar::service {

    class JobScheduler;

    class JobWorkerPool {
    public:
        using WorkHandler = std::function<void(const std::string&)>;

        JobWorkerPool() = default;
        ~JobWorkerPool();

        JobWorkerPool(const JobWorkerPool&) = delete;
        JobWorkerPool& operator=(const JobWorkerPool&) = delete;

        void start(std::size_t worker_count, JobScheduler& scheduler, WorkHandler handler);
        void stop();

        [[nodiscard]] bool running() const noexcept {
            return !threads_.empty();
        }

    private:
        void worker_loop();

        JobScheduler* scheduler_{ nullptr };
        WorkHandler handler_{};
        std::atomic_bool stop_requested_{ true };
        std::vector<std::thread> threads_{};
    };

} // namespace sugar::service
