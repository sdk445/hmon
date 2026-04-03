#pragma once

#include <chrono>
#include <string>
#include <vector>

namespace hmon::plugins::systemd {

struct ServiceInfo {
    std::string name;
    std::string load_state;
    std::string active_state;
    std::string sub_state;
    std::string description;
};

struct SystemdPluginCtx {
    std::vector<ServiceInfo> services;
    std::vector<ServiceInfo> cached_result;
    std::chrono::steady_clock::time_point last_cache_time;
    static constexpr int TTL_SECONDS = 10;
};

std::vector<ServiceInfo> collectServices(SystemdPluginCtx* ctx);

}
