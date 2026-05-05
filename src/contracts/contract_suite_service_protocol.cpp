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

namespace sugar::contract_suite_internal {

    namespace {
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

    } // namespace

    bool test_service_protocol_network_socket_contracts(ContractAsserts& t) {
        t.out << "[service_protocol_network_socket] socket-level protocol transport behavior\n";

        std::string socket_runtime_err;
        ScopedSocketRuntime socket_runtime(socket_runtime_err);
        t.expect(socket_runtime.active(), "socket runtime initializes for protocol socket contracts");
        if (!socket_runtime.active()) {
            t.out << "  socket runtime error: " << socket_runtime_err << "\n";
            return true;
        }

        sugar::service::ProtocolDispatcherConfig config{};
        config.runtime.storage_root = contract_tmp_case_dir("service_protocol_network_socket");
        config.source_root = std::filesystem::current_path();
        sugar::service::ProtocolDispatcher dispatcher(config);

        {
            sugar::service::NativeSocketHandle fds[2]{ kInvalidSocketHandle, kInvalidSocketHandle };
            std::string err;
            if (!make_connected_socket_pair(fds, err)) {
                return t.expect(false, "loopback socket pair creates transport fixture");
            }

            sugar::service::ProtocolRequest request{};
            request.command = sugar::service::ProtocolCommand::Connect;
            const std::string request_json = sugar::service::serialize_protocol_request_json(request);
            const std::string framed = sugar::service::frame_json_message(request_json);
            t.expect(write_all(fds[0], framed.data(), framed.size()),
                "socket fixture writes valid framed connect request");

            const bool handled = sugar::service::handle_protocol_socket_connection(
                fds[1], dispatcher, kSocketContractFrameLimit, sugar::service::SecurityLogFn{}, err);
            t.expect(handled, "network session handles valid framed request");

            std::string response_json;
            const bool read_ok = read_framed_json(fds[0], kSocketContractFrameLimit, response_json, err);
            t.expect(read_ok, "network session writes framed response for valid request");

            ParsedProtocolResponse response{};
            const bool parsed = read_ok && parse_protocol_response_json(response_json, response, err);
            t.expect(parsed, "network session response JSON parses after valid request");
            t.expect(parsed && response.ok, "network session reports ok=true for valid request");
            t.expect(parsed && response.error_code == sugar::service::ProtocolErrorCode::None,
                "network session reports error_code=none for valid request");

            close_socket_quietly(fds[0]);
            close_socket_quietly(fds[1]);
        }

        {
            sugar::service::NativeSocketHandle fds[2]{ kInvalidSocketHandle, kInvalidSocketHandle };
            std::string err;
            if (!make_connected_socket_pair(fds, err)) {
                return t.expect(false, "loopback socket pair recreates transport fixture for truncated frame");
            }

            sugar::service::ProtocolRequest request{};
            request.command = sugar::service::ProtocolCommand::Connect;
            std::string framed = sugar::service::frame_json_message(
                sugar::service::serialize_protocol_request_json(request));
            framed.pop_back();
            t.expect(write_all(fds[0], framed.data(), framed.size()),
                "socket fixture writes truncated framed request bytes");
            shutdown_socket_write(fds[0]);

            const bool handled = sugar::service::handle_protocol_socket_connection(
                fds[1], dispatcher, kSocketContractFrameLimit, sugar::service::SecurityLogFn{}, err);
            t.expect(handled, "network session handles truncated request without crashing server loop");

            std::string response_json;
            const bool read_ok = read_framed_json(fds[0], kSocketContractFrameLimit, response_json, err);
            t.expect(read_ok, "network session writes framed error response for truncated request");

            ParsedProtocolResponse response{};
            const bool parsed = read_ok && parse_protocol_response_json(response_json, response, err);
            t.expect(parsed, "network session error response JSON parses after truncated request");
            t.expect(parsed && !response.ok, "network session reports ok=false for truncated request");
            t.expect(parsed && response.error_code == sugar::service::ProtocolErrorCode::MalformedRequest,
                "network session classifies truncated frame as malformed_request");
            t.expect(parsed && response.message.find("frame body") != std::string::npos,
                "network session preserves truncated-body detail in malformed response");

            close_socket_quietly(fds[0]);
            close_socket_quietly(fds[1]);
        }

        {
            sugar::service::NativeSocketHandle fds[2]{ kInvalidSocketHandle, kInvalidSocketHandle };
            std::string err;
            if (!make_connected_socket_pair(fds, err)) {
                return t.expect(false, "loopback socket pair recreates transport fixture for zero-length frame");
            }

            const std::string zero_length_header(4, '\0');
            t.expect(write_all(fds[0], zero_length_header.data(), zero_length_header.size()),
                "socket fixture writes zero-length frame header bytes");
            shutdown_socket_write(fds[0]);

            const bool handled = sugar::service::handle_protocol_socket_connection(
                fds[1], dispatcher, kSocketContractFrameLimit, sugar::service::SecurityLogFn{}, err);
            t.expect(handled, "network session handles zero-length frame without crashing server loop");

            std::string response_json;
            const bool read_ok = read_framed_json(fds[0], kSocketContractFrameLimit, response_json, err);
            t.expect(read_ok, "network session writes framed error response for zero-length frame");

            ParsedProtocolResponse response{};
            const bool parsed = read_ok && parse_protocol_response_json(response_json, response, err);
            t.expect(parsed, "network session error response JSON parses after zero-length frame");
            t.expect(parsed && !response.ok, "network session reports ok=false for zero-length frame");
            t.expect(parsed && response.error_code == sugar::service::ProtocolErrorCode::MalformedRequest,
                "network session classifies zero-length frame as malformed_request");
            t.expect(parsed && response.message.find("frame length is invalid") != std::string::npos,
                "network session preserves zero-length-frame detail in malformed response");

            close_socket_quietly(fds[0]);
            close_socket_quietly(fds[1]);
        }

        {
            sugar::service::NativeSocketHandle fds[2]{ kInvalidSocketHandle, kInvalidSocketHandle };
            std::string err;
            if (!make_connected_socket_pair(fds, err)) {
                return t.expect(false, "loopback socket pair recreates transport fixture for oversized frame header");
            }

            const std::string oversized_header(4, static_cast<char>(0xFF));
            t.expect(write_all(fds[0], oversized_header.data(), oversized_header.size()),
                "socket fixture writes oversized frame header bytes");
            shutdown_socket_write(fds[0]);

            const bool handled = sugar::service::handle_protocol_socket_connection(
                fds[1], dispatcher, kSocketContractFrameLimit, sugar::service::SecurityLogFn{}, err);
            t.expect(handled, "network session handles oversized frame header without crashing server loop");

            std::string response_json;
            const bool read_ok = read_framed_json(fds[0], kSocketContractFrameLimit, response_json, err);
            t.expect(read_ok, "network session writes framed error response for oversized frame header");

            ParsedProtocolResponse response{};
            const bool parsed = read_ok && parse_protocol_response_json(response_json, response, err);
            t.expect(parsed, "network session error response JSON parses after oversized frame header");
            t.expect(parsed && !response.ok, "network session reports ok=false for oversized frame header");
            t.expect(parsed && response.error_code == sugar::service::ProtocolErrorCode::MalformedRequest,
                "network session classifies oversized frame header as malformed_request");
            t.expect(parsed && response.message.find("frame length is invalid") != std::string::npos,
                "network session preserves oversized-frame detail in malformed response");

            close_socket_quietly(fds[0]);
            close_socket_quietly(fds[1]);
        }

        {
            sugar::service::NativeSocketHandle fds[2]{ kInvalidSocketHandle, kInvalidSocketHandle };
            std::string err;
            if (!make_connected_socket_pair(fds, err)) {
                return t.expect(false, "loopback socket pair recreates transport fixture for partial frame header");
            }

            std::string partial_header;
            partial_header.push_back('\0');
            partial_header.push_back('\x10');
            t.expect(write_all(fds[0], partial_header.data(), partial_header.size()),
                "socket fixture writes partial frame header bytes");
            shutdown_socket_write(fds[0]);

            const bool handled = sugar::service::handle_protocol_socket_connection(
                fds[1], dispatcher, kSocketContractFrameLimit, sugar::service::SecurityLogFn{}, err);
            t.expect(handled, "network session handles partial frame header without crashing server loop");

            std::string response_json;
            const bool read_ok = read_framed_json(fds[0], kSocketContractFrameLimit, response_json, err);
            t.expect(read_ok, "network session writes framed error response for partial frame header");

            ParsedProtocolResponse response{};
            const bool parsed = read_ok && parse_protocol_response_json(response_json, response, err);
            t.expect(parsed, "network session error response JSON parses after partial frame header");
            t.expect(parsed && !response.ok, "network session reports ok=false for partial frame header");
            t.expect(parsed && response.error_code == sugar::service::ProtocolErrorCode::MalformedRequest,
                "network session classifies partial frame header as malformed_request");
            t.expect(parsed && response.message.find("frame header") != std::string::npos,
                "network session preserves partial-header detail in malformed response");

            close_socket_quietly(fds[0]);
            close_socket_quietly(fds[1]);
        }

        return true;
    }

    bool test_service_protocol_live_client_run_contracts(ContractAsserts& t) {
        t.out << "[service_protocol_live_client_run] live TCP one-command client run\n";

        std::string socket_runtime_err;
        ScopedSocketRuntime socket_runtime(socket_runtime_err);
        t.expect(socket_runtime.active(), "socket runtime initializes for live client run contract");
        if (!socket_runtime.active()) {
            t.out << "  socket runtime error: " << socket_runtime_err << "\n";
            return true;
        }

        const auto case_dir = contract_tmp_case_dir("service_protocol_live_client_run");
        const auto fixture_dir = case_dir / "fixtures";
        std::filesystem::create_directories(fixture_dir);
        const auto layout_path = fixture_dir / "layout.json";
        const auto logic_path = fixture_dir / "logic.json";
        const auto data_path = fixture_dir / "data.csv";

        std::string err;
        const bool fixtures_written =
            write_text_file(layout_path, single_series_layout_json(), err) &&
            write_text_file(logic_path, single_series_sweep_logic_json(8), err) &&
            write_text_file(data_path, single_series_csv_text(32), err);
        t.expect(fixtures_written, "live client run contract writes layout, logic, and data fixtures");
        if (!fixtures_written) {
            t.out << "  fixture error: " << err << "\n";
            return true;
        }

        ScopedLiveTcpServer server{};
        sugar::service::ServerCliOptions options{};
        options.storage_root = case_dir / "storage";
        options.source_root = std::filesystem::current_path();
        options.worker_limit = 2;
        options.connection_worker_limit = 2;
        options.max_pending_connections = 8;

        const bool started = server.start(options, err);
        t.expect(started, "live TCP server starts for one-command client run");
        if (!started) {
            t.out << "  server start error: " << err << "\n";
            return true;
        }

        std::vector<std::string> args{
            "sugar_client",
            "run",
            "--host",
            "127.0.0.1",
            "--port",
            std::to_string(server.port()),
            "--layout",
            layout_path.string(),
            "--logic",
            logic_path.string(),
            "--data",
            data_path.string(),
            "--poll-ms",
            "25",
            "--wait-timeout-ms",
            "30000",
            "--compact",
        };
        std::vector<char*> argv;
        argv.reserve(args.size());
        for (auto& arg : args) {
            argv.push_back(arg.data());
        }

        std::ostringstream client_out;
        std::ostringstream client_err;
        const int rc = sugar::service::run_tcp_client_from_cli(
            static_cast<int>(argv.size()),
            argv.data(),
            client_out,
            client_err);
        t.expect(rc == 0, "sugar_client run completes a hosted layout/logic/data job over live TCP");
        t.expect(client_err.str().empty(), "sugar_client run --compact keeps progress chatter off stderr on success");

        ParsedProtocolResponse result_response{};
        std::string parse_err;
        const bool parsed = parse_protocol_response_json(client_out.str(), result_response, parse_err);
        t.expect(parsed, "sugar_client run prints parseable final hosted result JSON");
        if (!parsed) {
            t.out << "  client parse error: " << parse_err << "\n";
            t.out << "  client stdout:\n" << client_out.str() << "\n";
            t.out << "  client stderr:\n" << client_err.str() << "\n";
        }
        t.expect(
            parsed &&
                result_response.ok &&
                result_response.command == sugar::service::ProtocolCommand::RetrieveResults &&
                result_response.has_job_result,
            "sugar_client run returns the final retrieve_results payload instead of intermediate ids");
        t.expect(
            parsed &&
                result_response.result_has_sweep_report &&
                result_response.result_sweep_top_candidate_count > 0,
            "sugar_client run preserves hosted sweep report metadata in the final result");

        std::vector<std::string> human_args = args;
        human_args.pop_back();
        std::vector<char*> human_argv;
        human_argv.reserve(human_args.size());
        for (auto& arg : human_args) {
            human_argv.push_back(arg.data());
        }

        std::ostringstream human_out;
        std::ostringstream human_err;
        const int human_rc = sugar::service::run_tcp_client_from_cli(
            static_cast<int>(human_argv.size()),
            human_argv.data(),
            human_out,
            human_err);
        t.expect(human_rc == 0, "sugar_client run completes in default human-output mode");
        t.expect(
            human_out.str().find("Execution completed") != std::string::npos &&
                human_out.str().find("PlanSpec Sweep Results") != std::string::npos,
            "sugar_client run default output prints the hosted summary text");
        t.expect(
            human_out.str().find("\"job_result\"") == std::string::npos,
            "sugar_client run default output avoids dumping raw hosted JSON");
        t.expect(
            human_err.str().find("Connected session") != std::string::npos &&
                human_err.str().find("Submitted job") != std::string::npos,
            "sugar_client run default output keeps progress chatter on stderr");

        const auto timeout_logic_path = fixture_dir / "timeout_logic.json";
        const auto timeout_data_path = fixture_dir / "timeout_data.csv";
        const bool timeout_fixtures_written =
            write_text_file(timeout_logic_path, single_series_sweep_logic_json(10000), err) &&
            write_text_file(timeout_data_path, single_series_csv_text(256), err);
        t.expect(timeout_fixtures_written, "live client run timeout contract writes long-running fixtures");
        if (!timeout_fixtures_written) {
            t.out << "  timeout fixture error: " << err << "\n";
        }

        if (timeout_fixtures_written) {
            std::vector<std::string> timeout_args{
                "sugar_client",
                "run",
                "--host",
                "127.0.0.1",
                "--port",
                std::to_string(server.port()),
                "--layout",
                layout_path.string(),
                "--logic",
                timeout_logic_path.string(),
                "--data",
                timeout_data_path.string(),
                "--poll-ms",
                "1",
                "--wait-timeout-ms",
                "1",
                "--compact",
            };
            std::vector<char*> timeout_argv;
            timeout_argv.reserve(timeout_args.size());
            for (auto& arg : timeout_args) {
                timeout_argv.push_back(arg.data());
            }

            std::ostringstream timeout_out;
            std::ostringstream timeout_err;
            const int timeout_rc = sugar::service::run_tcp_client_from_cli(
                static_cast<int>(timeout_argv.size()),
                timeout_argv.data(),
                timeout_out,
                timeout_err);
            t.expect(timeout_rc == 1, "sugar_client run returns failure after wait-timeout expiry");
            t.expect(
                timeout_err.str().find("reuse the same session id") != std::string::npos &&
                    timeout_err.str().find("Creating a new session will not have access to this job.") != std::string::npos,
                "sugar_client run timeout guidance tells operators to reuse the same session id");

            ParsedProtocolResponse timeout_status{};
            std::string timeout_parse_err;
            const bool timeout_status_parsed = parse_protocol_response_json(
                timeout_out.str(),
                timeout_status,
                timeout_parse_err);
            t.expect(timeout_status_parsed, "sugar_client run timeout prints parseable latest status JSON");
            if (timeout_status_parsed) {
                t.expect(
                    timeout_status.command == sugar::service::ProtocolCommand::CheckStatus &&
                        timeout_status.has_job_status &&
                        !timeout_status.session_id.empty() &&
                        !timeout_status.job_id.empty(),
                    "sugar_client run timeout preserves the original session and job ids in the latest status payload");

                sugar::service::JobState terminal_state{};
                bool saw_active_state = false;
                std::string failure_message;
                std::size_t poll_count = 0;
                std::string wait_err;
                const bool reached_terminal = wait_for_terminal_status_over_tcp(
                    server.port(),
                    timeout_status.session_id,
                    timeout_status.job_id,
                    terminal_state,
                    saw_active_state,
                    failure_message,
                    poll_count,
                    wait_err);
                t.expect(reached_terminal, "same-session polling can resume after sugar_client run times out");
                if (reached_terminal) {
                    ParsedProtocolResponse resumed_result{};
                    std::string resumed_result_json;
                    std::string resumed_err;
                    sugar::service::ProtocolRequest resumed_request{};
                    resumed_request.command = sugar::service::ProtocolCommand::RetrieveResults;
                    resumed_request.session_id = timeout_status.session_id;
                    resumed_request.job_id = timeout_status.job_id;
                    const bool resumed_ok = send_tcp_protocol_request(
                        server.port(),
                        resumed_request,
                        resumed_result,
                        resumed_result_json,
                        resumed_err);
                    t.expect(
                        resumed_ok &&
                            resumed_result.ok &&
                            resumed_result.command == sugar::service::ProtocolCommand::RetrieveResults,
                        "same-session result retrieval succeeds after sugar_client run timeout guidance");
                }
            }
        }

        server.stop();
        t.expect(server.exit_code() == 0, "live client run server stops cleanly after one-command run");
        if (server.exit_code() != 0 || rc != 0) {
            t.out << "  server stderr:\n" << server.stderr_text();
            t.out << "  server stdout:\n" << server.stdout_text();
            t.out << "  client stderr:\n" << client_err.str();
            t.out << "  client stdout:\n" << client_out.str();
        }

        return true;
    }

    bool test_service_protocol_live_multi_client_contracts(ContractAsserts& t) {
        t.out << "[service_protocol_live_multi_client] live TCP multi-client submit/status/result concurrency\n";

        std::string socket_runtime_err;
        ScopedSocketRuntime socket_runtime(socket_runtime_err);
        t.expect(socket_runtime.active(), "socket runtime initializes for live multi-client contracts");
        if (!socket_runtime.active()) {
            t.out << "  socket runtime error: " << socket_runtime_err << "\n";
            return true;
        }

        ScopedLiveTcpServer server{};
        sugar::service::ServerCliOptions options{};
        options.storage_root = contract_tmp_case_dir("service_protocol_live_multi_client");
        options.source_root = std::filesystem::current_path();
        options.worker_limit = 3;
        options.max_live_sessions = 16;
        options.connection_worker_limit = 4;
        options.max_pending_connections = 16;
        options.max_active_jobs = 32;

        std::string err;
        const bool started = server.start(options, err);
        t.expect(started, "live TCP server starts for multi-client protocol test");
        if (!started) {
            return true;
        }

        constexpr std::size_t kClientCount = 8;
        struct ClientRun {
            std::string session_id{};
            std::string job_id{};
            std::string error{};
            bool saw_active_state{ false };
            std::size_t status_polls{ 0 };
            ParsedProtocolResponse result_response{};
        };

        std::vector<ClientRun> runs(kClientCount);
        const std::string layout_json = single_series_layout_json();
        const std::string logic_json = single_series_sweep_logic_json(768);
        const std::string data_csv = single_series_csv_text(96);
        std::vector<std::thread> clients;
        clients.reserve(kClientCount);
        for (std::size_t i = 0; i < kClientCount; ++i) {
            clients.emplace_back([&, i] {
                std::string local_err;
                std::string layout_upload_id;
                std::string logic_upload_id;
                std::string data_upload_id;

                if (!connect_session(server.port(), runs[i].session_id, local_err) ||
                    !upload_artifact(
                        server.port(),
                        runs[i].session_id,
                        sugar::service::ArtifactKind::LayoutSpec,
                        "layout.json",
                        layout_json,
                        layout_upload_id,
                        local_err) ||
                    !upload_artifact(
                        server.port(),
                        runs[i].session_id,
                        sugar::service::ArtifactKind::LogicSpec,
                        "logic.json",
                        logic_json,
                        logic_upload_id,
                        local_err) ||
                    !upload_artifact(
                        server.port(),
                        runs[i].session_id,
                        sugar::service::ArtifactKind::DataCsv,
                        "data.csv",
                        data_csv,
                        data_upload_id,
                        local_err) ||
                    !submit_job(
                        server.port(),
                        runs[i].session_id,
                        layout_upload_id,
                        logic_upload_id,
                        data_upload_id,
                        runs[i].job_id,
                        local_err)) {
                    runs[i].error = local_err;
                    return;
                }

                sugar::service::JobState terminal_state = sugar::service::JobState::Queued;
                std::string terminal_failure;
                if (!wait_for_terminal_status_over_tcp(
                        server.port(),
                        runs[i].session_id,
                        runs[i].job_id,
                        terminal_state,
                        runs[i].saw_active_state,
                        terminal_failure,
                        runs[i].status_polls,
                        local_err)) {
                    runs[i].error = local_err;
                    return;
                }
                if (terminal_state != sugar::service::JobState::Completed) {
                    runs[i].error =
                        "job reached unexpected terminal state: " +
                        std::string(sugar::service::job_state_name(terminal_state));
                    if (!terminal_failure.empty()) {
                        runs[i].error += " | " + terminal_failure;
                    }
                    return;
                }

                sugar::service::ProtocolRequest result_request{};
                result_request.command = sugar::service::ProtocolCommand::RetrieveResults;
                result_request.session_id = runs[i].session_id;
                result_request.job_id = runs[i].job_id;
                if (!send_tcp_protocol_request(server.port(), result_request, runs[i].result_response, local_err)) {
                    runs[i].error = local_err;
                    return;
                }
                if (!runs[i].result_response.ok ||
                    !runs[i].result_response.has_job_result ||
                    runs[i].result_response.result_owner_session_id != runs[i].session_id ||
                    !runs[i].result_response.result_has_primary_backtest ||
                    !runs[i].result_response.result_has_sweep_report ||
                    runs[i].result_response.result_sweep_top_candidate_count == 0) {
                    runs[i].error = runs[i].result_response.message.empty()
                        ? "result response did not preserve owned hosted sweep metadata"
                        : runs[i].result_response.message;
                    return;
                }

                if (!disconnect_session(server.port(), runs[i].session_id, local_err)) {
                    runs[i].error = local_err;
                }
            });
        }
        for (auto& client : clients) {
            client.join();
        }

        bool all_clients_ok = true;
        std::set<std::string> session_ids;
        std::set<std::string> job_ids;
        bool saw_active_state = false;
        for (const auto& run : runs) {
            all_clients_ok = all_clients_ok &&
                run.error.empty() &&
                !run.session_id.empty() &&
                !run.job_id.empty() &&
                run.result_response.has_job_result;
            if (!run.session_id.empty()) {
                session_ids.insert(run.session_id);
            }
            if (!run.job_id.empty()) {
                job_ids.insert(run.job_id);
            }
            saw_active_state = saw_active_state || run.saw_active_state;
        }
        t.expect(all_clients_ok, "live TCP multi-client workers submit, observe, and retrieve results without per-client failures");
        if (!all_clients_ok) {
            t.out << "  server exit code while diagnosing multi-client failure: " << server.exit_code() << "\n";
            const std::string server_stderr = server.stderr_text();
            const std::string server_stdout = server.stdout_text();
            if (!server_stderr.empty()) {
                t.out << "  server stderr:\n" << server_stderr;
            }
            if (!server_stdout.empty()) {
                t.out << "  server stdout:\n" << server_stdout;
            }
            for (std::size_t i = 0; i < runs.size(); ++i) {
                const auto& run = runs[i];
                if (!run.error.empty()) {
                    t.out << "  client " << (i + 1) << " error: " << run.error << "\n";
                }
            }
        }
        t.expect(session_ids.size() == kClientCount, "live TCP multi-client run assigns distinct session ids");
        t.expect(job_ids.size() == kClientCount, "live TCP multi-client run assigns distinct job ids");
        t.expect(saw_active_state, "live TCP multi-client polling observes queued/running progress while jobs are in flight");

        if (runs.size() >= 2 && !runs[0].job_id.empty() && !runs[1].session_id.empty()) {
            sugar::service::ProtocolRequest foreign_result_request{};
            foreign_result_request.command = sugar::service::ProtocolCommand::RetrieveResults;
            foreign_result_request.session_id = runs[1].session_id;
            foreign_result_request.job_id = runs[0].job_id;

            ParsedProtocolResponse foreign_result_response{};
            err.clear();
            const bool foreign_replied =
                send_tcp_protocol_request(server.port(), foreign_result_request, foreign_result_response, err);
            t.expect(foreign_replied, "live TCP multi-client run returns a structured reply for foreign result retrieval");
            t.expect(
                foreign_replied &&
                    !foreign_result_response.ok &&
                    foreign_result_response.error_code == sugar::service::ProtocolErrorCode::UnauthorizedRequest,
                "live TCP multi-client run rejects foreign result retrieval without cross-talk");
        }

        server.stop();
        t.expect(server.exit_code() == 0, "live TCP multi-client server stops cleanly after concurrent traffic");
        if (server.exit_code() != 0) {
            t.out << "  server stderr:\n" << server.stderr_text();
        }

        return true;
    }

    bool test_service_protocol_live_cancel_polling_contracts(ContractAsserts& t) {
        t.out << "[service_protocol_live_cancel_polling] live TCP repeated status polling and cancellation\n";

        std::string socket_runtime_err;
        ScopedSocketRuntime socket_runtime(socket_runtime_err);
        t.expect(socket_runtime.active(), "socket runtime initializes for live cancel-polling contracts");
        if (!socket_runtime.active()) {
            t.out << "  socket runtime error: " << socket_runtime_err << "\n";
            return true;
        }

        ScopedLiveTcpServer server{};
        sugar::service::ServerCliOptions options{};
        options.storage_root = contract_tmp_case_dir("service_protocol_live_cancel_polling");
        options.source_root = std::filesystem::current_path();
        options.worker_limit = 1;
        options.connection_worker_limit = 4;
        options.max_pending_connections = 16;
        options.max_active_jobs = 4;

        std::string err;
        const bool started = server.start(options, err);
        t.expect(started, "live TCP server starts for cancel-polling protocol test");
        if (!started) {
            return true;
        }

        std::string owner_session_id;
        std::string intruder_session_id;
        std::string layout_upload_id;
        std::string logic_upload_id;
        std::string data_upload_id;
        std::string job_id;
        const bool staged =
            connect_session(server.port(), owner_session_id, err) &&
            connect_session(server.port(), intruder_session_id, err) &&
            upload_artifact(
                server.port(),
                owner_session_id,
                sugar::service::ArtifactKind::LayoutSpec,
                "layout.json",
                single_series_layout_json(),
                layout_upload_id,
                err) &&
            upload_artifact(
                server.port(),
                owner_session_id,
                sugar::service::ArtifactKind::LogicSpec,
                "logic.json",
                single_series_sweep_logic_json(10000),
                logic_upload_id,
                err) &&
            upload_artifact(
                server.port(),
                owner_session_id,
                sugar::service::ArtifactKind::DataCsv,
                "data.csv",
                single_series_csv_text(256),
                data_upload_id,
                err) &&
            submit_job(
                server.port(),
                owner_session_id,
                layout_upload_id,
                logic_upload_id,
                data_upload_id,
                job_id,
                err);
        t.expect(staged, "live TCP cancel-polling path stages owner/intruder sessions and submits a long-running job");
        if (!staged) {
            server.stop();
            return true;
        }

        sugar::service::ProtocolRequest foreign_status_request{};
        foreign_status_request.command = sugar::service::ProtocolCommand::CheckStatus;
        foreign_status_request.session_id = intruder_session_id;
        foreign_status_request.job_id = job_id;
        ParsedProtocolResponse foreign_status_response{};
        err.clear();
        const bool foreign_status_replied =
            send_tcp_protocol_request(server.port(), foreign_status_request, foreign_status_response, err);
        t.expect(foreign_status_replied, "live TCP cancel-polling path returns a structured reply for foreign status access");
        t.expect(
            foreign_status_replied &&
                !foreign_status_response.ok &&
                foreign_status_response.error_code == sugar::service::ProtocolErrorCode::UnauthorizedRequest,
            "live TCP cancel-polling path rejects foreign status polling without leaking state");

        std::atomic_bool saw_active_state{ false };
        std::atomic_bool stop_pollers{ false };
        std::mutex poll_mutex{};
        std::vector<sugar::service::JobState> polled_states;
        std::vector<std::string> poll_errors;
        std::size_t result_poll_count = 0;
        std::vector<std::string> result_poll_errors;
        constexpr std::size_t kPollerCount = 3;
        std::vector<std::thread> pollers;
        pollers.reserve(kPollerCount);
        for (std::size_t i = 0; i < kPollerCount; ++i) {
            pollers.emplace_back([&, i] {
                using namespace std::chrono_literals;

                const auto deadline = std::chrono::steady_clock::now() + kTcpCancelPollerTimeout;
                while (!stop_pollers.load(std::memory_order_relaxed) &&
                    std::chrono::steady_clock::now() < deadline) {
                    sugar::service::ProtocolRequest status_request{};
                    status_request.command = sugar::service::ProtocolCommand::CheckStatus;
                    status_request.session_id = owner_session_id;
                    status_request.job_id = job_id;

                    ParsedProtocolResponse status_response{};
                    std::string local_err;
                    if (!send_tcp_protocol_request(server.port(), status_request, status_response, local_err)) {
                        std::lock_guard lock(poll_mutex);
                        poll_errors.push_back("poller " + std::to_string(i + 1) + ": " + local_err);
                        stop_pollers.store(true, std::memory_order_relaxed);
                        return;
                    }
                    if (!status_response.ok || !status_response.has_job_status) {
                        std::lock_guard lock(poll_mutex);
                        poll_errors.push_back(
                            "poller " + std::to_string(i + 1) + ": " +
                            (status_response.message.empty() ? "missing status payload" : status_response.message));
                        stop_pollers.store(true, std::memory_order_relaxed);
                        return;
                    }

                    {
                        std::lock_guard lock(poll_mutex);
                        polled_states.push_back(status_response.job_state);
                    }
                    if (status_response.job_state == sugar::service::JobState::Validating ||
                        status_response.job_state == sugar::service::JobState::Running ||
                        status_response.job_state == sugar::service::JobState::CancelRequested) {
                        saw_active_state.store(true, std::memory_order_relaxed);
                    }
                    if (status_response.job_state == sugar::service::JobState::Completed ||
                        status_response.job_state == sugar::service::JobState::Rejected ||
                        status_response.job_state == sugar::service::JobState::Failed ||
                        status_response.job_state == sugar::service::JobState::Canceled) {
                        stop_pollers.store(true, std::memory_order_relaxed);
                        return;
                    }

                    std::this_thread::sleep_for(kTcpPollInterval);
                }
            });
        }
        constexpr std::size_t kResultPollerCount = 2;
        std::vector<std::thread> result_pollers;
        result_pollers.reserve(kResultPollerCount);
        for (std::size_t i = 0; i < kResultPollerCount; ++i) {
            result_pollers.emplace_back([&, i] {
                using namespace std::chrono_literals;

                const auto deadline = std::chrono::steady_clock::now() + kTcpCancelPollerTimeout;
                while (!stop_pollers.load(std::memory_order_relaxed) &&
                    std::chrono::steady_clock::now() < deadline) {
                    sugar::service::ProtocolRequest result_request{};
                    result_request.command = sugar::service::ProtocolCommand::RetrieveResults;
                    result_request.session_id = owner_session_id;
                    result_request.job_id = job_id;

                    ParsedProtocolResponse result_response{};
                    std::string local_err;
                    if (!send_tcp_protocol_request(server.port(), result_request, result_response, local_err)) {
                        std::lock_guard lock(poll_mutex);
                        result_poll_errors.push_back("result poller " + std::to_string(i + 1) + ": " + local_err);
                        stop_pollers.store(true, std::memory_order_relaxed);
                        return;
                    }

                    {
                        std::lock_guard lock(poll_mutex);
                        ++result_poll_count;
                    }

                    if (result_response.ok ||
                        result_response.error_code != sugar::service::ProtocolErrorCode::InvalidState) {
                        std::lock_guard lock(poll_mutex);
                        result_poll_errors.push_back(
                            "result poller " + std::to_string(i + 1) + ": unexpected result response");
                        stop_pollers.store(true, std::memory_order_relaxed);
                        return;
                    }

                    std::this_thread::sleep_for(kTcpPollInterval);
                }
            });
        }

        const auto active_deadline = std::chrono::steady_clock::now() + kTcpCancelActiveWaitTimeout;
        while (!saw_active_state.load(std::memory_order_relaxed) &&
            !stop_pollers.load(std::memory_order_relaxed) &&
            std::chrono::steady_clock::now() < active_deadline) {
            std::this_thread::sleep_for(kTcpWaitInterval);
        }
        t.expect(saw_active_state.load(std::memory_order_relaxed),
            "live TCP cancel-polling path exposes validating/running state before cancellation");

        sugar::service::ProtocolRequest cancel_request{};
        cancel_request.command = sugar::service::ProtocolCommand::CancelJob;
        cancel_request.session_id = owner_session_id;
        cancel_request.job_id = job_id;

        ParsedProtocolResponse cancel_response{};
        err.clear();
        const bool cancel_replied = send_tcp_protocol_request(server.port(), cancel_request, cancel_response, err);
        t.expect(cancel_replied, "live TCP cancel-polling path returns a structured reply for cancel");
        t.expect(cancel_replied && cancel_response.ok, "live TCP cancel-polling path accepts owner cancellation");

        const auto cancel_deadline = std::chrono::steady_clock::now() + kTcpCancelPollerTimeout;
        while (!stop_pollers.load(std::memory_order_relaxed) &&
            std::chrono::steady_clock::now() < cancel_deadline) {
            std::this_thread::sleep_for(kTcpWaitInterval);
        }
        stop_pollers.store(true, std::memory_order_relaxed);
        for (auto& poller : pollers) {
            poller.join();
        }
        for (auto& poller : result_pollers) {
            poller.join();
        }

        bool pollers_clean = true;
        {
            std::lock_guard lock(poll_mutex);
            pollers_clean = poll_errors.empty() && result_poll_errors.empty();
        }
        t.expect(pollers_clean, "live TCP cancel-polling path keeps repeated status and result polling stable under cancellation");
        if (!pollers_clean) {
            std::lock_guard lock(poll_mutex);
            for (const auto& poll_error : poll_errors) {
                t.out << "  poll error: " << poll_error << "\n";
            }
            for (const auto& poll_error : result_poll_errors) {
                t.out << "  result poll error: " << poll_error << "\n";
            }
        }

        std::size_t observed_poll_count = 0;
        bool saw_cancel_requested_state = false;
        bool saw_canceled_state = false;
        {
            std::lock_guard lock(poll_mutex);
            observed_poll_count = polled_states.size();
            for (const auto state : polled_states) {
                saw_cancel_requested_state =
                    saw_cancel_requested_state || state == sugar::service::JobState::CancelRequested;
                saw_canceled_state = saw_canceled_state || state == sugar::service::JobState::Canceled;
            }
        }
        t.expect(observed_poll_count >= kPollerCount,
            "live TCP cancel-polling path performs repeated status reads instead of a single terminal fetch");
        t.expect(result_poll_count >= kResultPollerCount,
            "live TCP cancel-polling path performs repeated result reads while cancellation is in flight");

        sugar::service::JobState final_state = sugar::service::JobState::Queued;
        bool saw_final_active_state = false;
        std::string final_failure;
        std::size_t final_poll_count = 0;
        err.clear();
        const bool reached_terminal = wait_for_terminal_status_over_tcp(
            server.port(),
            owner_session_id,
            job_id,
            final_state,
            saw_final_active_state,
            final_failure,
            final_poll_count,
            err);
        t.expect(reached_terminal, "live TCP cancel-polling path reaches a terminal state after cancellation");
        t.expect(reached_terminal && (final_poll_count > 0 || saw_final_active_state),
            "live TCP cancel-polling path can still query terminal owner status after cancellation");
        t.expect(reached_terminal && final_state == sugar::service::JobState::Canceled,
            "live TCP cancel-polling path terminates the owner job as canceled");
        if (reached_terminal && final_state != sugar::service::JobState::Canceled && !final_failure.empty()) {
            t.out << "  final failure detail: " << final_failure << "\n";
        }
        t.expect(saw_cancel_requested_state || saw_canceled_state,
            "live TCP cancel-polling path exposes cancel-related states during repeated polling");

        sugar::service::ProtocolRequest result_request{};
        result_request.command = sugar::service::ProtocolCommand::RetrieveResults;
        result_request.session_id = owner_session_id;
        result_request.job_id = job_id;
        ParsedProtocolResponse canceled_result_response{};
        err.clear();
        const bool canceled_result_replied =
            send_tcp_protocol_request(server.port(), result_request, canceled_result_response, err);
        t.expect(canceled_result_replied, "live TCP cancel-polling path returns a structured reply for canceled result retrieval");
        t.expect(
            canceled_result_replied &&
                !canceled_result_response.ok &&
                canceled_result_response.message.find("not available") != std::string::npos,
            "live TCP cancel-polling path leaves canceled result retrieval explicitly unavailable");

        err.clear();
        const bool disconnected_owner = disconnect_session(server.port(), owner_session_id, err);
        const bool disconnected_intruder = disconnect_session(server.port(), intruder_session_id, err);
        t.expect(disconnected_owner && disconnected_intruder,
            "live TCP cancel-polling path disconnects both owner and intruder sessions cleanly");

        server.stop();
        t.expect(server.exit_code() == 0, "live TCP cancel-polling server stops cleanly after cancel traffic");
        if (server.exit_code() != 0) {
            t.out << "  server stderr:\n" << server.stderr_text();
        }

        return true;
    }

    bool test_service_protocol_remote_bind_policy_contracts(ContractAsserts& t) {
        t.out << "[service_protocol_remote_bind_policy] live TCP remote-bind policy enforcement\n";

        std::string socket_runtime_err;
        ScopedSocketRuntime socket_runtime(socket_runtime_err);
        t.expect(socket_runtime.active(), "socket runtime initializes for remote-bind contracts");
        if (!socket_runtime.active()) {
            t.out << "  socket runtime error: " << socket_runtime_err << "\n";
            return true;
        }

        {
            sugar::service::ServerCliOptions options{};
            options.bind_host = "0.0.0.0";
            options.storage_root = contract_tmp_case_dir("service_protocol_remote_bind_policy_reject");

            std::ostringstream out;
            std::ostringstream err;
            const int exit_code = sugar::service::run_tcp_server(options, out, err);
            t.expect(exit_code != 0,
                "server rejects non-loopback bind without the explicit unsafe flag");
            t.expect(err.str().find("--allow-remote-unsafe") != std::string::npos,
                "server explains that non-loopback bind requires the explicit unsafe flag");
        }

        ScopedLiveTcpServer server{};
        sugar::service::ServerCliOptions options{};
        options.bind_host = "0.0.0.0";
        options.allow_remote_unsafe = true;
        options.storage_root = contract_tmp_case_dir("service_protocol_remote_bind_policy_allow");
        options.source_root = std::filesystem::current_path();

        std::string err;
        const bool started = server.start(options, err);
        t.expect(started, "server starts on a non-loopback bind when the unsafe flag is explicit");
        if (!started) {
            return true;
        }

        ParsedProtocolResponse response{};
        sugar::service::ProtocolRequest request{};
        request.command = sugar::service::ProtocolCommand::Connect;
        std::string response_json;
        const bool replied = send_tcp_protocol_request(server.port(), request, response, response_json, err);
        t.expect(replied && response.ok && !response.session_id.empty(),
            "client can still connect through loopback after an explicit unsafe remote bind");

        server.stop();
        t.expect(server.exit_code() == 0, "unsafe remote-bind server stops cleanly after explicit remote-bind connect");
        t.expect(server.stderr_text().find("without authentication or transport protection") != std::string::npos,
            "server emits a loud warning when non-loopback bind is allowed explicitly");

        return true;
    }

    bool test_service_protocol_slow_client_resilience_contracts(ContractAsserts& t) {
        t.out << "[service_protocol_slow_client_resilience] live TCP slow-client and flood resilience\n";

        std::string socket_runtime_err;
        ScopedSocketRuntime socket_runtime(socket_runtime_err);
        t.expect(socket_runtime.active(), "socket runtime initializes for slow-client contracts");
        if (!socket_runtime.active()) {
            t.out << "  socket runtime error: " << socket_runtime_err << "\n";
            return true;
        }

        ScopedLiveTcpServer server{};
        sugar::service::ServerCliOptions options{};
        options.storage_root = contract_tmp_case_dir("service_protocol_slow_client_resilience");
        options.source_root = std::filesystem::current_path();
        options.worker_limit = 1;
        options.connection_worker_limit = 1;
        options.max_pending_connections = 1;
        options.socket_timeout_ms = 100;

        std::string err;
        const bool started = server.start(options, err);
        t.expect(started, "live TCP server starts for slow-client resilience test");
        if (!started) {
            return true;
        }

        sugar::service::NativeSocketHandle slow_header_fd = kInvalidSocketHandle;
        sugar::service::NativeSocketHandle slow_body_fd = kInvalidSocketHandle;
        std::vector<sugar::service::NativeSocketHandle> idle_fds;
        const bool opened_header = open_connected_loopback_socket(server.port(), slow_header_fd, err);
        const bool opened_body = open_connected_loopback_socket(server.port(), slow_body_fd, err);
        t.expect(opened_header && opened_body,
            "slow-client resilience test opens raw slow-header and slow-body sockets");
        if (opened_header) {
            const char partial_header[2]{ 0, 0 };
            t.expect(write_all(slow_header_fd, partial_header, sizeof(partial_header)),
                "slow-client resilience test writes a partial frame header");
        }
        if (opened_body) {
            const char full_header[4]{ 0, 0, 0, 8 };
            const char partial_body[2]{ '{', '}' };
            t.expect(write_all(slow_body_fd, full_header, sizeof(full_header)),
                "slow-client resilience test writes a full frame header");
            t.expect(write_all(slow_body_fd, partial_body, sizeof(partial_body)),
                "slow-client resilience test writes only part of the declared frame body");
        }

        for (int i = 0; i < 4; ++i) {
            sugar::service::NativeSocketHandle idle_fd = kInvalidSocketHandle;
            if (open_connected_loopback_socket(server.port(), idle_fd, err)) {
                idle_fds.push_back(idle_fd);
            }
        }
        t.expect(!idle_fds.empty(), "slow-client resilience test opens several idle flood sockets");

        std::this_thread::sleep_for(std::chrono::milliseconds(350));

        std::string session_id;
        err.clear();
        const bool connected = connect_session(server.port(), session_id, err);
        t.expect(connected && !session_id.empty(),
            "server remains responsive after slow-header, slow-body, and idle-flood pressure");
        if (connected) {
            std::string disconnect_err;
            (void)disconnect_session(server.port(), session_id, disconnect_err);
        }

        if (is_valid_socket(slow_header_fd)) {
            close_socket_quietly(slow_header_fd);
        }
        if (is_valid_socket(slow_body_fd)) {
            close_socket_quietly(slow_body_fd);
        }
        for (const auto fd : idle_fds) {
            close_socket_quietly(fd);
        }

        server.stop();
        t.expect(server.exit_code() == 0, "slow-client resilience server stops cleanly after timeout pressure");
        t.expect(
            server.stdout_text().find("SECURITY schema_version=sugarbot.hosted.v1 category=protocol event=malformed_frame")
                != std::string::npos,
            "slow-client resilience path records schema-stamped malformed-frame protocol audit events");

        return true;
    }

} // namespace sugar::contract_suite_internal
