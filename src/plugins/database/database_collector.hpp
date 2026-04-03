#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace hmon::plugins::database {

struct DbInfo {
    std::string type;          /* postgresql, mysql, redis, mongodb */
    std::string status;        /* running, stopped */
    int active_connections = 0;
    int max_connections = 0;
    int64_t uptime_seconds = 0;
    std::string version;
};

struct DatabasePluginCtx {
    std::vector<DbInfo> databases;
};

std::vector<DbInfo> collectDatabases(DatabasePluginCtx* ctx);

}
