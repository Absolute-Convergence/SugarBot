#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

#include "service/protocol_dispatch.h"

namespace sugar::service {

    bool handle_protocol_socket_connection(
        int fd,
        ProtocolDispatcher& dispatcher,
        std::size_t max_message_bytes,
        const SecurityLogFn& security_log,
        std::string& err);

} // namespace sugar::service
