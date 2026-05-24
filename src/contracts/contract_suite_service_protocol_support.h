#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "service/network_server.h"
#include "service/network_session.h"
#include "service/protocol_dispatch.h"
#include "service/protocol_framing.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#endif

namespace sugar::contract_suite_internal::service_protocol_contract {

    inline constexpr std::size_t kProtocolMessageLimit = 4 * 1024 * 1024;
    inline constexpr std::size_t kSocketContractFrameLimit = 4096;
    inline constexpr auto kLiveServerStartTimeout = std::chrono::seconds(2);
    inline constexpr auto kTcpTerminalWaitTimeout = std::chrono::seconds(30);
    inline constexpr auto kTcpCancelPollerTimeout = std::chrono::seconds(10);
    inline constexpr auto kTcpCancelActiveWaitTimeout = std::chrono::seconds(5);
    inline constexpr auto kTcpPollInterval = std::chrono::milliseconds(50);
    inline constexpr auto kTcpWaitInterval = std::chrono::milliseconds(5);

#ifdef _WIN32
    inline constexpr sugar::service::NativeSocketHandle kInvalidSocketHandle =
        static_cast<sugar::service::NativeSocketHandle>(INVALID_SOCKET);
#else
    inline constexpr sugar::service::NativeSocketHandle kInvalidSocketHandle = -1;
#endif

    int last_socket_error();
    std::string socket_error_message(int error_code);
    bool is_interrupted_socket_error(int error_code);
    bool reserve_loopback_port(std::uint16_t& out_port, std::string& err);
    bool can_connect_loopback(std::uint16_t port);

#ifdef _WIN32
    class ScopedSocketRuntime {
    public:
        explicit ScopedSocketRuntime(std::string& err) {
            WSADATA data{};
            const int result = WSAStartup(MAKEWORD(2, 2), &data);
            if (result != 0) {
                err = socket_error_message(result);
                return;
            }
            active_ = true;
            err.clear();
        }

        ~ScopedSocketRuntime() {
            if (active_) {
                WSACleanup();
            }
        }

        bool active() const {
            return active_;
        }

    private:
        bool active_{ false };
    };
#else
    class ScopedSocketRuntime {
    public:
        explicit ScopedSocketRuntime(std::string& err) {
            err.clear();
        }

        bool active() const {
            return true;
        }
    };
#endif

    struct ParsedProtocolResponse {
        sugar::service::ProtocolCommand command{ sugar::service::ProtocolCommand::Unknown };
        bool ok{ false };
        sugar::service::ProtocolErrorCode error_code{ sugar::service::ProtocolErrorCode::InternalError };
        std::string schema_version{};
        std::string message{};
        std::string session_id{};
        std::string job_id{};
        std::string upload_id{};
        bool has_job_status{ false };
        sugar::service::JobState job_state{ sugar::service::JobState::Queued };
        std::string job_owner_session_id{};
        std::string job_failure_message{};
        bool has_job_result{ false };
        std::string result_owner_session_id{};
        bool result_has_primary_backtest{ false };
        bool result_has_sweep_report{ false };
        std::size_t result_sweep_top_candidate_count{ 0 };
    };

    bool is_valid_socket(sugar::service::NativeSocketHandle handle);
    void close_socket_quietly(sugar::service::NativeSocketHandle fd);
    void shutdown_socket_write(sugar::service::NativeSocketHandle fd);
    bool read_exact(sugar::service::NativeSocketHandle fd, char* buffer, std::size_t size);
    bool write_all(sugar::service::NativeSocketHandle fd, const char* buffer, std::size_t size);
    bool read_framed_json(
        sugar::service::NativeSocketHandle fd,
        std::size_t max_message_bytes,
        std::string& out_json,
        std::string& err);
    bool parse_protocol_response_json(
        std::string_view json,
        ParsedProtocolResponse& out,
        std::string& err);

    std::string single_series_layout_json();
    std::string single_series_sweep_logic_json(std::size_t max_evals);
    std::string single_series_csv_text(std::size_t rows = 3);
    bool write_text_file(
        const std::filesystem::path& path,
        std::string_view content,
        std::string& err);

    class ScopedLiveTcpServer {
    public:
        ScopedLiveTcpServer() = default;

        ~ScopedLiveTcpServer() {
            stop();
        }

        bool start(sugar::service::ServerCliOptions options, std::string& err) {
            stop();

            if (!reserve_loopback_port(port_, err)) {
                return false;
            }

            if (options.bind_host.empty()) {
                options.bind_host = "127.0.0.1";
            }
            options.port = port_;
            stop_requested_ = std::make_shared<std::atomic_bool>(false);
            options.stop_requested = stop_requested_;
            options_ = std::move(options);

            server_thread_ = std::thread([this] {
                exit_code_ = sugar::service::run_tcp_server(options_, server_out_, server_err_);
            });

            const auto deadline = std::chrono::steady_clock::now() + kLiveServerStartTimeout;
            while (std::chrono::steady_clock::now() < deadline) {
                if (can_connect_loopback(port_)) {
                    err.clear();
                    return true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }

            err = "timed out waiting for live TCP server to listen";
            stop();
            return false;
        }

        void stop() {
            if (stop_requested_) {
                stop_requested_->store(true, std::memory_order_relaxed);
            }
            if (server_thread_.joinable()) {
                server_thread_.join();
            }
            stop_requested_.reset();
        }

        std::uint16_t port() const {
            return port_;
        }

        int exit_code() const {
            return exit_code_;
        }

        std::string stderr_text() const {
            return server_err_.str();
        }

        std::string stdout_text() const {
            return server_out_.str();
        }

    private:
        sugar::service::ServerCliOptions options_{};
        std::uint16_t port_{ 0 };
        std::shared_ptr<std::atomic_bool> stop_requested_{};
        std::thread server_thread_{};
        int exit_code_{ 0 };
        std::ostringstream server_out_{};
        std::ostringstream server_err_{};
    };

    bool send_tcp_protocol_request(
        std::uint16_t port,
        const sugar::service::ProtocolRequest& request,
        ParsedProtocolResponse& out_response,
        std::string& out_response_json,
        std::string& err);
    bool send_tcp_protocol_request(
        std::uint16_t port,
        const sugar::service::ProtocolRequest& request,
        ParsedProtocolResponse& out_response,
        std::string& err);
    bool open_connected_loopback_socket(
        std::uint16_t port,
        sugar::service::NativeSocketHandle& out_fd,
        std::string& err);
    bool make_connected_socket_pair(
        sugar::service::NativeSocketHandle (&fds)[2],
        std::string& err);
    bool connect_session(std::uint16_t port, std::string& out_session_id, std::string& err);
    bool upload_artifact(
        std::uint16_t port,
        const std::string& session_id,
        sugar::service::ArtifactKind kind,
        std::string_view client_name,
        std::string_view content,
        std::string& out_upload_id,
        std::string& err);
    bool submit_job(
        std::uint16_t port,
        const std::string& session_id,
        const std::string& layout_upload_id,
        const std::string& logic_upload_id,
        const std::string& data_upload_id,
        std::string& out_job_id,
        std::string& err);
    bool disconnect_session(std::uint16_t port, const std::string& session_id, std::string& err);
    bool wait_for_terminal_status_over_tcp(
        std::uint16_t port,
        const std::string& session_id,
        const std::string& job_id,
        sugar::service::JobState& out_state,
        bool& out_saw_active_state,
        std::string& out_failure_message,
        std::size_t& out_poll_count,
        std::string& err);

} // namespace sugar::contract_suite_internal::service_protocol_contract
