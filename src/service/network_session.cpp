#include "service/network_session.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>

#include "service/protocol_framing.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif

namespace sugar::service {

    namespace {

#ifdef _WIN32
        SOCKET to_socket(NativeSocketHandle handle) {
            return static_cast<SOCKET>(handle);
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
#else
        int to_socket(NativeSocketHandle handle) {
            return handle;
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
#endif

        enum class SocketIoState {
            Complete = 0,
            PeerClosed = 1,
            Error = 2,
        };

        struct SocketIoResult {
            SocketIoState state{ SocketIoState::Complete };
            std::size_t transferred{ 0 };
            std::string detail{};
        };

        enum class FramedReadState {
            Complete = 0,
            PeerClosed = 1,
            Malformed = 2,
        };

        struct FramedReadResult {
            FramedReadState state{ FramedReadState::Complete };
            std::string payload{};
            std::string detail{};
        };

        SocketIoResult read_exact(NativeSocketHandle fd, char* buffer, std::size_t size) {
            SocketIoResult result{};
            std::size_t offset = 0;
            while (offset < size) {
                const int chunk_size = static_cast<int>(std::min<std::size_t>(
                    size - offset,
                    static_cast<std::size_t>(std::numeric_limits<int>::max())));
                const auto got = ::recv(to_socket(fd), buffer + offset, chunk_size, 0);
                if (got == 0) {
                    result.state = SocketIoState::PeerClosed;
                    result.transferred = offset;
                    return result;
                }
                if (got < 0) {
                    const int error_code = last_socket_error();
                    if (is_interrupted_socket_error(error_code)) {
                        continue;
                    }
                    result.state = SocketIoState::Error;
                    result.transferred = offset;
                    result.detail = socket_error_message(error_code);
                    return result;
                }
                offset += static_cast<std::size_t>(got);
            }
            result.state = SocketIoState::Complete;
            result.transferred = offset;
            return result;
        }

        bool write_all(NativeSocketHandle fd, const char* buffer, std::size_t size, std::string& err) {
            std::size_t offset = 0;
#if defined(MSG_NOSIGNAL)
            constexpr int kSendFlags = MSG_NOSIGNAL;
#else
            constexpr int kSendFlags = 0;
#endif
            while (offset < size) {
                const int chunk_size = static_cast<int>(std::min<std::size_t>(
                    size - offset,
                    static_cast<std::size_t>(std::numeric_limits<int>::max())));
                const auto sent = ::send(to_socket(fd), buffer + offset, chunk_size, kSendFlags);
                if (sent <= 0) {
                    const int error_code = last_socket_error();
                    if (sent < 0 && is_interrupted_socket_error(error_code)) {
                        continue;
                    }
                    err = socket_error_message(error_code);
                    return false;
                }
                offset += static_cast<std::size_t>(sent);
            }
            err.clear();
            return true;
        }

        FramedReadResult read_one_framed_message(NativeSocketHandle fd, std::size_t max_message_bytes) {
            FramedReadResult result{};

            std::array<char, 4> header{};
            const SocketIoResult header_io = read_exact(fd, header.data(), header.size());
            if (header_io.state != SocketIoState::Complete) {
                if (header_io.state == SocketIoState::PeerClosed && header_io.transferred == 0) {
                    result.state = FramedReadState::PeerClosed;
                    result.detail = "socket closed before request frame";
                    return result;
                }
                result.state = FramedReadState::Malformed;
                result.detail = header_io.state == SocketIoState::PeerClosed
                    ? "socket closed while reading frame header"
                    : "socket read failed while reading frame header: " + header_io.detail;
                return result;
            }

            const std::uint32_t size =
                (static_cast<std::uint32_t>(static_cast<unsigned char>(header[0])) << 24) |
                (static_cast<std::uint32_t>(static_cast<unsigned char>(header[1])) << 16) |
                (static_cast<std::uint32_t>(static_cast<unsigned char>(header[2])) << 8) |
                static_cast<std::uint32_t>(static_cast<unsigned char>(header[3]));
            if (size == 0 || size > max_message_bytes) {
                result.state = FramedReadState::Malformed;
                result.detail = "socket frame length is invalid";
                return result;
            }

            std::string payload(size, '\0');
            const SocketIoResult body_io = read_exact(fd, payload.data(), payload.size());
            if (body_io.state != SocketIoState::Complete) {
                result.state = FramedReadState::Malformed;
                result.detail = body_io.state == SocketIoState::PeerClosed
                    ? "socket closed while reading frame body"
                    : "socket read failed while reading frame body: " + body_io.detail;
                return result;
            }

            result.state = FramedReadState::Complete;
            result.payload = std::move(payload);
            result.detail.clear();
            return result;
        }

        std::string make_protocol_error_json(
            ProtocolErrorCode error_code,
            std::string_view detail) {
            ProtocolResponse response{};
            response.command = ProtocolCommand::Unknown;
            response.ok = false;
            response.error_code = error_code;
            response.message = std::string(detail);
            return serialize_protocol_response_json(response);
        }

    } // namespace

    bool handle_protocol_socket_connection(
        NativeSocketHandle fd,
        ProtocolDispatcher& dispatcher,
        std::size_t max_message_bytes,
        const SecurityLogFn& security_log,
        std::string& err) {
        const FramedReadResult read_result = read_one_framed_message(fd, max_message_bytes);
        if (read_result.state == FramedReadState::PeerClosed) {
            err.clear();
            return true;
        }

        std::string response_json{};
        if (read_result.state == FramedReadState::Complete) {
            dispatcher.handle_json_request(read_result.payload, response_json);
        }
        else {
            if (security_log) {
                security_log("event=malformed_frame result=reject detail=" + read_result.detail);
            }
            response_json = make_protocol_error_json(ProtocolErrorCode::MalformedRequest, read_result.detail);
        }

        const std::string framed = frame_json_message(response_json);
        if (!write_all(fd, framed.data(), framed.size(), err)) {
            if (err.empty()) {
                err = "failed to write response frame";
            }
            return false;
        }

        err.clear();
        return true;
    }

} // namespace sugar::service
