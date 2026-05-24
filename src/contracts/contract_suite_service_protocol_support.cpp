#include "contract_suite_internal.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "ingest/json_min.h"
#include "service/network_client.h"
#include "service/network_server.h"
#include "service/network_session.h"
#include "service/protocol_dispatch.h"
#include "service/protocol_framing.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace sugar::contract_suite_internal::service_protocol_contract {

        constexpr std::size_t kProtocolMessageLimit = 4 * 1024 * 1024;
        constexpr std::size_t kSocketContractFrameLimit = 4096;
        constexpr auto kLiveServerStartTimeout = std::chrono::seconds(2);
        constexpr auto kTcpTerminalWaitTimeout = std::chrono::seconds(30);
        constexpr auto kTcpCancelPollerTimeout = std::chrono::seconds(10);
        constexpr auto kTcpCancelActiveWaitTimeout = std::chrono::seconds(5);
        constexpr auto kTcpPollInterval = std::chrono::milliseconds(50);
        constexpr auto kTcpWaitInterval = std::chrono::milliseconds(5);

#ifdef _WIN32
        constexpr sugar::service::NativeSocketHandle kInvalidSocketHandle =
            static_cast<sugar::service::NativeSocketHandle>(INVALID_SOCKET);
        constexpr std::uint16_t kWindowsLoopbackTestPortBase = 20000;
        constexpr std::uint16_t kWindowsLoopbackTestPortCount = 20000;

        SOCKET to_socket(sugar::service::NativeSocketHandle handle) {
            return static_cast<SOCKET>(handle);
        }

        sugar::service::NativeSocketHandle to_native_socket_handle(SOCKET socket) {
            return static_cast<sugar::service::NativeSocketHandle>(socket);
        }

        int last_socket_error() {
            return WSAGetLastError();
        }

        bool is_interrupted_socket_error(int error_code) {
            return error_code == WSAEINTR;
        }

        std::string socket_error_message(int error_code) {
            return "WinSock error " + std::to_string(error_code);
        }

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
        constexpr sugar::service::NativeSocketHandle kInvalidSocketHandle = -1;

        int to_socket(sugar::service::NativeSocketHandle handle) {
            return handle;
        }

        sugar::service::NativeSocketHandle to_native_socket_handle(int socket) {
            return socket;
        }

        int last_socket_error() {
            return errno;
        }

        bool is_interrupted_socket_error(int error_code) {
            return error_code == EINTR;
        }

        std::string socket_error_message(int error_code) {
            return std::strerror(error_code);
        }

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

        bool is_valid_socket(sugar::service::NativeSocketHandle handle) {
            return handle != kInvalidSocketHandle;
        }

#ifdef _WIN32
        bool is_address_in_use_error(int error_code) {
            return error_code == WSAEADDRINUSE || error_code == WSAEACCES;
        }

        std::uint16_t next_windows_loopback_test_port() {
            static std::atomic_uint counter{ 0 };
            const auto tick = static_cast<unsigned int>(
                std::chrono::steady_clock::now().time_since_epoch().count());
            const unsigned int offset =
                (counter.fetch_add(1, std::memory_order_relaxed) + tick) %
                kWindowsLoopbackTestPortCount;
            return static_cast<std::uint16_t>(kWindowsLoopbackTestPortBase + offset);
        }
#endif

        void close_socket_quietly(sugar::service::NativeSocketHandle fd) {
            if (!is_valid_socket(fd)) {
                return;
            }
#ifdef _WIN32
            ::closesocket(to_socket(fd));
#else
            ::close(fd);
#endif
        }

        void shutdown_socket_write(sugar::service::NativeSocketHandle fd) {
            if (!is_valid_socket(fd)) {
                return;
            }
#ifdef _WIN32
            ::shutdown(to_socket(fd), SD_SEND);
#else
            ::shutdown(fd, SHUT_WR);
#endif
        }

        bool bind_loopback_test_listener(
            sugar::service::NativeSocketHandle fd,
            std::uint16_t requested_port,
            std::uint16_t& out_port,
            std::string& err) {
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(requested_port);
            if (::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
                err = "failed to encode loopback listener address";
                return false;
            }

            if (::bind(to_socket(fd), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
                err = "failed to bind loopback listener: " + socket_error_message(last_socket_error());
                return false;
            }

            sockaddr_in bound_addr{};
#ifdef _WIN32
            int bound_size = sizeof(bound_addr);
#else
            socklen_t bound_size = sizeof(bound_addr);
#endif
            if (::getsockname(to_socket(fd), reinterpret_cast<sockaddr*>(&bound_addr), &bound_size) != 0) {
                err = "failed to query loopback listener: " + socket_error_message(last_socket_error());
                return false;
            }

            out_port = ntohs(bound_addr.sin_port);
            err.clear();
            return true;
        }

        bool bind_available_loopback_test_listener(
            sugar::service::NativeSocketHandle fd,
            std::uint16_t& out_port,
            std::string& err) {
#ifdef _WIN32
            std::string last_err;
            for (std::size_t attempt = 0; attempt < kWindowsLoopbackTestPortCount; ++attempt) {
                sockaddr_in addr{};
                addr.sin_family = AF_INET;
                addr.sin_port = htons(next_windows_loopback_test_port());
                if (::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
                    err = "failed to encode loopback listener address";
                    return false;
                }
                if (::bind(to_socket(fd), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
                    sockaddr_in bound_addr{};
                    int bound_size = sizeof(bound_addr);
                    if (::getsockname(to_socket(fd), reinterpret_cast<sockaddr*>(&bound_addr), &bound_size) != 0) {
                        err = "failed to query loopback listener: " + socket_error_message(last_socket_error());
                        return false;
                    }
                    out_port = ntohs(bound_addr.sin_port);
                    err.clear();
                    return true;
                }

                const int error_code = last_socket_error();
                last_err = socket_error_message(error_code);
                if (!is_address_in_use_error(error_code)) {
                    err = "failed to bind loopback listener: " + last_err;
                    return false;
                }
            }

            err = "failed to bind loopback listener after probing Windows test ports: " + last_err;
            return false;
#else
            return bind_loopback_test_listener(fd, 0, out_port, err);
#endif
        }

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

        bool read_exact(sugar::service::NativeSocketHandle fd, char* buffer, std::size_t size) {
            std::size_t offset = 0;
            while (offset < size) {
                const int chunk_size = static_cast<int>(std::min<std::size_t>(
                    size - offset,
                    static_cast<std::size_t>(std::numeric_limits<int>::max())));
                const auto got = ::recv(to_socket(fd), buffer + offset, chunk_size, 0);
                if (got <= 0) {
                    const int error_code = last_socket_error();
                    if (got < 0 && is_interrupted_socket_error(error_code)) {
                        continue;
                    }
                    return false;
                }
                offset += static_cast<std::size_t>(got);
            }
            return true;
        }

        bool write_all(sugar::service::NativeSocketHandle fd, const char* buffer, std::size_t size) {
            std::size_t offset = 0;
            while (offset < size) {
#if defined(MSG_NOSIGNAL)
                constexpr int kSendFlags = MSG_NOSIGNAL;
#else
                constexpr int kSendFlags = 0;
#endif
                const int chunk_size = static_cast<int>(std::min<std::size_t>(
                    size - offset,
                    static_cast<std::size_t>(std::numeric_limits<int>::max())));
                const auto sent = ::send(to_socket(fd), buffer + offset, chunk_size, kSendFlags);
                if (sent <= 0) {
                    const int error_code = last_socket_error();
                    if (sent < 0 && is_interrupted_socket_error(error_code)) {
                        continue;
                    }
                    return false;
                }
                offset += static_cast<std::size_t>(sent);
            }
            return true;
        }

        bool read_framed_json(
            sugar::service::NativeSocketHandle fd,
            std::size_t max_message_bytes,
            std::string& out_json,
            std::string& err) {
            char header[4]{};
            if (!read_exact(fd, header, sizeof(header))) {
                err = "failed to read response header";
                return false;
            }

            const std::uint32_t size =
                (static_cast<std::uint32_t>(static_cast<unsigned char>(header[0])) << 24) |
                (static_cast<std::uint32_t>(static_cast<unsigned char>(header[1])) << 16) |
                (static_cast<std::uint32_t>(static_cast<unsigned char>(header[2])) << 8) |
                static_cast<std::uint32_t>(static_cast<unsigned char>(header[3]));
            if (size == 0 || size > max_message_bytes) {
                err = "response frame length is invalid";
                return false;
            }

            out_json.assign(size, '\0');
            if (!read_exact(fd, out_json.data(), out_json.size())) {
                err = "failed to read response body";
                return false;
            }

            err.clear();
            return true;
        }

        bool parse_protocol_command_name(
            std::string_view name,
            sugar::service::ProtocolCommand& out_command,
            std::string& err) {
            using sugar::service::ProtocolCommand;

            if (name == "connect") {
                out_command = ProtocolCommand::Connect;
            }
            else if (name == "list_datasets") {
                out_command = ProtocolCommand::ListDatasets;
            }
            else if (name == "list_templates") {
                out_command = ProtocolCommand::ListTemplates;
            }
            else if (name == "upload_artifact") {
                out_command = ProtocolCommand::UploadArtifact;
            }
            else if (name == "validate_request") {
                out_command = ProtocolCommand::ValidateRequest;
            }
            else if (name == "submit_job") {
                out_command = ProtocolCommand::SubmitJob;
            }
            else if (name == "check_status") {
                out_command = ProtocolCommand::CheckStatus;
            }
            else if (name == "retrieve_results") {
                out_command = ProtocolCommand::RetrieveResults;
            }
            else if (name == "cancel_job") {
                out_command = ProtocolCommand::CancelJob;
            }
            else if (name == "disconnect") {
                out_command = ProtocolCommand::Disconnect;
            }
            else if (name == "unknown") {
                out_command = ProtocolCommand::Unknown;
            }
            else {
                err = "protocol response has unknown op";
                return false;
            }

            err.clear();
            return true;
        }

        bool parse_protocol_error_code_name(
            std::string_view name,
            sugar::service::ProtocolErrorCode& out_code,
            std::string& err) {
            using sugar::service::ProtocolErrorCode;

            if (name == "none") {
                out_code = ProtocolErrorCode::None;
            }
            else if (name == "malformed_request") {
                out_code = ProtocolErrorCode::MalformedRequest;
            }
            else if (name == "unauthorized_request") {
                out_code = ProtocolErrorCode::UnauthorizedRequest;
            }
            else if (name == "invalid_artifact") {
                out_code = ProtocolErrorCode::InvalidArtifact;
            }
            else if (name == "invalid_plan_data_compatibility") {
                out_code = ProtocolErrorCode::InvalidPlanDataCompatibility;
            }
            else if (name == "quota_exceeded") {
                out_code = ProtocolErrorCode::QuotaExceeded;
            }
            else if (name == "not_found") {
                out_code = ProtocolErrorCode::NotFound;
            }
            else if (name == "invalid_state") {
                out_code = ProtocolErrorCode::InvalidState;
            }
            else if (name == "unsupported_platform") {
                out_code = ProtocolErrorCode::UnsupportedPlatform;
            }
            else if (name == "internal_error") {
                out_code = ProtocolErrorCode::InternalError;
            }
            else {
                err = "protocol response has unknown error_code";
                return false;
            }

            err.clear();
            return true;
        }

        bool parse_job_state_name(
            std::string_view name,
            sugar::service::JobState& out_state,
            std::string& err) {
            using sugar::service::JobState;

            if (name == "queued") {
                out_state = JobState::Queued;
            }
            else if (name == "validating") {
                out_state = JobState::Validating;
            }
            else if (name == "rejected") {
                out_state = JobState::Rejected;
            }
            else if (name == "running") {
                out_state = JobState::Running;
            }
            else if (name == "cancel_requested") {
                out_state = JobState::CancelRequested;
            }
            else if (name == "canceled") {
                out_state = JobState::Canceled;
            }
            else if (name == "completed") {
                out_state = JobState::Completed;
            }
            else if (name == "failed") {
                out_state = JobState::Failed;
            }
            else {
                err = "protocol response has unknown job state";
                return false;
            }

            err.clear();
            return true;
        }

        bool parse_protocol_response_json(
            std::string_view json,
            ParsedProtocolResponse& out,
            std::string& err) {
            using namespace sugar::json_min;

            Value root;
            ParseError parse_error{};
            if (!parse(json, root, parse_error)) {
                err = parse_error.message;
                return false;
            }
            if (!root.is_obj()) {
                err = "protocol response root must be object";
                return false;
            }

            const auto& object = root.as_obj();
            const auto* ok_value = obj_get(object, "ok");
            const auto* schema_version_value = obj_get(object, "schema_version");
            const auto* op_value = obj_get(object, "op");
            const auto* error_value = obj_get(object, "error_code");
            const auto* message_value = obj_get(object, "message");
            if (!ok_value || !ok_value->is_bool()) {
                err = "protocol response missing bool ok field";
                return false;
            }
            if (!schema_version_value || !schema_version_value->is_str()) {
                err = "protocol response missing string schema_version field";
                return false;
            }
            if (schema_version_value->as_str() != sugar::service::hosted_observability_schema_version()) {
                err = "protocol response uses unsupported hosted observability schema version";
                return false;
            }
            if (!op_value || !op_value->is_str()) {
                err = "protocol response missing string op field";
                return false;
            }
            if (!error_value || !error_value->is_str()) {
                err = "protocol response missing string error_code field";
                return false;
            }
            if (!message_value || !message_value->is_str()) {
                err = "protocol response missing string message field";
                return false;
            }

            out = ParsedProtocolResponse{};
            out.ok = ok_value->as_bool();
            out.schema_version = schema_version_value->as_str();
            out.message = message_value->as_str();

            if (!parse_protocol_command_name(op_value->as_str(), out.command, err) ||
                !parse_protocol_error_code_name(error_value->as_str(), out.error_code, err)) {
                return false;
            }

            if (const auto* session_value = obj_get(object, "session_id"); session_value != nullptr) {
                if (!session_value->is_str()) {
                    err = "protocol response session_id must be a string";
                    return false;
                }
                out.session_id = session_value->as_str();
            }

            if (const auto* job_value = obj_get(object, "job_id"); job_value != nullptr) {
                if (!job_value->is_str()) {
                    err = "protocol response job_id must be a string";
                    return false;
                }
                out.job_id = job_value->as_str();
            }

            if (const auto* upload_value = obj_get(object, "upload"); upload_value != nullptr) {
                if (!upload_value->is_obj()) {
                    err = "protocol response upload must be an object";
                    return false;
                }
                const auto* upload_id_value = obj_get(upload_value->as_obj(), "upload_id");
                if (!upload_id_value || !upload_id_value->is_str()) {
                    err = "protocol response upload.upload_id must be a string";
                    return false;
                }
                out.upload_id = upload_id_value->as_str();
            }

            if (const auto* job_status_value = obj_get(object, "job_status"); job_status_value != nullptr) {
                if (!job_status_value->is_obj()) {
                    err = "protocol response job_status must be an object";
                    return false;
                }
                const auto& job_status_object = job_status_value->as_obj();
                const auto* state_value = obj_get(job_status_object, "state");
                const auto* owner_value = obj_get(job_status_object, "owner_session_id");
                const auto* failure_value = obj_get(job_status_object, "failure_message");
                if (!state_value || !state_value->is_str()) {
                    err = "protocol response job_status.state must be a string";
                    return false;
                }
                if (!owner_value || !owner_value->is_str()) {
                    err = "protocol response job_status.owner_session_id must be a string";
                    return false;
                }
                if (!failure_value || !failure_value->is_str()) {
                    err = "protocol response job_status.failure_message must be a string";
                    return false;
                }
                out.has_job_status = true;
                out.job_owner_session_id = owner_value->as_str();
                out.job_failure_message = failure_value->as_str();
                if (!parse_job_state_name(state_value->as_str(), out.job_state, err)) {
                    return false;
                }
            }

            if (const auto* job_result_value = obj_get(object, "job_result"); job_result_value != nullptr) {
                if (!job_result_value->is_obj()) {
                    err = "protocol response job_result must be an object";
                    return false;
                }
                const auto& job_result_object = job_result_value->as_obj();
                const auto* owner_value = obj_get(job_result_object, "owner_session_id");
                const auto* metadata_value = obj_get(job_result_object, "metadata");
                if (!owner_value || !owner_value->is_str()) {
                    err = "protocol response job_result.owner_session_id must be a string";
                    return false;
                }
                if (!metadata_value || !metadata_value->is_obj()) {
                    err = "protocol response job_result.metadata must be an object";
                    return false;
                }
                out.has_job_result = true;
                out.result_owner_session_id = owner_value->as_str();
                out.result_has_primary_backtest = obj_get(metadata_value->as_obj(), "primary_backtest") != nullptr;
                if (const auto* sweep_value = obj_get(metadata_value->as_obj(), "sweep");
                    sweep_value != nullptr) {
                    if (!sweep_value->is_obj()) {
                        err = "protocol response job_result.metadata.sweep must be an object";
                        return false;
                    }
                    out.result_has_sweep_report = true;
                    if (const auto* top_candidates_value =
                            obj_get(sweep_value->as_obj(), "top_candidates");
                        top_candidates_value != nullptr) {
                        if (!top_candidates_value->is_arr()) {
                            err = "protocol response job_result.metadata.sweep.top_candidates must be an array";
                            return false;
                        }
                        out.result_sweep_top_candidate_count =
                            top_candidates_value->as_arr().size();
                    }
                }
            }

            err.clear();
            return true;
        }

        std::string single_series_layout_json() {
            return
                "{\n"
                "  \"ingest\": {\n"
                "    \"time_col\": \"row_id\",\n"
                "    \"time_kind\": \"Index\",\n"
                "    \"header_policy\": \"RequiredHeaders\",\n"
                "    \"value_cols\": [\"close\"],\n"
                "    \"primary_value_col\": \"close\",\n"
                "    \"missing_tokens\": [\"\", \"NA\", \"null\", \"nan\", \"NaN\"],\n"
                "    \"missing_default\": { \"kind\": \"NaN\" },\n"
                "    \"sort_by_time\": true,\n"
                "    \"dedupe\": \"Error\"\n"
                "  }\n"
                "}\n";
        }

        std::string single_series_sweep_logic_json(std::size_t max_evals) {
            std::ostringstream out;
            out
                << "{\n"
                << "  \"version\": 1,\n"
                << "  \"name\": \"service_protocol_live_sweep\",\n"
                << "  \"trade_mode\": \"LongOnly\",\n"
                << "  \"params\": {\n"
                << "    \"n_fast\": 2\n"
                << "  },\n"
                << "  \"numeric_nodes\": [\n"
                << "    { \"id\": \"c\", \"op\": \"core.close\" },\n"
                << "    { \"id\": \"sma\", \"op\": \"ts.sma\", \"args\": [\"c\"], \"params\": { \"n\": { \"ref\": \"n_fast\" } } },\n"
                << "    { \"id\": \"z\", \"op\": \"core.const\", \"params\": { \"value\": 0.0 } }\n"
                << "  ],\n"
                << "  \"bool_nodes\": [\n"
                << "    { \"id\": \"entry\", \"op\": \"cmp.gt\", \"args\": [\"sma\", \"z\"] },\n"
                << "    { \"id\": \"exit\", \"op\": \"cmp.lt\", \"args\": [\"sma\", \"z\"] }\n"
                << "  ],\n"
                << "  \"bindings\": {\n"
                << "    \"entry_long\": \"entry\",\n"
                << "    \"exit_long\": \"exit\",\n"
                << "    \"entry_short\": \"\",\n"
                << "    \"exit_short\": \"\",\n"
                << "    \"allow_trade\": \"\"\n"
                << "  },\n"
                << "  \"sweep\": {\n"
                << "    \"enabled\": true,\n"
                << "    \"engine\": \"exhaustive\",\n"
                << "    \"top_k\": 3,\n"
                << "    \"max_evals\": " << max_evals << ",\n"
                << "    \"params\": {\n"
                << "      \"n_fast\": {\n"
                << "        \"min\": 2,\n"
                << "        \"max\": " << (1 + max_evals) << ",\n"
                << "        \"step\": 1,\n"
                << "        \"is_int\": true\n"
                << "      }\n"
                << "    }\n"
                << "  }\n"
                << "}\n";
            return out.str();
        }

        std::string single_series_csv_text(std::size_t rows = 3) {
            std::ostringstream out;
            out << "row_id,close\n";
            for (std::size_t i = 0; i < rows; ++i) {
                out << (1 + i)
                    << ","
                    << (100.0 + static_cast<double>(i))
                    << "\n";
            }
            return out.str();
        }

        bool write_text_file(
            const std::filesystem::path& path,
            std::string_view content,
            std::string& err) {
            std::ofstream out(path, std::ios::binary);
            if (!out) {
                err = "failed to open contract fixture file: " + path.string();
                return false;
            }
            out << content;
            if (!out) {
                err = "failed to write contract fixture file: " + path.string();
                return false;
            }
            err.clear();
            return true;
        }

        bool reserve_loopback_port(std::uint16_t& out_port, std::string& err) {
            out_port = 0;

            const auto fd = to_native_socket_handle(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
            if (!is_valid_socket(fd)) {
                err = "failed to create probe socket";
                return false;
            }

            if (!bind_available_loopback_test_listener(fd, out_port, err)) {
                close_socket_quietly(fd);
                return false;
            }

            close_socket_quietly(fd);
            err.clear();
            return out_port != 0;
        }

        bool can_connect_loopback(std::uint16_t port) {
            const auto fd = to_native_socket_handle(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
            if (!is_valid_socket(fd)) {
                return false;
            }

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            if (::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
                close_socket_quietly(fd);
                return false;
            }

            const bool ok = ::connect(to_socket(fd), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
            close_socket_quietly(fd);
            return ok;
        }

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
            std::string& err) {
            const auto fd = to_native_socket_handle(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
            if (!is_valid_socket(fd)) {
                err = "failed to create client socket: " + socket_error_message(last_socket_error());
                return false;
            }

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            if (::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
                close_socket_quietly(fd);
                err = "failed to encode client loopback address";
                return false;
            }
            if (::connect(to_socket(fd), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
                const int error_code = last_socket_error();
                close_socket_quietly(fd);
                err = "failed to connect to live TCP server: " + socket_error_message(error_code);
                return false;
            }

            const std::string request_json = sugar::service::serialize_protocol_request_json(request);
            const std::string framed = sugar::service::frame_json_message(request_json);
            if (!write_all(fd, framed.data(), framed.size())) {
                close_socket_quietly(fd);
                err = "failed to send framed request";
                return false;
            }

            if (!read_framed_json(fd, kProtocolMessageLimit, out_response_json, err)) {
                close_socket_quietly(fd);
                return false;
            }

            close_socket_quietly(fd);
            if (!parse_protocol_response_json(out_response_json, out_response, err)) {
                return false;
            }

            err.clear();
            return true;
        }

        bool send_tcp_protocol_request(
            std::uint16_t port,
            const sugar::service::ProtocolRequest& request,
            ParsedProtocolResponse& out_response,
            std::string& err) {
            std::string ignored_response_json;
            return send_tcp_protocol_request(port, request, out_response, ignored_response_json, err);
        }

        bool open_connected_loopback_socket(
            std::uint16_t port,
            sugar::service::NativeSocketHandle& out_fd,
            std::string& err) {
            out_fd = to_native_socket_handle(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
            if (!is_valid_socket(out_fd)) {
                err = "failed to create raw client socket";
                return false;
            }

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            if (::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
                close_socket_quietly(out_fd);
                out_fd = kInvalidSocketHandle;
                err = "failed to encode client loopback address";
                return false;
            }
            if (::connect(to_socket(out_fd), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
                close_socket_quietly(out_fd);
                out_fd = kInvalidSocketHandle;
                err = "failed to connect raw client socket";
                return false;
            }

            err.clear();
            return true;
        }

        bool make_connected_socket_pair(
            sugar::service::NativeSocketHandle (&fds)[2],
            std::string& err) {
            fds[0] = kInvalidSocketHandle;
            fds[1] = kInvalidSocketHandle;

            const auto listener = to_native_socket_handle(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
            if (!is_valid_socket(listener)) {
                err = "failed to create loopback socket-pair listener: " + socket_error_message(last_socket_error());
                return false;
            }

            std::uint16_t bound_port = 0;
            if (!bind_available_loopback_test_listener(listener, bound_port, err)) {
                close_socket_quietly(listener);
                return false;
            }
            (void)bound_port;
            if (::listen(to_socket(listener), 1) != 0) {
                const int error_code = last_socket_error();
                close_socket_quietly(listener);
                err = "failed to listen on loopback socket-pair listener: " + socket_error_message(error_code);
                return false;
            }

            sockaddr_in bound_addr{};
#ifdef _WIN32
            int bound_size = sizeof(bound_addr);
#else
            socklen_t bound_size = sizeof(bound_addr);
#endif
            if (::getsockname(to_socket(listener), reinterpret_cast<sockaddr*>(&bound_addr), &bound_size) != 0) {
                const int error_code = last_socket_error();
                close_socket_quietly(listener);
                err = "failed to query loopback socket-pair listener: " + socket_error_message(error_code);
                return false;
            }

            fds[0] = to_native_socket_handle(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
            if (!is_valid_socket(fds[0])) {
                close_socket_quietly(listener);
                err = "failed to create loopback socket-pair client: " + socket_error_message(last_socket_error());
                return false;
            }
            if (::connect(to_socket(fds[0]), reinterpret_cast<sockaddr*>(&bound_addr), sizeof(bound_addr)) != 0) {
                const int error_code = last_socket_error();
                close_socket_quietly(fds[0]);
                close_socket_quietly(listener);
                fds[0] = kInvalidSocketHandle;
                err = "failed to connect loopback socket-pair client: " + socket_error_message(error_code);
                return false;
            }

            fds[1] = to_native_socket_handle(::accept(to_socket(listener), nullptr, nullptr));
            close_socket_quietly(listener);
            if (!is_valid_socket(fds[1])) {
                const int error_code = last_socket_error();
                close_socket_quietly(fds[0]);
                fds[0] = kInvalidSocketHandle;
                err = "failed to accept loopback socket-pair server: " + socket_error_message(error_code);
                return false;
            }

            err.clear();
            return true;
        }

        bool connect_session(std::uint16_t port, std::string& out_session_id, std::string& err) {
            sugar::service::ProtocolRequest request{};
            request.command = sugar::service::ProtocolCommand::Connect;

            ParsedProtocolResponse response{};
            if (!send_tcp_protocol_request(port, request, response, err)) {
                return false;
            }
            if (!response.ok || response.session_id.empty()) {
                err = response.message.empty() ? "connect response did not return a session id" : response.message;
                return false;
            }

            out_session_id = response.session_id;
            err.clear();
            return true;
        }

        bool upload_artifact(
            std::uint16_t port,
            const std::string& session_id,
            sugar::service::ArtifactKind kind,
            std::string_view client_name,
            std::string_view content,
            std::string& out_upload_id,
            std::string& err) {
            sugar::service::ProtocolRequest request{};
            request.command = sugar::service::ProtocolCommand::UploadArtifact;
            request.session_id = session_id;
            request.upload_kind = kind;
            request.upload_client_name = std::string(client_name);
            request.upload_content = std::string(content);

            ParsedProtocolResponse response{};
            if (!send_tcp_protocol_request(port, request, response, err)) {
                return false;
            }
            if (!response.ok || response.upload_id.empty()) {
                err = response.message.empty() ? "upload response did not return an upload id" : response.message;
                return false;
            }

            out_upload_id = response.upload_id;
            err.clear();
            return true;
        }

        bool submit_job(
            std::uint16_t port,
            const std::string& session_id,
            const std::string& layout_upload_id,
            const std::string& logic_upload_id,
            const std::string& data_upload_id,
            std::string& out_job_id,
            std::string& err) {
            sugar::service::ProtocolRequest request{};
            request.command = sugar::service::ProtocolCommand::SubmitJob;
            request.session_id = session_id;
            request.layout_upload_id = layout_upload_id;
            request.logic_upload_id = logic_upload_id;
            request.data_upload_id = data_upload_id;

            ParsedProtocolResponse response{};
            if (!send_tcp_protocol_request(port, request, response, err)) {
                return false;
            }
            if (!response.ok || response.job_id.empty()) {
                err = response.message.empty() ? "submit response did not return a job id" : response.message;
                return false;
            }

            out_job_id = response.job_id;
            err.clear();
            return true;
        }

        bool disconnect_session(std::uint16_t port, const std::string& session_id, std::string& err) {
            sugar::service::ProtocolRequest request{};
            request.command = sugar::service::ProtocolCommand::Disconnect;
            request.session_id = session_id;

            ParsedProtocolResponse response{};
            if (!send_tcp_protocol_request(port, request, response, err)) {
                return false;
            }
            if (!response.ok) {
                err = response.message;
                return false;
            }

            err.clear();
            return true;
        }

        bool wait_for_terminal_status_over_tcp(
            std::uint16_t port,
            const std::string& session_id,
            const std::string& job_id,
            sugar::service::JobState& out_state,
            bool& out_saw_active_state,
            std::string& out_failure_message,
            std::size_t& out_poll_count,
            std::string& err) {
            using sugar::service::JobState;

            const auto deadline = std::chrono::steady_clock::now() + kTcpTerminalWaitTimeout;
            while (std::chrono::steady_clock::now() < deadline) {
                sugar::service::ProtocolRequest request{};
                request.command = sugar::service::ProtocolCommand::CheckStatus;
                request.session_id = session_id;
                request.job_id = job_id;

                ParsedProtocolResponse response{};
                if (!send_tcp_protocol_request(port, request, response, err)) {
                    return false;
                }
                if (!response.ok || !response.has_job_status) {
                    err = response.message.empty() ? "status polling did not return a job_status payload" : response.message;
                    return false;
                }

                ++out_poll_count;
                out_state = response.job_state;
                out_failure_message = response.job_failure_message;
                if (response.job_state == JobState::Validating ||
                    response.job_state == JobState::Running ||
                    response.job_state == JobState::CancelRequested) {
                    out_saw_active_state = true;
                }
                if (response.job_state == JobState::Completed ||
                    response.job_state == JobState::Rejected ||
                    response.job_state == JobState::Failed ||
                    response.job_state == JobState::Canceled) {
                    err.clear();
                    return true;
                }

                std::this_thread::sleep_for(kTcpPollInterval);
            }

            err = "timed out waiting for terminal job state over TCP";
            return false;
        }

} // namespace sugar::contract_suite_internal::service_protocol_contract

