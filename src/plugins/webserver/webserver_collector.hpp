#pragma once

#include <chrono>
#include <string>
#include <vector>

namespace hmon::plugins::webserver {

struct WebServerInfo {
    std::string type;
    std::string status;
    int active_connections = 0;
    double requests_per_sec = 0.0;
    int64_t total_requests = 0;
    int64_t uptime_seconds = 0;
};

struct WebServerPluginCtx {
    std::vector<WebServerInfo> servers;
    std::vector<WebServerInfo> cached_result;
    std::chrono::steady_clock::time_point last_cache_time;
    static constexpr int TTL_SECONDS = 10;
    int64_t prev_total_requests = 0;
};

std::vector<WebServerInfo> collectWebServers(WebServerPluginCtx* ctx);

}
