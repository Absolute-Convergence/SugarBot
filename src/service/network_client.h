#pragma once

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <string>

namespace sugar::service {

    struct ClientCliOptions {
        std::string host{ "127.0.0.1" };
        std::uint16_t port{ 4123 };
        std::size_t max_message_bytes{ 4 * 1024 * 1024 };
        std::uint32_t socket_timeout_ms{ 30000 };
        bool compact_output{ false };
    };

    int run_tcp_client_from_cli(int argc, char** argv, std::ostream& out, std::ostream& err);

} // namespace sugar::service
