#include "service/network_server.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <iostream>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "service/network_session.h"

#ifdef _WIN32

namespace sugar::service {

    int run_tcp_server(const ServerCliOptions&, std::ostream&, std::ostream& err) {
        err << "Error: sugar_server TCP sockets are currently supported only on Linux/WSL builds.\n";
        return 1;
    }

    int run_tcp_server_from_cli(int, char**, std::ostream& out, std::ostream& err) {
        ServerCliOptions options{};
        return run_tcp_server(options, out, err);
    }

} // namespace sugar::service

#else

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace sugar::service {

    namespace {

        bool is_stop_requested(const std::shared_ptr<std::atomic_bool>& stop_requested) {
            return stop_requested && stop_requested->load(std::memory_order_relaxed);
        }

        class PendingConnectionQueue {
        public:
            explicit PendingConnectionQueue(std::size_t max_pending_connections)
                : max_pending_connections_(std::max<std::size_t>(std::size_t{ 1 }, max_pending_connections)) {}

            bool wait_for_enqueue_slot(const std::shared_ptr<std::atomic_bool>& stop_requested) {
                using namespace std::chrono_literals;

                std::unique_lock lock(mutex_);
                while (!shutting_down_ && pending_connections_.size() >= max_pending_connections_) {
                    if (is_stop_requested(stop_requested)) {
                        return false;
                    }
                    slot_available_.wait_for(lock, 50ms);
                }
                return !shutting_down_ && !is_stop_requested(stop_requested);
            }

            bool enqueue_ready(int client_fd) {
                std::lock_guard lock(mutex_);
                if (shutting_down_ || pending_connections_.size() >= max_pending_connections_) {
                    return false;
                }
                pending_connections_.push_back(client_fd);
                connection_ready_.notify_one();
                return true;
            }

            bool dequeue(int& client_fd) {
                std::unique_lock lock(mutex_);
                connection_ready_.wait(lock, [&] {
                    return shutting_down_ || !pending_connections_.empty();
                });

                // Shutdown is drain-aware: workers keep consuming sockets that were
                // already accepted into the queue before they finally exit.
                if (pending_connections_.empty()) {
                    client_fd = -1;
                    return false;
                }

                client_fd = pending_connections_.front();
                pending_connections_.pop_front();
                slot_available_.notify_one();
                return true;
            }

            void shutdown() {
                std::lock_guard lock(mutex_);
                shutting_down_ = true;
                connection_ready_.notify_all();
                slot_available_.notify_all();
            }

        private:
            const std::size_t max_pending_connections_{ 1 };
            std::deque<int> pending_connections_{};
            bool shutting_down_{ false };
            std::mutex mutex_{};
            std::condition_variable connection_ready_{};
            std::condition_variable slot_available_{};
        };

        enum class ListenWaitStatus {
            Ready = 0,
            StopRequested = 1,
            Error = 2,
        };

        ListenWaitStatus wait_for_next_connection(
            int server_fd,
            const std::shared_ptr<std::atomic_bool>& stop_requested,
            std::string& err) {
            pollfd descriptor{};
            descriptor.fd = server_fd;
            descriptor.events = POLLIN;

            while (!is_stop_requested(stop_requested)) {
                const int poll_result = ::poll(&descriptor, 1, 50);
                if (poll_result == 0) {
                    continue;
                }
                if (poll_result < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    err = std::string("listen poll failed: ") + std::strerror(errno);
                    return ListenWaitStatus::Error;
                }
                if ((descriptor.revents & POLLIN) != 0) {
                    err.clear();
                    return ListenWaitStatus::Ready;
                }
                if ((descriptor.revents & (POLLERR | POLLNVAL | POLLHUP)) != 0) {
                    err = "listen socket entered an invalid state";
                    return ListenWaitStatus::Error;
                }
            }

            err.clear();
            return ListenWaitStatus::StopRequested;
        }

        void log_line(std::ostream& stream, std::mutex& mutex, const std::string& line) {
            std::lock_guard lock(mutex);
            stream << line;
        }

        SecurityLogFn make_security_logger(std::ostream& stream, std::mutex& mutex) {
            return [&stream, &mutex](std::string_view line) {
                log_line(stream, mutex, "SECURITY " + std::string(line) + "\n");
            };
        }

        void close_socket_quietly(int fd) {
            if (fd >= 0) {
                ::close(fd);
            }
        }

        bool parse_u16_option(
            std::string_view text,
            const char* label,
            std::uint16_t& out,
            std::ostream& err) {
            try {
                const unsigned long parsed = std::stoul(std::string(text));
                if (parsed > std::numeric_limits<std::uint16_t>::max()) {
                    err << "Error: " << label << " is out of range\n";
                    return false;
                }
                out = static_cast<std::uint16_t>(parsed);
                return true;
            }
            catch (const std::exception&) {
                err << "Error: " << label << " must be an integer\n";
                return false;
            }
        }

        bool parse_u32_option(
            std::string_view text,
            const char* label,
            std::uint32_t min_value,
            std::uint32_t max_value,
            std::uint32_t& out,
            std::ostream& err) {
            try {
                const unsigned long parsed = std::stoul(std::string(text));
                if (parsed < min_value || parsed > max_value) {
                    err << "Error: " << label << " must be in [" << min_value << ", " << max_value << "]\n";
                    return false;
                }
                out = static_cast<std::uint32_t>(parsed);
                return true;
            }
            catch (const std::exception&) {
                err << "Error: " << label << " must be an integer\n";
                return false;
            }
        }

        bool parse_size_option(
            std::string_view text,
            const char* label,
            std::size_t min_value,
            std::size_t max_value,
            std::size_t& out,
            std::ostream& err) {
            try {
                const unsigned long long parsed = std::stoull(std::string(text));
                if (parsed < min_value || parsed > max_value) {
                    err << "Error: " << label << " must be in [" << min_value << ", " << max_value << "]\n";
                    return false;
                }
                out = static_cast<std::size_t>(parsed);
                return true;
            }
            catch (const std::exception&) {
                err << "Error: " << label << " must be an integer\n";
                return false;
            }
        }

        bool is_loopback_bind_host(std::string_view host) {
            in_addr addr{};
            if (::inet_pton(AF_INET, std::string(host).c_str(), &addr) != 1) {
                return false;
            }

            const std::uint32_t host_order = ntohl(addr.s_addr);
            return (host_order & 0xFF000000u) == 0x7F000000u;
        }

        bool is_valid_ipv4_bind_host(std::string_view host) {
            in_addr addr{};
            return ::inet_pton(AF_INET, std::string(host).c_str(), &addr) == 1;
        }

        bool set_socket_timeout(int fd, int optname, std::uint32_t timeout_ms, std::string& err) {
            timeval timeout{};
            timeout.tv_sec = static_cast<decltype(timeout.tv_sec)>(timeout_ms / 1000u);
            timeout.tv_usec = static_cast<decltype(timeout.tv_usec)>((timeout_ms % 1000u) * 1000u);
            if (timeout.tv_sec == 0 && timeout.tv_usec == 0) {
                timeout.tv_usec = 1000;
            }

            if (::setsockopt(fd, SOL_SOCKET, optname, &timeout, sizeof(timeout)) != 0) {
                err = std::string("failed to set socket timeout: ") + std::strerror(errno);
                return false;
            }
            err.clear();
            return true;
        }

        bool set_socket_flag(int fd, int level, int optname, int value, std::string& err) {
            if (::setsockopt(fd, level, optname, &value, sizeof(value)) != 0) {
                err = std::string("failed to set socket option: ") + std::strerror(errno);
                return false;
            }
            err.clear();
            return true;
        }

        bool configure_connected_socket(int fd, std::uint32_t socket_timeout_ms, std::string& err) {
            const int enabled = 1;
            return
                set_socket_timeout(fd, SO_RCVTIMEO, socket_timeout_ms, err) &&
                set_socket_timeout(fd, SO_SNDTIMEO, socket_timeout_ms, err) &&
                set_socket_flag(fd, SOL_SOCKET, SO_KEEPALIVE, enabled, err) &&
                set_socket_flag(fd, IPPROTO_TCP, TCP_NODELAY, enabled, err);
        }

    } // namespace

    int run_tcp_server(const ServerCliOptions& options, std::ostream& out, std::ostream& err) {
        if (!is_valid_ipv4_bind_host(options.bind_host)) {
            err << "Error: invalid bind host '" << options.bind_host << "'\n";
            return 1;
        }
        if (!options.allow_remote_unsafe && !is_loopback_bind_host(options.bind_host)) {
            err << "Error: non-loopback bind host '" << options.bind_host
                << "' requires --allow-remote-unsafe\n";
            return 1;
        }
        if (options.allow_remote_unsafe && !is_loopback_bind_host(options.bind_host)) {
            err << "Warning: non-loopback bind host '" << options.bind_host
                << "' is running without authentication or transport protection\n";
        }

        ProtocolDispatcherConfig dispatcher_config{};
        dispatcher_config.runtime.storage_root = options.storage_root;
        dispatcher_config.runtime.quotas.server_worker_limit = options.worker_limit;
        dispatcher_config.runtime.quotas.max_total_active_jobs = options.max_active_jobs;
        dispatcher_config.runtime.quotas.max_live_sessions = options.max_live_sessions;
        dispatcher_config.source_root = options.source_root;
        PendingConnectionQueue pending_connections(options.max_pending_connections);
        std::mutex log_mutex{};
        dispatcher_config.security_log = make_security_logger(out, log_mutex);
        ProtocolDispatcher dispatcher(dispatcher_config);
        if (options.allow_remote_unsafe && !is_loopback_bind_host(options.bind_host)) {
            dispatcher_config.security_log(
                "event=remote_bind result=unsafe host=" + options.bind_host);
        }

        const std::size_t connection_worker_limit =
            std::max<std::size_t>(std::size_t{ 1 }, options.connection_worker_limit);
        std::vector<std::thread> connection_workers;
        connection_workers.reserve(connection_worker_limit);
        for (std::size_t i = 0; i < connection_worker_limit; ++i) {
            connection_workers.emplace_back([&, i] {
                while (true) {
                    int client_fd = -1;
                    if (!pending_connections.dequeue(client_fd)) {
                        return;
                    }

                    std::string connection_err;
                    if (!handle_protocol_socket_connection(
                            client_fd,
                            dispatcher,
                            options.max_message_bytes,
                            dispatcher_config.security_log,
                            connection_err) &&
                        !connection_err.empty()) {
                        log_line(
                            err,
                            log_mutex,
                            "Warning: connection worker " + std::to_string(i + 1) +
                                " failed: " + connection_err + "\n");
                    }

                    close_socket_quietly(client_fd);
                }
            });
        }

        const int server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd < 0) {
            pending_connections.shutdown();
            for (auto& worker : connection_workers) {
                worker.join();
            }
            err << "Error: failed to create socket: " << std::strerror(errno) << "\n";
            return 1;
        }

        int reuse = 1;
        ::setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(options.port);
        if (::inet_pton(AF_INET, options.bind_host.c_str(), &addr.sin_addr) != 1) {
            pending_connections.shutdown();
            for (auto& worker : connection_workers) {
                worker.join();
            }
            err << "Error: invalid bind host '" << options.bind_host << "'\n";
            ::close(server_fd);
            return 1;
        }
        if (::bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            pending_connections.shutdown();
            for (auto& worker : connection_workers) {
                worker.join();
            }
            err << "Error: bind failed: " << std::strerror(errno) << "\n";
            ::close(server_fd);
            return 1;
        }
        const int listen_backlog = static_cast<int>(std::min<std::size_t>(
            static_cast<std::size_t>(SOMAXCONN),
            connection_worker_limit + std::max<std::size_t>(std::size_t{ 1 }, options.max_pending_connections)));
        if (::listen(server_fd, listen_backlog) != 0) {
            pending_connections.shutdown();
            for (auto& worker : connection_workers) {
                worker.join();
            }
            err << "Error: listen failed: " << std::strerror(errno) << "\n";
            ::close(server_fd);
            return 1;
        }

        out << "sugar_server listening on " << options.bind_host << ":" << options.port << "\n";

        int exit_code = 0;
        std::size_t served_connections = 0;
        while (true) {
            if (!pending_connections.wait_for_enqueue_slot(options.stop_requested)) {
                break;
            }

            std::string wait_err;
            const ListenWaitStatus wait_status =
                wait_for_next_connection(server_fd, options.stop_requested, wait_err);
            if (wait_status == ListenWaitStatus::StopRequested) {
                break;
            }
            if (wait_status == ListenWaitStatus::Error) {
                log_line(err, log_mutex, "Error: " + wait_err + "\n");
                exit_code = 1;
                break;
            }

            const int client_fd = ::accept(server_fd, nullptr, nullptr);
            if (client_fd < 0) {
                if (errno == EINTR) {
                    continue;
                }
                log_line(err, log_mutex, std::string("Error: accept failed: ") + std::strerror(errno) + "\n");
                exit_code = 1;
                break;
            }
            std::string socket_err;
            if (!configure_connected_socket(client_fd, options.socket_timeout_ms, socket_err)) {
                log_line(err, log_mutex, "Warning: failed to configure accepted socket: " + socket_err + "\n");
                close_socket_quietly(client_fd);
                continue;
            }

            if (!pending_connections.enqueue_ready(client_fd)) {
                close_socket_quietly(client_fd);
                break;
            }

            ++served_connections;
            if (options.serve_once && served_connections >= 1) {
                break;
            }
        }

        pending_connections.shutdown();
        for (auto& worker : connection_workers) {
            worker.join();
        }
        ::close(server_fd);
        return exit_code;
    }

    int run_tcp_server_from_cli(int argc, char** argv, std::ostream& out, std::ostream& err) {
        ServerCliOptions options{};
#ifdef SUGARBOT_SOURCE_DIR
        options.source_root = SUGARBOT_SOURCE_DIR;
#endif
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            auto require_value = [&](const char* label, std::string& value) -> bool {
                if (i + 1 >= argc) {
                    err << "Error: missing value for " << label << "\n";
                    return false;
                }
                value = argv[++i];
                return true;
            };

            if (arg == "--host") {
                if (!require_value("--host", options.bind_host)) return 1;
            }
            else if (arg == "--allow-remote-unsafe") {
                options.allow_remote_unsafe = true;
            }
            else if (arg == "--port") {
                std::string port_text;
                if (!require_value("--port", port_text)) return 1;
                if (!parse_u16_option(port_text, "--port", options.port, err)) return 1;
            }
            else if (arg == "--storage-root") {
                std::string path;
                if (!require_value("--storage-root", path)) return 1;
                options.storage_root = path;
            }
            else if (arg == "--source-root") {
                std::string path;
                if (!require_value("--source-root", path)) return 1;
                options.source_root = path;
            }
            else if (arg == "--once") {
                options.serve_once = true;
            }
            else if (arg == "--workers") {
                std::string worker_text;
                if (!require_value("--workers", worker_text)) return 1;
                if (!parse_size_option(worker_text, "--workers", 1, 256, options.worker_limit, err)) return 1;
            }
            else if (arg == "--max-active-jobs") {
                std::string limit_text;
                if (!require_value("--max-active-jobs", limit_text)) return 1;
                if (!parse_size_option(limit_text, "--max-active-jobs", 1, 256, options.max_active_jobs, err)) return 1;
            }
            else if (arg == "--max-live-sessions") {
                std::string limit_text;
                if (!require_value("--max-live-sessions", limit_text)) return 1;
                if (!parse_size_option(limit_text, "--max-live-sessions", 1, 256, options.max_live_sessions, err)) return 1;
            }
            else if (arg == "--connection-workers") {
                std::string worker_text;
                if (!require_value("--connection-workers", worker_text)) return 1;
                if (!parse_size_option(worker_text, "--connection-workers", 1, 256, options.connection_worker_limit, err)) return 1;
            }
            else if (arg == "--max-pending-connections") {
                std::string limit_text;
                if (!require_value("--max-pending-connections", limit_text)) return 1;
                if (!parse_size_option(limit_text, "--max-pending-connections", 1, 4096, options.max_pending_connections, err)) return 1;
            }
            else if (arg == "--max-message-bytes") {
                std::string limit_text;
                if (!require_value("--max-message-bytes", limit_text)) return 1;
                if (!parse_size_option(limit_text, "--max-message-bytes", 256, 64 * 1024 * 1024, options.max_message_bytes, err)) return 1;
            }
            else if (arg == "--socket-timeout-ms") {
                std::string timeout_text;
                if (!require_value("--socket-timeout-ms", timeout_text)) return 1;
                if (!parse_u32_option(timeout_text, "--socket-timeout-ms", 1, 600000, options.socket_timeout_ms, err)) return 1;
            }
            else {
                err << "Error: unknown server option '" << arg << "'\n";
                return 1;
            }
        }

        return run_tcp_server(options, out, err);
    }

} // namespace sugar::service

#endif
