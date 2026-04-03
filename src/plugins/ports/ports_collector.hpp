#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace hmon::plugins::ports {

struct ListeningPort {
    uint16_t port = 0;
    std::string proto;
    std::string local_addr;
    int pid = -1;
    std::string process;
};

struct PortsPluginCtx {
    std::vector<ListeningPort> ports;
    std::vector<ListeningPort> cached_result;
    std::chrono::steady_clock::time_point last_cache_time;
    static constexpr int TTL_SECONDS = 5;
};

std::vector<ListeningPort> collectListeningPorts(PortsPluginCtx* ctx);

}
