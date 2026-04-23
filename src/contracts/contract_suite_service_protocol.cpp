#include "contract_suite_internal.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "ingest/json_min.h"
#include "service/network_server.h"
#include "service/network_session.h"
#include "service/protocol_dispatch.h"
#include "service/protocol_framing.h"

#ifdef _WIN32

namespace sugar::contract_suite_internal {

    bool test_service_protocol_network_socket_contracts(ContractAsserts& t) {
        t.out << "[service_protocol_network_socket] skipped on Windows stub builds\n";
        return true;
    }

    bool test_service_protocol_live_multi_client_contracts(ContractAsserts& t) {
        t.out << "[service_protocol_live_multi_client] skipped on Windows stub builds\n";
        return true;
    }

    bool test_service_protocol_live_cancel_polling_contracts(ContractAsserts& t) {
        t.out << "[service_protocol_live_cancel_polling] skipped on Windows stub builds\n";
        return true;
    }

    bool test_service_protocol_remote_bind_policy_contracts(ContractAsserts& t) {
        t.out << "[service_protocol_remote_bind_policy] skipped on Windows stub builds\n";
        return true;
    }

    bool test_service_protocol_slow_client_resilience_contracts(ContractAsserts& t) {
        t.out << "[service_protocol_slow_client_resilience] skipped on Windows stub builds\n";
        return true;
    }

} // namespace sugar::contract_suite_internal

#else

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace sugar::contract_suite_internal {

    namespace {
        constexpr std::size_t kProtocolMessageLimit = 4 * 1024 * 1024;
        constexpr std::size_t kSocketContractFrameLimit = 4096;
        constexpr auto kLiveServerStartTimeout = std::chrono::seconds(2);
        constexpr auto kTcpTerminalWaitTimeout = std::chrono::seconds(15);
        constexpr auto kTcpCancelPollerTimeout = std::chrono::seconds(10);
        constexpr auto kTcpCancelActiveWaitTimeout = std::chrono::seconds(5);
        constexpr auto kTcpPollInterval = std::chrono::milliseconds(2);
        constexpr auto kTcpWaitInterval = std::chrono::milliseconds(5);

        struct ParsedProtocolResponse {
            sugar::service::ProtocolCommand command{ sugar::service::ProtocolCommand::Unknown };
            bool ok{ false };
            sugar::service::ProtocolErrorCode error_code{ sugar::service::ProtocolErrorCode::InternalError };
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
        };

        bool read_exact(int fd, char* buffer, std::size_t size) {
            std::size_t offset = 0;
            while (offset < size) {
                const auto got = ::recv(fd, buffer + offset, size - offset, 0);
                if (got < 0 && errno == EINTR) {
                    continue;
                }
                if (got <= 0) {
                    return false;
                }
                offset += static_cast<std::size_t>(got);
            }
            return true;
        }

        bool write_all(int fd, const char* buffer, std::size_t size) {
            std::size_t offset = 0;
            while (offset < size) {
#ifdef MSG_NOSIGNAL
                constexpr int kSendFlags = MSG_NOSIGNAL;
#else
                constexpr int kSendFlags = 0;
#endif
                const auto sent = ::send(fd, buffer + offset, size - offset, kSendFlags);
                if (sent < 0 && errno == EINTR) {
                    continue;
                }
                if (sent <= 0) {
                    return false;
                }
                offset += static_cast<std::size_t>(sent);
            }
            return true;
        }

        bool read_framed_json(int fd, std::size_t max_message_bytes, std::string& out_json, std::string& err) {
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
            const auto* op_value = obj_get(object, "op");
            const auto* error_value = obj_get(object, "error_code");
            const auto* message_value = obj_get(object, "message");
            if (!ok_value || !ok_value->is_bool()) {
                err = "protocol response missing bool ok field";
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

        bool reserve_loopback_port(std::uint16_t& out_port, std::string& err) {
            out_port = 0;

            const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) {
                err = "failed to create probe socket";
                return false;
            }

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(0);
            if (::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
                ::close(fd);
                err = "failed to encode loopback probe address";
                return false;
            }

            if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
                ::close(fd);
                err = "failed to reserve loopback port";
                return false;
            }

            sockaddr_in bound_addr{};
            socklen_t bound_size = sizeof(bound_addr);
            if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound_addr), &bound_size) != 0) {
                ::close(fd);
                err = "failed to query reserved loopback port";
                return false;
            }

            out_port = ntohs(bound_addr.sin_port);
            ::close(fd);
            err.clear();
            return out_port != 0;
        }

        bool can_connect_loopback(std::uint16_t port) {
            const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) {
                return false;
            }

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            if (::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
                ::close(fd);
                return false;
            }

            const bool ok = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
            ::close(fd);
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
            const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) {
                err = "failed to create client socket";
                return false;
            }

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            if (::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
                ::close(fd);
                err = "failed to encode client loopback address";
                return false;
            }
            if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
                ::close(fd);
                err = "failed to connect to live TCP server";
                return false;
            }

            const std::string request_json = sugar::service::serialize_protocol_request_json(request);
            const std::string framed = sugar::service::frame_json_message(request_json);
            if (!write_all(fd, framed.data(), framed.size())) {
                ::close(fd);
                err = "failed to send framed request";
                return false;
            }

            if (!read_framed_json(fd, kProtocolMessageLimit, out_response_json, err)) {
                ::close(fd);
                return false;
            }

            ::close(fd);
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

        bool open_connected_loopback_socket(std::uint16_t port, int& out_fd, std::string& err) {
            out_fd = ::socket(AF_INET, SOCK_STREAM, 0);
            if (out_fd < 0) {
                err = "failed to create raw client socket";
                return false;
            }

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            if (::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
                ::close(out_fd);
                out_fd = -1;
                err = "failed to encode client loopback address";
                return false;
            }
            if (::connect(out_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
                ::close(out_fd);
                out_fd = -1;
                err = "failed to connect raw client socket";
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

        sugar::service::ProtocolDispatcherConfig config{};
        config.runtime.storage_root = contract_tmp_case_dir("service_protocol_network_socket");
        config.source_root = std::filesystem::current_path();
        sugar::service::ProtocolDispatcher dispatcher(config);

        {
            int fds[2]{ -1, -1 };
            if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
                return t.expect(false, "socketpair creates transport fixture");
            }

            sugar::service::ProtocolRequest request{};
            request.command = sugar::service::ProtocolCommand::Connect;
            const std::string request_json = sugar::service::serialize_protocol_request_json(request);
            const std::string framed = sugar::service::frame_json_message(request_json);
            t.expect(write_all(fds[0], framed.data(), framed.size()),
                "socket fixture writes valid framed connect request");

            std::string err;
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

            ::close(fds[0]);
            ::close(fds[1]);
        }

        {
            int fds[2]{ -1, -1 };
            if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
                return t.expect(false, "socketpair recreates transport fixture for truncated frame");
            }

            sugar::service::ProtocolRequest request{};
            request.command = sugar::service::ProtocolCommand::Connect;
            std::string framed = sugar::service::frame_json_message(
                sugar::service::serialize_protocol_request_json(request));
            framed.pop_back();
            t.expect(write_all(fds[0], framed.data(), framed.size()),
                "socket fixture writes truncated framed request bytes");
            ::shutdown(fds[0], SHUT_WR);

            std::string err;
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

            ::close(fds[0]);
            ::close(fds[1]);
        }

        {
            int fds[2]{ -1, -1 };
            if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
                return t.expect(false, "socketpair recreates transport fixture for zero-length frame");
            }

            const std::string zero_length_header(4, '\0');
            t.expect(write_all(fds[0], zero_length_header.data(), zero_length_header.size()),
                "socket fixture writes zero-length frame header bytes");
            ::shutdown(fds[0], SHUT_WR);

            std::string err;
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

            ::close(fds[0]);
            ::close(fds[1]);
        }

        {
            int fds[2]{ -1, -1 };
            if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
                return t.expect(false, "socketpair recreates transport fixture for oversized frame header");
            }

            const std::string oversized_header(4, static_cast<char>(0xFF));
            t.expect(write_all(fds[0], oversized_header.data(), oversized_header.size()),
                "socket fixture writes oversized frame header bytes");
            ::shutdown(fds[0], SHUT_WR);

            std::string err;
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

            ::close(fds[0]);
            ::close(fds[1]);
        }

        {
            int fds[2]{ -1, -1 };
            if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
                return t.expect(false, "socketpair recreates transport fixture for partial frame header");
            }

            std::string partial_header;
            partial_header.push_back('\0');
            partial_header.push_back('\x10');
            t.expect(write_all(fds[0], partial_header.data(), partial_header.size()),
                "socket fixture writes partial frame header bytes");
            ::shutdown(fds[0], SHUT_WR);

            std::string err;
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

            ::close(fds[0]);
            ::close(fds[1]);
        }

        return true;
    }

    bool test_service_protocol_live_multi_client_contracts(ContractAsserts& t) {
        t.out << "[service_protocol_live_multi_client] live TCP multi-client submit/status/result concurrency\n";

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
                    !runs[i].result_response.result_has_primary_backtest) {
                    runs[i].error = runs[i].result_response.message.empty()
                        ? "result response did not preserve owned primary_backtest metadata"
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

        int slow_header_fd = -1;
        int slow_body_fd = -1;
        std::vector<int> idle_fds;
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
            int idle_fd = -1;
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

        if (slow_header_fd >= 0) {
            ::close(slow_header_fd);
        }
        if (slow_body_fd >= 0) {
            ::close(slow_body_fd);
        }
        for (const int fd : idle_fds) {
            ::close(fd);
        }

        server.stop();
        t.expect(server.exit_code() == 0, "slow-client resilience server stops cleanly after timeout pressure");
        t.expect(server.stdout_text().find("SECURITY event=malformed_frame") != std::string::npos,
            "slow-client resilience path records malformed-frame security events");

        return true;
    }

} // namespace sugar::contract_suite_internal

#endif
