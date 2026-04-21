#include <iostream>

#include "service/network_client.h"

int main(int argc, char** argv) {
    return sugar::service::run_tcp_client_from_cli(argc, argv, std::cout, std::cerr);
}
