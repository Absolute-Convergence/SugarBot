#pragma once

#include <cstddef>
#include <string>

#include "service/protocol_dispatch.h"

namespace sugar::service {

    bool handle_protocol_socket_connection(
        int fd,
        ProtocolDispatcher& dispatcher,
        std::size_t max_message_bytes,
        std::string& err);

} // namespace sugar::service
