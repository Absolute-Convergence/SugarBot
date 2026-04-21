#include <iostream>

#include "service/network_server.h"

int main(int argc, char** argv) {
    return sugar::service::run_tcp_server_from_cli(argc, argv, std::cout, std::cerr);
}
