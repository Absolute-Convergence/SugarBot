#include "contract_suite_internal.h"
#include "contract_suite_service_protocol_support.h"

#include <filesystem>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "service/network_server.h"
#include "service/network_session.h"
#include "service/protocol_dispatch.h"
#include "service/protocol_framing.h"

namespace sugar::contract_suite_internal {

    using namespace service_protocol_contract;

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
