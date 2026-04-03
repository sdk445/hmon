#pragma once

#include <string>
#include <vector>

namespace hmon::plugins::webserver {

struct WebServerInfo {
    std::string type;          /* nginx, apache, caddy */
    std::string status;        /* running, stopped */
    int active_connections = 0;
    double requests_per_sec = 0.0;
    int64_t total_requests = 0;
    int64_t uptime_seconds = 0;
};

struct WebServerPluginCtx {
    std::vector<WebServerInfo> servers;
    int64_t prev_total_requests = 0;
};

std::vector<WebServerInfo> collectWebServers(WebServerPluginCtx* ctx);

}
