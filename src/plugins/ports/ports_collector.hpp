#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hmon::plugins::ports {

struct ListeningPort {
    uint16_t port = 0;
    std::string proto;       /* tcp, tcp6, udp, udp6 */
    std::string local_addr;
    int pid = -1;
    std::string process;
};

struct PortsPluginCtx {
    std::vector<ListeningPort> ports;
};

std::vector<ListeningPort> collectListeningPorts(PortsPluginCtx* ctx);

}
