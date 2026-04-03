#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace hmon::plugins::database {

struct DbInfo {
    std::string type;
    std::string status;
    int active_connections = 0;
    int max_connections = 0;
    int64_t uptime_seconds = 0;
    std::string version;
};

struct DatabasePluginCtx {
    std::vector<DbInfo> databases;
    std::vector<DbInfo> cached_result;
    std::chrono::steady_clock::time_point last_cache_time;
    static constexpr int TTL_SECONDS = 15;
};

std::vector<DbInfo> collectDatabases(DatabasePluginCtx* ctx);

}
