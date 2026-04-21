#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <memory>
#include <string>

#include "service/protocol_dispatch.h"

namespace sugar::service {

    struct ServerCliOptions {
        std::string bind_host{ "127.0.0.1" };
        std::uint16_t port{ 4123 };
        bool serve_once{ false };
        std::filesystem::path storage_root{};
        std::filesystem::path source_root{};
        std::size_t max_message_bytes{ 4 * 1024 * 1024 };
        std::size_t worker_limit{ 1 };
        std::size_t max_active_jobs{ 0 };
        std::size_t connection_worker_limit{ 4 };
        std::size_t max_pending_connections{ 32 };
        std::uint32_t socket_timeout_ms{ 30000 };
        std::shared_ptr<std::atomic_bool> stop_requested{};
    };

    int run_tcp_server(const ServerCliOptions& options, std::ostream& out, std::ostream& err);
    int run_tcp_server_from_cli(int argc, char** argv, std::ostream& out, std::ostream& err);

} // namespace sugar::service
