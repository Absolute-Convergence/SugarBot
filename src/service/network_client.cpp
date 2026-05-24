#include "service/network_client.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "ingest/json_min.h"
#include "service/network_session.h"
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
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace sugar::service {

    namespace {
#ifdef _WIN32
        constexpr NativeSocketHandle kInvalidSocketHandle = static_cast<NativeSocketHandle>(INVALID_SOCKET);

        SOCKET to_socket(NativeSocketHandle handle) {
            return static_cast<SOCKET>(handle);
        }

        NativeSocketHandle to_native_socket_handle(SOCKET socket) {
            return static_cast<NativeSocketHandle>(socket);
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

        class SocketRuntime {
        public:
            explicit SocketRuntime(std::string& err) {
                WSADATA data{};
                const int result = WSAStartup(MAKEWORD(2, 2), &data);
                if (result != 0) {
                    err = socket_error_message(result);
                    return;
                }
                active_ = true;
                err.clear();
            }

            ~SocketRuntime() {
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
        constexpr NativeSocketHandle kInvalidSocketHandle = -1;

        int to_socket(NativeSocketHandle handle) {
            return handle;
        }

        NativeSocketHandle to_native_socket_handle(int socket) {
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

        class SocketRuntime {
        public:
            explicit SocketRuntime(std::string& err) {
                err.clear();
            }

            bool active() const {
                return true;
            }
        };
#endif

        bool is_valid_socket(NativeSocketHandle handle) {
            return handle != kInvalidSocketHandle;
        }

        void close_socket_quietly(NativeSocketHandle fd) {
            if (!is_valid_socket(fd)) {
                return;
            }
#ifdef _WIN32
            ::closesocket(to_socket(fd));
#else
            ::close(fd);
#endif
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

        bool set_socket_timeout(NativeSocketHandle fd, int optname, std::uint32_t timeout_ms, std::string& err) {
#ifdef _WIN32
            const DWORD timeout = std::max<DWORD>(1, static_cast<DWORD>(timeout_ms));
            if (::setsockopt(
                    to_socket(fd),
                    SOL_SOCKET,
                    optname,
                    reinterpret_cast<const char*>(&timeout),
                    sizeof(timeout)) != 0) {
                err = "failed to set socket timeout: " + socket_error_message(last_socket_error());
                return false;
            }
#else
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
#endif
            err.clear();
            return true;
        }

        bool set_socket_flag(NativeSocketHandle fd, int level, int optname, int value, std::string& err) {
#ifdef _WIN32
            if (::setsockopt(
                    to_socket(fd),
                    level,
                    optname,
                    reinterpret_cast<const char*>(&value),
                    sizeof(value)) != 0) {
                err = "failed to set socket option: " + socket_error_message(last_socket_error());
                return false;
            }
#else
            if (::setsockopt(fd, level, optname, &value, sizeof(value)) != 0) {
                err = std::string("failed to set socket option: ") + std::strerror(errno);
                return false;
            }
#endif
            err.clear();
            return true;
        }

        bool configure_connected_socket(NativeSocketHandle fd, std::uint32_t socket_timeout_ms, std::string& err) {
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

        void print_client_usage(std::ostream& out) {
            out
                << "SugarBot hosted-service client\n\n"
                << "Usage:\n"
                << "  sugar_client --help\n"
                << "  sugar_client connect [options]\n"
                << "  sugar_client list-datasets --session <id> [options]\n"
                << "  sugar_client list-templates --session <id> [options]\n"
                << "  sugar_client upload --session <id> --kind layout|logic|data --file <path> [--name <filename>] [options]\n"
                << "  sugar_client run --layout <layout.json> --logic <logic.json> --data <data.csv> [--wait] [options]\n"
                << "  sugar_client validate --session <id> --layout-upload <id> --logic-upload <id> --data-upload <id> [options]\n"
                << "  sugar_client submit --session <id> --layout-upload <id> --logic-upload <id> --data-upload <id> [options]\n"
                << "  sugar_client status --session <id> --job <id> [options]\n"
                << "  sugar_client result --session <id> --job <id> [options]\n"
                << "  sugar_client cancel --session <id> --job <id> [options]\n"
                << "  sugar_client disconnect --session <id> [options]\n\n"
                << "Options:\n"
                << "  --host <ipv4>              Server host, default 127.0.0.1\n"
                << "  --port <port>              Server port, default 4123\n"
                << "  --socket-timeout-ms <ms>   Bounded socket read/write timeout\n"
                << "  --compact                  Print concise hosted reports/status summaries\n"
                << "  --raw-json                 Print one-line protocol JSON for scripts\n\n"
                << "Run options:\n"
                << "  --wait                     Accepted for clarity; run waits by default\n"
                << "  --poll-ms <ms>             Status poll interval, default 250\n"
                << "  --wait-timeout-ms <ms>     Wait timeout, default 300000\n\n"
                << "Notes:\n"
                << "  Run prints the retained hosted summary by default. Use --compact for a low-repetition report.\n"
                << "  Status/result print pretty JSON by default. Use --compact for concise hosted status/report views.\n"
                << "  Use --raw-json for one-line protocol JSON.\n"
                << "  Run connects, uploads layout/logic/data, submits, waits, and prints the final result.\n"
                << "  Upload --name is optional when --file is provided; the file name is used.\n"
                << "  Layout and logic uploads are JSON; data uploads are CSV.\n";
        }

        std::string default_upload_name_for_path(const std::string& path) {
            return std::filesystem::path(path).filename().string();
        }

        bool read_exact(NativeSocketHandle fd, char* buffer, std::size_t size) {
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

        bool write_all(NativeSocketHandle fd, const char* buffer, std::size_t size) {
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

        bool send_request(
            const ClientCliOptions& options,
            const ProtocolRequest& request,
            std::string& response_json,
            std::string& err) {
            std::string socket_runtime_err;
            SocketRuntime socket_runtime(socket_runtime_err);
            if (!socket_runtime.active()) {
                err = "failed to initialize socket runtime: " + socket_runtime_err;
                return false;
            }

            const NativeSocketHandle fd =
                to_native_socket_handle(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
            if (!is_valid_socket(fd)) {
                err = "failed to create client socket: " + socket_error_message(last_socket_error());
                return false;
            }
            if (!configure_connected_socket(fd, options.socket_timeout_ms, err)) {
                close_socket_quietly(fd);
                return false;
            }

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(options.port);
            if (::inet_pton(AF_INET, options.host.c_str(), &addr.sin_addr) != 1) {
                close_socket_quietly(fd);
                err = "invalid host";
                return false;
            }
            if (::connect(to_socket(fd), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
                const int error_code = last_socket_error();
                close_socket_quietly(fd);
                err = "failed to connect to server: " + socket_error_message(error_code);
                return false;
            }

            const auto request_json = serialize_protocol_request_json(request);
            const auto framed = frame_json_message(request_json);
            if (!write_all(fd, framed.data(), framed.size())) {
                close_socket_quietly(fd);
                err = "failed to send framed request";
                return false;
            }

            char header[4]{};
            if (!read_exact(fd, header, sizeof(header))) {
                close_socket_quietly(fd);
                err = "failed to read response header";
                return false;
            }
            const std::uint32_t size =
                (static_cast<std::uint32_t>(static_cast<unsigned char>(header[0])) << 24) |
                (static_cast<std::uint32_t>(static_cast<unsigned char>(header[1])) << 16) |
                (static_cast<std::uint32_t>(static_cast<unsigned char>(header[2])) << 8) |
                static_cast<std::uint32_t>(static_cast<unsigned char>(header[3]));
            if (size == 0 || size > options.max_message_bytes) {
                close_socket_quietly(fd);
                err = "response frame length is invalid";
                return false;
            }

            response_json.assign(size, '\0');
            if (!read_exact(fd, response_json.data(), response_json.size())) {
                close_socket_quietly(fd);
                err = "failed to read response body";
                return false;
            }

            close_socket_quietly(fd);
            err.clear();
            return true;
        }

        std::string format_json_pretty(std::string_view json) {
            std::string out;
            out.reserve(json.size() * 2);

            auto next_non_space = [json](std::size_t start) {
                for (std::size_t i = start; i < json.size(); ++i) {
                    const unsigned char ch = static_cast<unsigned char>(json[i]);
                    if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
                        return json[i];
                    }
                }
                return '\0';
            };
            auto append_indent = [&out](int indent) {
                if (indent > 0) {
                    out.append(static_cast<std::size_t>(indent) * 4, ' ');
                }
            };

            int indent = 0;
            bool in_string = false;
            bool escape = false;
            char previous_token = '\0';

            for (std::size_t i = 0; i < json.size(); ++i) {
                const char ch = json[i];
                if (in_string) {
                    out.push_back(ch);
                    if (escape) {
                        escape = false;
                    }
                    else if (ch == '\\') {
                        escape = true;
                    }
                    else if (ch == '"') {
                        in_string = false;
                    }
                    continue;
                }

                if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
                    continue;
                }

                switch (ch) {
                case '"':
                    in_string = true;
                    out.push_back(ch);
                    break;
                case '{':
                case '[':
                {
                    out.push_back(ch);
                    const char close = (ch == '{') ? '}' : ']';
                    if (next_non_space(i + 1) != close) {
                        out.push_back('\n');
                        ++indent;
                        append_indent(indent);
                    }
                    previous_token = ch;
                    break;
                }
                case '}':
                case ']':
                {
                    const char open = (ch == '}') ? '{' : '[';
                    if (previous_token != open) {
                        out.push_back('\n');
                        if (indent > 0) {
                            --indent;
                        }
                        append_indent(indent);
                    }
                    else if (indent > 0) {
                        --indent;
                    }
                    out.push_back(ch);
                    previous_token = ch;
                    break;
                }
                case ',':
                    out.push_back(ch);
                    out.push_back('\n');
                    append_indent(indent);
                    previous_token = ch;
                    break;
                case ':':
                    out.push_back(ch);
                    out.push_back(' ');
                    previous_token = ch;
                    break;
                default:
                    out.push_back(ch);
                    previous_token = ch;
                    break;
                }
            }

            return out;
        }

        struct ClientRunOptions {
            std::string layout_path{};
            std::string logic_path{};
            std::string data_path{};
            std::uint32_t poll_ms{ 250 };
            std::uint32_t wait_timeout_ms{ 300000 };
        };

        struct ClientResponseSummary {
            bool ok{ false };
            std::string message{};
            std::string session_id{};
            std::string job_id{};
            std::string upload_id{};
            std::string job_state{};
            std::string result_id{};
            std::string summary_text{};
        };

        bool read_optional_json_string(
            const json_min::Object& object,
            const char* key,
            std::string& out,
            std::string& err) {
            const json_min::Value* value = json_min::obj_get(object, key);
            if (!value) {
                return true;
            }
            if (!value->is_str()) {
                err = std::string("response field '") + key + "' must be a string";
                return false;
            }
            out = value->as_str();
            return true;
        }

        bool parse_client_response_summary(
            std::string_view response_json,
            ClientResponseSummary& out,
            std::string& err) {
            json_min::Value root{};
            json_min::ParseError parse_err{};
            if (!json_min::parse(response_json, root, parse_err)) {
                std::ostringstream detail;
                detail << parse_err.message << " at line " << parse_err.line << ", col " << parse_err.col;
                err = detail.str();
                return false;
            }
            if (!root.is_obj()) {
                err = "response root must be an object";
                return false;
            }

            out = ClientResponseSummary{};
            const auto& object = root.as_obj();
            const json_min::Value* ok_value = json_min::obj_get(object, "ok");
            if (!ok_value || !ok_value->is_bool()) {
                err = "response field 'ok' must be a bool";
                return false;
            }
            out.ok = ok_value->as_bool();

            std::string schema_version;
            if (!read_optional_json_string(object, "schema_version", schema_version, err)) {
                return false;
            }
            if (schema_version != hosted_observability_schema_version()) {
                err = "response schema_version is not supported";
                return false;
            }

            if (!read_optional_json_string(object, "message", out.message, err) ||
                !read_optional_json_string(object, "session_id", out.session_id, err) ||
                !read_optional_json_string(object, "job_id", out.job_id, err)) {
                return false;
            }

            if (const json_min::Value* upload = json_min::obj_get(object, "upload")) {
                if (!upload->is_obj()) {
                    err = "response field 'upload' must be an object";
                    return false;
                }
                if (!read_optional_json_string(upload->as_obj(), "upload_id", out.upload_id, err)) {
                    return false;
                }
            }

            if (const json_min::Value* status = json_min::obj_get(object, "job_status")) {
                if (!status->is_obj()) {
                    err = "response field 'job_status' must be an object";
                    return false;
                }
                const auto& status_object = status->as_obj();
                if (!read_optional_json_string(status_object, "state", out.job_state, err) ||
                    !read_optional_json_string(status_object, "result_id", out.result_id, err)) {
                    return false;
                }
                if (out.job_id.empty() &&
                    !read_optional_json_string(status_object, "job_id", out.job_id, err)) {
                    return false;
                }
            }

            if (const json_min::Value* result = json_min::obj_get(object, "job_result")) {
                if (!result->is_obj()) {
                    err = "response field 'job_result' must be an object";
                    return false;
                }
                const auto& result_object = result->as_obj();
                if (!read_optional_json_string(result_object, "result_id", out.result_id, err)) {
                    return false;
                }
                if (!read_optional_json_string(result_object, "summary_text", out.summary_text, err)) {
                    return false;
                }
                if (out.job_id.empty() &&
                    !read_optional_json_string(result_object, "job_id", out.job_id, err)) {
                    return false;
                }
            }

            err.clear();
            return true;
        }

        bool send_and_parse_response(
            const ClientCliOptions& options,
            const ProtocolRequest& request,
            ClientResponseSummary& summary,
            std::string& response_json,
            std::string& err) {
            if (!send_request(options, request, response_json, err)) {
                return false;
            }
            return parse_client_response_summary(response_json, summary, err);
        }

        bool suppress_progress_output(const ClientCliOptions& options) {
            return options.compact_output || options.raw_json_output;
        }

        std::string compact_scalar_text(const json_min::Value& value) {
            if (value.is_null()) {
                return "null";
            }
            if (value.is_bool()) {
                return value.as_bool() ? "true" : "false";
            }
            if (value.is_i64()) {
                return std::to_string(value.as_i64());
            }
            if (value.is_f64()) {
                std::ostringstream text;
                text << std::setprecision(12) << value.as_f64();
                return text.str();
            }
            if (value.is_str()) {
                return value.as_str();
            }
            if (value.is_arr()) {
                return "<array>";
            }
            return "<object>";
        }

        const json_min::Object* as_json_object(const json_min::Value* value) {
            if (!value || !value->is_obj()) {
                return nullptr;
            }
            return &value->as_obj();
        }

        const json_min::Array* as_json_array(const json_min::Value* value) {
            if (!value || !value->is_arr()) {
                return nullptr;
            }
            return &value->as_arr();
        }

        void append_compact_field(
            std::ostream& out,
            const json_min::Object& object,
            const char* key,
            const char* label = nullptr) {
            const json_min::Value* value = json_min::obj_get(object, key);
            if (!value) {
                return;
            }
            if (value->is_str() && value->as_str().empty()) {
                return;
            }
            out << " " << (label ? label : key) << "=" << compact_scalar_text(*value);
        }

        std::string compact_param_value_text(const json_min::Value& value) {
            if (const json_min::Object* object = as_json_object(&value)) {
                const json_min::Value* ref = json_min::obj_get(*object, "ref");
                if (ref && ref->is_str()) {
                    return std::string("ref:") + ref->as_str();
                }
            }
            return compact_scalar_text(value);
        }

        std::string compact_params_text(const json_min::Value* params_value) {
            const json_min::Object* params = as_json_object(params_value);
            if (!params || params->empty()) {
                return "<none>";
            }

            std::vector<std::string> names;
            names.reserve(params->size());
            for (const auto& [name, value] : *params) {
                (void)value;
                names.push_back(name);
            }
            std::sort(names.begin(), names.end());

            std::ostringstream text;
            for (std::size_t i = 0; i < names.size(); ++i) {
                if (i != 0) {
                    text << ", ";
                }
                const json_min::Value* value = json_min::obj_get(*params, names[i]);
                text << names[i] << "=";
                if (value) {
                    text << compact_param_value_text(*value);
                }
                else {
                    text << "<missing>";
                }
            }
            return text.str();
        }

        void print_compact_counts_line(
            std::ostream& out,
            std::string_view label,
            const json_min::Object& object) {
            out << "  " << label << ":";
            std::vector<std::string> keys;
            keys.reserve(object.size());
            for (const auto& [key, value] : object) {
                (void)value;
                keys.push_back(key);
            }
            std::sort(keys.begin(), keys.end());
            for (const auto& key : keys) {
                if (const json_min::Value* value = json_min::obj_get(object, key)) {
                    out << " " << key << "=" << compact_scalar_text(*value);
                }
            }
            out << "\n";
        }

        void print_compact_dynamic_line(
            std::ostream& out,
            std::string_view label,
            const json_min::Object& dynamic) {
            const json_min::Value* enabled = json_min::obj_get(dynamic, "enabled");
            if (enabled && enabled->is_bool() && !enabled->as_bool()) {
                return;
            }
            out << "  " << label << ":";
            append_compact_field(out, dynamic, "mode");
            append_compact_field(out, dynamic, "total_decisions");
            append_compact_field(out, dynamic, "fallback_decisions");
            append_compact_field(out, dynamic, "suppressed_decisions");
            append_compact_field(out, dynamic, "inactive_output_rows");
            append_compact_field(out, dynamic, "inactive_node_rows");
            append_compact_field(out, dynamic, "max_active_subgraphs_per_row");
            append_compact_field(out, dynamic, "max_active_subgraph_cap");
            append_compact_field(out, dynamic, "bounded_by_declared_caps");
            append_compact_field(out, dynamic, "route_decision_digest");
            out << "\n";
        }

        void print_compact_run_block(
            std::ostream& out,
            std::string_view label,
            const json_min::Object& run) {
            out << "  " << label << ":";
            append_compact_field(out, run, "outcome");
            const json_min::Object* execution =
                as_json_object(json_min::obj_get(run, "execution"));
            if (execution) {
                append_compact_field(out, *execution, "pnl");
                append_compact_field(out, *execution, "trades");
                append_compact_field(out, *execution, "max_drawdown");
                append_compact_field(out, *execution, "trade_hash");
                append_compact_field(out, *execution, "rows_evaluated");
                append_compact_field(out, *execution, "warmup_rows");
            }
            const json_min::Object* objective =
                as_json_object(json_min::obj_get(run, "objective"));
            if (objective) {
                append_compact_field(out, *objective, "rankability", "objective_rankability");
                append_compact_field(out, *objective, "score", "objective_score");
            }
            out << "\n";

            if (execution) {
                const json_min::Object* dynamic =
                    as_json_object(json_min::obj_get(*execution, "dynamic_execution"));
                if (dynamic) {
                    print_compact_dynamic_line(out, "dynamic_execution", *dynamic);
                }
            }

            const json_min::Object* provenance =
                as_json_object(json_min::obj_get(run, "provenance"));
            if (provenance) {
                out << "  provenance:";
                append_compact_field(out, *provenance, "primary_series_key");
                append_compact_field(out, *provenance, "series_count");
                append_compact_field(out, *provenance, "input_warning_count");
                append_compact_field(out, *provenance, "dropped_row_count");
                append_compact_field(out, *provenance, "topology_usage_count");
                append_compact_field(out, *provenance, "topology_usages_suppressed_count");
                out << "\n";
            }
        }

        void print_compact_sweep_report(
            std::ostream& out,
            const json_min::Object& report) {
            out << "  report:";
            append_compact_field(out, report, "schema_version");
            append_compact_field(out, report, "kind");
            append_compact_field(out, report, "engine");
            append_compact_field(out, report, "objective");
            out << "\n";

            out << "  sweep_outcome:";
            append_compact_field(out, report, "canceled");
            append_compact_field(out, report, "evaluations_run");
            append_compact_field(out, report, "compile_failures");
            append_compact_field(out, report, "execution_failures");
            append_compact_field(out, report, "candidate_space_size");
            append_compact_field(out, report, "candidate_space_truncated");
            append_compact_field(out, report, "top_k");
            if (const json_min::Array* warnings =
                    as_json_array(json_min::obj_get(report, "warnings"))) {
                out << " warnings=" << warnings->size();
            }
            out << "\n";

            if (const json_min::Object* outcomes =
                    as_json_object(json_min::obj_get(report, "candidate_outcomes"))) {
                print_compact_counts_line(out, "candidate_outcomes", *outcomes);
            }
            if (const json_min::Object* rankability =
                    as_json_object(json_min::obj_get(report, "candidate_rankability"))) {
                print_compact_counts_line(out, "candidate_rankability", *rankability);
            }

            const json_min::Array* candidates =
                as_json_array(json_min::obj_get(report, "candidates"));
            if (candidates && !candidates->empty()) {
                const json_min::Object* best = as_json_object(&candidates->front());
                if (best) {
                    out << "  best:";
                    append_compact_field(out, *best, "rank");
                    append_compact_field(out, *best, "candidate_index");
                    append_compact_field(out, *best, "score");
                    out << "\n";
                    out << "  best_params: "
                        << compact_params_text(json_min::obj_get(*best, "params"))
                        << "\n";
                    if (const json_min::Object* run =
                            as_json_object(json_min::obj_get(*best, "run"))) {
                        print_compact_run_block(out, "best_run", *run);
                    }
                }
            }

            if (const json_min::Object* trace =
                    as_json_object(json_min::obj_get(report, "proposal_trace"))) {
                out << "  replay:";
                append_compact_field(out, *trace, "captured");
                append_compact_field(out, *trace, "policy_name", "policy");
                append_compact_field(out, *trace, "proposal_count");
                append_compact_field(out, *trace, "observation_count");
                append_compact_field(out, *trace, "events_capped");
                append_compact_field(out, *trace, "trace_digest");
                append_compact_field(out, *trace, "exact_replay_from_report");
                out << "\n";
            }

            if (const json_min::Object* posterior =
                    as_json_object(json_min::obj_get(report, "posterior"))) {
                out << "  posterior:";
                append_compact_field(out, *posterior, "requested");
                append_compact_field(out, *posterior, "computed");
                append_compact_field(out, *posterior, "status");
                append_compact_field(out, *posterior, "training_observation_count");
                append_compact_field(out, *posterior, "finite_observation_count");
                append_compact_field(out, *posterior, "acquisition_ready");
                out << "\n";
            }

            if (const json_min::Object* diversity =
                    as_json_object(json_min::obj_get(report, "result_diversity"))) {
                out << "  diversity:";
                append_compact_field(out, *diversity, "requested");
                append_compact_field(out, *diversity, "retained_candidate_pool");
                append_compact_field(out, *diversity, "returned_unique_trade_hashes");
                append_compact_field(out, *diversity, "suppressed_duplicate_trade_hash");
                append_compact_field(out, *diversity, "suppressed_basin");
                append_compact_field(out, *diversity, "suppressed_min_trades");
                out << "\n";
            }
        }

        void print_compact_panel_report(
            std::ostream& out,
            const json_min::Object& report) {
            out << "  report:";
            append_compact_field(out, report, "schema_version");
            append_compact_field(out, report, "kind");
            append_compact_field(out, report, "ranking_objective");
            out << "\n";
            out << "  panel_outcome:";
            append_compact_field(out, report, "entities_total");
            append_compact_field(out, report, "entities_ok");
            append_compact_field(out, report, "entities_failed");
            if (const json_min::Array* rows =
                    as_json_array(json_min::obj_get(report, "top_rows"))) {
                out << " top_rows=" << rows->size() << "\n";
                if (!rows->empty()) {
                    const json_min::Object* top = as_json_object(&rows->front());
                    if (top) {
                        out << "  top_panel_row:";
                        append_compact_field(out, *top, "rank");
                        append_compact_field(out, *top, "entity_key");
                        append_compact_field(out, *top, "series_key");
                        append_compact_field(out, *top, "ranking_score");
                        out << "\n";
                        if (const json_min::Object* run =
                                as_json_object(json_min::obj_get(*top, "run"))) {
                            print_compact_run_block(out, "top_panel_run", *run);
                        }
                    }
                }
            }
            else {
                out << "\n";
            }
        }

        bool print_hosted_compact_report(
            std::string_view response_json,
            std::ostream& out,
            std::string& err) {
            json_min::Value root{};
            json_min::ParseError parse_err{};
            if (!json_min::parse(response_json, root, parse_err)) {
                std::ostringstream detail;
                detail << parse_err.message << " at line " << parse_err.line
                    << ", col " << parse_err.col;
                err = detail.str();
                return false;
            }
            const json_min::Object* root_object = as_json_object(&root);
            if (!root_object) {
                err = "response root must be an object";
                return false;
            }

            out << "Hosted compact report:\n";
            out << "  protocol:";
            append_compact_field(out, *root_object, "schema_version");
            append_compact_field(out, *root_object, "op");
            append_compact_field(out, *root_object, "ok");
            append_compact_field(out, *root_object, "message");
            out << "\n";

            const json_min::Object* result =
                as_json_object(json_min::obj_get(*root_object, "job_result"));
            if (!result) {
                out << "  report_available=false reason=no_job_result\n";
                err.clear();
                return true;
            }

            out << "  ids:";
            append_compact_field(out, *root_object, "job_id");
            append_compact_field(out, *result, "result_id");
            append_compact_field(out, *result, "owner_session_id");
            out << "\n";

            const json_min::Value* summary = json_min::obj_get(*result, "summary_text");
            if (summary && summary->is_str() && !summary->as_str().empty()) {
                std::string first_line = summary->as_str();
                const std::size_t newline = first_line.find('\n');
                if (newline != std::string::npos) {
                    first_line.resize(newline);
                }
                out << "  server_summary=" << first_line << "\n";
            }

            const json_min::Object* metadata =
                as_json_object(json_min::obj_get(*result, "metadata"));
            const json_min::Object* report = metadata
                ? as_json_object(json_min::obj_get(*metadata, "report"))
                : nullptr;
            if (!report) {
                out << "  report_available=false reason=no_metadata_report\n";
                err.clear();
                return true;
            }

            const json_min::Value* kind_value = json_min::obj_get(*report, "kind");
            const std::string kind = (kind_value && kind_value->is_str())
                ? kind_value->as_str()
                : std::string{};
            if (kind == "sweep") {
                print_compact_sweep_report(out, *report);
            }
            else if (kind == "run") {
                out << "  report:";
                append_compact_field(out, *report, "schema_version");
                append_compact_field(out, *report, "kind");
                out << "\n";
                if (const json_min::Object* run =
                        as_json_object(json_min::obj_get(*report, "run"))) {
                    print_compact_run_block(out, "run", *run);
                }
            }
            else if (kind == "panel") {
                print_compact_panel_report(out, *report);
            }
            else {
                out << "  report:";
                append_compact_field(out, *report, "schema_version");
                append_compact_field(out, *report, "kind");
                out << "\n";
            }

            err.clear();
            return true;
        }

        bool print_hosted_compact_status(
            std::string_view response_json,
            std::ostream& out,
            std::string& err) {
            json_min::Value root{};
            json_min::ParseError parse_err{};
            if (!json_min::parse(response_json, root, parse_err)) {
                std::ostringstream detail;
                detail << parse_err.message << " at line " << parse_err.line
                    << ", col " << parse_err.col;
                err = detail.str();
                return false;
            }
            const json_min::Object* root_object = as_json_object(&root);
            if (!root_object) {
                err = "response root must be an object";
                return false;
            }

            out << "Hosted compact status:\n";
            out << "  protocol:";
            append_compact_field(out, *root_object, "schema_version");
            append_compact_field(out, *root_object, "op");
            append_compact_field(out, *root_object, "ok");
            append_compact_field(out, *root_object, "message");
            out << "\n";

            const json_min::Object* status =
                as_json_object(json_min::obj_get(*root_object, "job_status"));
            if (!status) {
                out << "  status_available=false reason=no_job_status\n";
                err.clear();
                return true;
            }

            out << "  ids:";
            append_compact_field(out, *root_object, "session_id");
            append_compact_field(out, *root_object, "job_id");
            append_compact_field(out, *status, "owner_session_id");
            out << "\n";

            out << "  status:";
            append_compact_field(out, *status, "state");
            append_compact_field(out, *status, "result_id");
            append_compact_field(out, *status, "failure_message");
            out << "\n";

            if (const json_min::Object* summary =
                    as_json_object(json_min::obj_get(*status, "progress_summary"))) {
                out << "  progress:";
                append_compact_field(out, *summary, "event_count", "events");
                append_compact_field(out, *summary, "latest_state");
                append_compact_field(out, *summary, "latest_message");
                append_compact_field(out, *summary, "latest_completed");
                append_compact_field(out, *summary, "latest_total");
                append_compact_field(out, *summary, "work_event_count", "work_events");
                append_compact_field(out, *summary, "latest_work_completed");
                append_compact_field(out, *summary, "latest_work_total");
                append_compact_field(out, *summary, "cancel_requested");
                append_compact_field(out, *summary, "terminal");
                out << "\n";
            }
            else if (const json_min::Array* progress =
                    as_json_array(json_min::obj_get(*status, "progress"))) {
                out << "  progress: events=" << progress->size() << "\n";
            }

            if (const json_min::Object* retention =
                    as_json_object(json_min::obj_get(*status, "retention"))) {
                out << "  retention:";
                append_compact_field(out, *retention, "result_retained");
                append_compact_field(out, *retention, "report_status");
                append_compact_field(out, *retention, "cleanup_eligible");
                append_compact_field(out, *retention, "no_result_reason");
                out << "\n";
            }

            err.clear();
            return true;
        }

        void print_json_response(
            const ClientCliOptions& options,
            std::string_view response_json,
            std::ostream& out) {
            out << (options.raw_json_output ? std::string(response_json) : format_json_pretty(response_json)) << "\n";
        }

        void print_run_result_response(
            const ClientCliOptions& options,
            const ClientResponseSummary& summary,
            std::string_view response_json,
            std::ostream& out) {
            if (options.raw_json_output) {
                print_json_response(options, response_json, out);
                return;
            }
            if (options.compact_output) {
                std::string compact_err;
                if (print_hosted_compact_report(response_json, out, compact_err)) {
                    return;
                }
                out << "Hosted compact report:\n"
                    << "  report_available=false reason=compact_parse_failed\n"
                    << "  detail=" << compact_err << "\n";
                return;
            }
            if (summary.summary_text.empty()) {
                print_json_response(options, response_json, out);
                return;
            }

            out << summary.summary_text;
            if (summary.summary_text.back() != '\n') {
                out << "\n";
            }
        }

        void print_command_response(
            const ClientCliOptions& options,
            std::string_view command,
            std::string_view response_json,
            std::ostream& out) {
            if (options.raw_json_output) {
                print_json_response(options, response_json, out);
                return;
            }
            if (options.compact_output) {
                std::string compact_err;
                if (command == "status") {
                    if (print_hosted_compact_status(response_json, out, compact_err)) {
                        return;
                    }
                    out << "Hosted compact status:\n"
                        << "  status_available=false reason=compact_parse_failed\n"
                        << "  detail=" << compact_err << "\n";
                    return;
                }
                if (command == "result") {
                    if (print_hosted_compact_report(response_json, out, compact_err)) {
                        return;
                    }
                    out << "Hosted compact report:\n"
                        << "  report_available=false reason=compact_parse_failed\n"
                        << "  detail=" << compact_err << "\n";
                    return;
                }
            }
            print_json_response(options, response_json, out);
        }

        bool require_success(
            std::string_view action,
            const ClientResponseSummary& summary,
            std::ostream& err) {
            if (summary.ok) {
                return true;
            }
            err << "Error: " << action << " failed";
            if (!summary.message.empty()) {
                err << ": " << summary.message;
            }
            err << "\n";
            return false;
        }

        bool terminal_job_state(std::string_view state) {
            return state == "completed" ||
                state == "failed" ||
                state == "rejected" ||
                state == "canceled";
        }

        void disconnect_session_best_effort(
            const ClientCliOptions& options,
            std::string_view session_id) {
            if (session_id.empty()) {
                return;
            }
            ProtocolRequest disconnect{};
            disconnect.command = ProtocolCommand::Disconnect;
            disconnect.session_id = std::string(session_id);
            ClientResponseSummary ignored_summary{};
            std::string ignored_response;
            std::string ignored_err;
            (void)send_and_parse_response(options, disconnect, ignored_summary, ignored_response, ignored_err);
        }

        bool upload_run_artifact(
            const ClientCliOptions& options,
            std::string_view session_id,
            ArtifactKind kind,
            std::string_view path,
            std::string_view content,
            std::string& upload_id,
            std::ostream& err) {
            ProtocolRequest upload{};
            upload.command = ProtocolCommand::UploadArtifact;
            upload.session_id = std::string(session_id);
            upload.upload_kind = kind;
            upload.upload_client_name = default_upload_name_for_path(std::string(path));
            upload.upload_content = std::string(content);
            if (upload.upload_client_name.empty()) {
                err << "Error: upload path '" << path << "' does not contain a file name\n";
                return false;
            }

            ClientResponseSummary summary{};
            std::string response_json;
            std::string request_err;
            if (!send_and_parse_response(options, upload, summary, response_json, request_err)) {
                err << "Error: " << request_err << "\n";
                return false;
            }
            if (!require_success("upload", summary, err)) {
                return false;
            }
            if (summary.upload_id.empty()) {
                err << "Error: upload response did not include an upload id\n";
                return false;
            }
            upload_id = summary.upload_id;
            return true;
        }

        int run_hosted_pipeline(
            const ClientCliOptions& options,
            const ClientRunOptions& run_options,
            std::ostream& out,
            std::ostream& err) {
            if (run_options.layout_path.empty()) {
                err << "Error: run requires --layout <layout.json>\n";
                return 1;
            }
            if (run_options.logic_path.empty()) {
                err << "Error: run requires --logic <logic.json>\n";
                return 1;
            }
            if (run_options.data_path.empty()) {
                err << "Error: run requires --data <data.csv>\n";
                return 1;
            }

            std::string layout_content;
            std::string logic_content;
            std::string data_content;
            if (!read_file_text(run_options.layout_path, layout_content)) {
                err << "Error: failed to read layout file '" << run_options.layout_path << "'\n";
                return 1;
            }
            if (!read_file_text(run_options.logic_path, logic_content)) {
                err << "Error: failed to read logic file '" << run_options.logic_path << "'\n";
                return 1;
            }
            if (!read_file_text(run_options.data_path, data_content)) {
                err << "Error: failed to read data file '" << run_options.data_path << "'\n";
                return 1;
            }

            ProtocolRequest connect{};
            connect.command = ProtocolCommand::Connect;
            ClientResponseSummary connect_summary{};
            std::string response_json;
            std::string request_err;
            if (!send_and_parse_response(options, connect, connect_summary, response_json, request_err)) {
                err << "Error: " << request_err << "\n";
                return 1;
            }
            if (!require_success("connect", connect_summary, err)) {
                return 1;
            }
            if (connect_summary.session_id.empty()) {
                err << "Error: connect response did not include a session id\n";
                return 1;
            }
            const std::string session_id = connect_summary.session_id;
            if (!suppress_progress_output(options)) {
                err << "Connected session " << session_id << "\n";
            }

            std::string layout_upload_id;
            std::string logic_upload_id;
            std::string data_upload_id;
            if (!upload_run_artifact(
                    options,
                    session_id,
                    ArtifactKind::LayoutSpec,
                    run_options.layout_path,
                    layout_content,
                    layout_upload_id,
                    err) ||
                !upload_run_artifact(
                    options,
                    session_id,
                    ArtifactKind::LogicSpec,
                    run_options.logic_path,
                    logic_content,
                    logic_upload_id,
                    err) ||
                !upload_run_artifact(
                    options,
                    session_id,
                    ArtifactKind::DataCsv,
                    run_options.data_path,
                    data_content,
                    data_upload_id,
                    err)) {
                disconnect_session_best_effort(options, session_id);
                return 1;
            }
            if (!suppress_progress_output(options)) {
                err << "Uploaded artifacts " << layout_upload_id << ", " << logic_upload_id << ", "
                    << data_upload_id << "\n";
            }

            ProtocolRequest submit{};
            submit.command = ProtocolCommand::SubmitJob;
            submit.session_id = session_id;
            submit.layout_upload_id = layout_upload_id;
            submit.logic_upload_id = logic_upload_id;
            submit.data_upload_id = data_upload_id;
            ClientResponseSummary submit_summary{};
            response_json.clear();
            request_err.clear();
            if (!send_and_parse_response(options, submit, submit_summary, response_json, request_err)) {
                err << "Error: " << request_err << "\n";
                disconnect_session_best_effort(options, session_id);
                return 1;
            }
            if (!require_success("submit", submit_summary, err)) {
                disconnect_session_best_effort(options, session_id);
                return 1;
            }
            if (submit_summary.job_id.empty()) {
                err << "Error: submit response did not include a job id\n";
                disconnect_session_best_effort(options, session_id);
                return 1;
            }
            const std::string job_id = submit_summary.job_id;
            if (!suppress_progress_output(options)) {
                err << "Submitted job " << job_id << "\n";
            }

            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(run_options.wait_timeout_ms);
            std::string final_state;
            std::string latest_status_json;
            std::string last_reported_state;
            while (true) {
                ProtocolRequest status{};
                status.command = ProtocolCommand::CheckStatus;
                status.session_id = session_id;
                status.job_id = job_id;
                ClientResponseSummary status_summary{};
                response_json.clear();
                request_err.clear();
                if (!send_and_parse_response(options, status, status_summary, response_json, request_err)) {
                    err << "Error: " << request_err << "\n";
                    return 1;
                }
                latest_status_json = response_json;
                if (!require_success("status", status_summary, err)) {
                    disconnect_session_best_effort(options, session_id);
                    return 1;
                }
                if (status_summary.job_state.empty()) {
                    err << "Error: status response did not include a job state\n";
                    disconnect_session_best_effort(options, session_id);
                    return 1;
                }
                if (!suppress_progress_output(options) && status_summary.job_state != last_reported_state) {
                    err << "Status: " << status_summary.job_state << "\n";
                    last_reported_state = status_summary.job_state;
                }
                if (terminal_job_state(status_summary.job_state)) {
                    final_state = status_summary.job_state;
                    break;
                }
                if (std::chrono::steady_clock::now() >= deadline) {
                    print_command_response(options, "status", latest_status_json, out);
                    err << "Error: run timed out after " << run_options.wait_timeout_ms
                        << " ms; the job may still be running on the server. "
                        << "To check it later, reuse the same session id with "
                        << "`sugar_client status --session " << session_id
                        << " --job " << job_id << "` or "
                        << "`sugar_client result --session " << session_id
                        << " --job " << job_id << "`. "
                        << "Creating a new session will not have access to this job.\n";
                    return 1;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(run_options.poll_ms));
            }

            ProtocolRequest result{};
            result.command = ProtocolCommand::RetrieveResults;
            result.session_id = session_id;
            result.job_id = job_id;
            ClientResponseSummary result_summary{};
            response_json.clear();
            request_err.clear();
            if (!send_and_parse_response(options, result, result_summary, response_json, request_err)) {
                err << "Error: " << request_err << "\n";
                disconnect_session_best_effort(options, session_id);
                return 1;
            }
            print_run_result_response(options, result_summary, response_json, out);
            disconnect_session_best_effort(options, session_id);
            if (!require_success("result", result_summary, err)) {
                return 1;
            }
            return final_state == "completed" ? 0 : 1;
        }

    } // namespace

    int run_tcp_client_from_cli(int argc, char** argv, std::ostream& out, std::ostream& err) {
        if (argc < 2) {
            print_client_usage(out);
            return 0;
        }

        ClientCliOptions options{};
        ClientRunOptions run_options{};
        std::string command = argv[1];
        ProtocolRequest request{};
        std::string upload_file_path{};

        if (command == "--help" || command == "-h" || command == "help") {
            print_client_usage(out);
            return 0;
        }

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
            if (arg == "--help" || arg == "-h") {
                print_client_usage(out);
                return 0;
            }
            else if (arg == "--host") {
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
            else if (arg == "--poll-ms" && command == "run") {
                std::string poll_text;
                if (!require_value(i, "--poll-ms", poll_text)) return 1;
                if (!parse_u32_option(poll_text, "--poll-ms", 1, 600000, run_options.poll_ms, err)) return 1;
            }
            else if (arg == "--wait-timeout-ms" && command == "run") {
                std::string timeout_text;
                if (!require_value(i, "--wait-timeout-ms", timeout_text)) return 1;
                if (!parse_u32_option(
                        timeout_text,
                        "--wait-timeout-ms",
                        1,
                        86400000,
                        run_options.wait_timeout_ms,
                        err)) {
                    return 1;
                }
            }
            else if (arg == "--compact") {
                options.compact_output = true;
            }
            else if (arg == "--raw-json" || arg == "--json-raw") {
                options.raw_json_output = true;
            }
            else if (arg == "--wait" && command == "run") {
                // The run command waits by default; accept --wait for command discoverability.
            }
            else if (arg == "--layout" && command == "run") {
                if (!require_value(i, "--layout", run_options.layout_path)) return 1;
            }
            else if (arg == "--logic" && command == "run") {
                if (!require_value(i, "--logic", run_options.logic_path)) return 1;
            }
            else if (arg == "--data" && command == "run") {
                if (!require_value(i, "--data", run_options.data_path)) return 1;
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
                upload_file_path = path;
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

        if (options.compact_output && options.raw_json_output) {
            err << "Error: --compact and --raw-json are mutually exclusive\n";
            return 1;
        }

        if (command == "run") {
            return run_hosted_pipeline(options, run_options, out, err);
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
            if (request.upload_client_name.empty() && !upload_file_path.empty()) {
                request.upload_client_name = default_upload_name_for_path(upload_file_path);
            }
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
            err << "Run sugar_client --help for usage.\n";
            return 1;
        }

        std::string response_json;
        std::string request_err;
        if (!send_request(options, request, response_json, request_err)) {
            err << "Error: " << request_err << "\n";
            return 1;
        }

        print_command_response(options, command, response_json, out);

        bool ok = false;
        std::string parse_err;
        if (!parse_protocol_response_ok(response_json, ok, parse_err)) {
            err << "Error: " << parse_err << "\n";
            return 1;
        }
        return ok ? 0 : 1;
    }

} // namespace sugar::service
