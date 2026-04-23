#include "service/network_client.h"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include "service/protocol_framing.h"

#ifdef _WIN32

namespace sugar::service {

    int run_tcp_client_from_cli(int, char**, std::ostream&, std::ostream& err) {
        err << "Error: sugar_client TCP sockets are currently supported only on Linux/WSL builds.\n";
        return 1;
    }

} // namespace sugar::service

#else

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace sugar::service {

    namespace {
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

        bool read_file_text(const std::filesystem::path& path, std::string& out) {
            std::ifstream file(path, std::ios::binary);
            if (!file) {
                return false;
            }
            out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
            return true;
        }

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

        bool send_request(
            const ClientCliOptions& options,
            const ProtocolRequest& request,
            std::string& response_json,
            std::string& err) {
            const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) {
                err = "failed to create client socket";
                return false;
            }
            if (!configure_connected_socket(fd, options.socket_timeout_ms, err)) {
                ::close(fd);
                return false;
            }

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(options.port);
            if (::inet_pton(AF_INET, options.host.c_str(), &addr.sin_addr) != 1) {
                ::close(fd);
                err = "invalid host";
                return false;
            }
            if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
                ::close(fd);
                err = "failed to connect to server";
                return false;
            }

            const auto request_json = serialize_protocol_request_json(request);
            const auto framed = frame_json_message(request_json);
            if (!write_all(fd, framed.data(), framed.size())) {
                ::close(fd);
                err = "failed to send framed request";
                return false;
            }

            char header[4]{};
            if (!read_exact(fd, header, sizeof(header))) {
                ::close(fd);
                err = "failed to read response header";
                return false;
            }
            const std::uint32_t size =
                (static_cast<std::uint32_t>(static_cast<unsigned char>(header[0])) << 24) |
                (static_cast<std::uint32_t>(static_cast<unsigned char>(header[1])) << 16) |
                (static_cast<std::uint32_t>(static_cast<unsigned char>(header[2])) << 8) |
                static_cast<std::uint32_t>(static_cast<unsigned char>(header[3]));
            if (size == 0 || size > options.max_message_bytes) {
                ::close(fd);
                err = "response frame length is invalid";
                return false;
            }

            response_json.assign(size, '\0');
            if (!read_exact(fd, response_json.data(), response_json.size())) {
                ::close(fd);
                err = "failed to read response body";
                return false;
            }

            ::close(fd);
            err.clear();
            return true;
        }

    } // namespace

    int run_tcp_client_from_cli(int argc, char** argv, std::ostream& out, std::ostream& err) {
        if (argc < 2) {
            err << "Error: missing client command\n";
            return 1;
        }

        ClientCliOptions options{};
        std::string command = argv[1];
        ProtocolRequest request{};

        auto require_value = [&](int& index, const char* label, std::string& value) -> bool {
            if (index + 1 >= argc) {
                err << "Error: missing value for " << label << "\n";
                return false;
            }
            value = argv[++index];
            return true;
        };

        for (int i = 2; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--host") {
                if (!require_value(i, "--host", options.host)) return 1;
            }
            else if (arg == "--port") {
                std::string port_text;
                if (!require_value(i, "--port", port_text)) return 1;
                if (!parse_u16_option(port_text, "--port", options.port, err)) return 1;
            }
            else if (arg == "--socket-timeout-ms") {
                std::string timeout_text;
                if (!require_value(i, "--socket-timeout-ms", timeout_text)) return 1;
                if (!parse_u32_option(timeout_text, "--socket-timeout-ms", 1, 600000, options.socket_timeout_ms, err)) return 1;
            }
            else if (arg == "--session") {
                if (!require_value(i, "--session", request.session_id)) return 1;
            }
            else if (arg == "--job") {
                if (!require_value(i, "--job", request.job_id)) return 1;
            }
            else if (arg == "--layout-upload") {
                if (!require_value(i, "--layout-upload", request.layout_upload_id)) return 1;
            }
            else if (arg == "--logic-upload") {
                if (!require_value(i, "--logic-upload", request.logic_upload_id)) return 1;
            }
            else if (arg == "--data-upload") {
                if (!require_value(i, "--data-upload", request.data_upload_id)) return 1;
            }
            else if (arg == "--kind") {
                std::string kind_name;
                if (!require_value(i, "--kind", kind_name)) return 1;
                bool ok = false;
                if (kind_name == "layout") {
                    request.upload_kind = ArtifactKind::LayoutSpec;
                    ok = true;
                }
                else if (kind_name == "logic") {
                    request.upload_kind = ArtifactKind::LogicSpec;
                    ok = true;
                }
                else if (kind_name == "data") {
                    request.upload_kind = ArtifactKind::DataCsv;
                    ok = true;
                }
                if (!ok) {
                    err << "Error: --kind must be layout, logic, or data\n";
                    return 1;
                }
            }
            else if (arg == "--name") {
                if (!require_value(i, "--name", request.upload_client_name)) return 1;
            }
            else if (arg == "--file") {
                std::string path;
                if (!require_value(i, "--file", path)) return 1;
                if (!read_file_text(path, request.upload_content)) {
                    err << "Error: failed to read file '" << path << "'\n";
                    return 1;
                }
            }
            else {
                err << "Error: unknown client option '" << arg << "'\n";
                return 1;
            }
        }

        if (command == "connect") {
            request.command = ProtocolCommand::Connect;
        }
        else if (command == "list-datasets") {
            request.command = ProtocolCommand::ListDatasets;
        }
        else if (command == "list-templates") {
            request.command = ProtocolCommand::ListTemplates;
        }
        else if (command == "upload") {
            request.command = ProtocolCommand::UploadArtifact;
        }
        else if (command == "validate") {
            request.command = ProtocolCommand::ValidateRequest;
        }
        else if (command == "submit") {
            request.command = ProtocolCommand::SubmitJob;
        }
        else if (command == "status") {
            request.command = ProtocolCommand::CheckStatus;
        }
        else if (command == "result") {
            request.command = ProtocolCommand::RetrieveResults;
        }
        else if (command == "cancel") {
            request.command = ProtocolCommand::CancelJob;
        }
        else if (command == "disconnect") {
            request.command = ProtocolCommand::Disconnect;
        }
        else {
            err << "Error: unknown client command '" << command << "'\n";
            return 1;
        }

        std::string response_json;
        std::string request_err;
        if (!send_request(options, request, response_json, request_err)) {
            err << "Error: " << request_err << "\n";
            return 1;
        }

        out << response_json << "\n";

        bool ok = false;
        std::string parse_err;
        if (!parse_protocol_response_ok(response_json, ok, parse_err)) {
            err << "Error: " << parse_err << "\n";
            return 1;
        }
        return ok ? 0 : 1;
    }

} // namespace sugar::service

#endif
