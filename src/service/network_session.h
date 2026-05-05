#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "service/protocol_dispatch.h"

namespace sugar::service {

#ifdef _WIN32
    using NativeSocketHandle = std::uintptr_t;
#else
    using NativeSocketHandle = int;
#endif

    bool handle_protocol_socket_connection(
        NativeSocketHandle fd,
        ProtocolDispatcher& dispatcher,
        std::size_t max_message_bytes,
        const SecurityLogFn& security_log,
        std::string& err);

} // namespace sugar::service
