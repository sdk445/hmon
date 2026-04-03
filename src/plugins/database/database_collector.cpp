#include "database_collector.hpp"

#include <chrono>
#include <cstdio>
#include <sstream>
#include <string>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace {

static std::string trim(const std::string& s) {
    auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

static std::string runCmd(const std::string& cmd) {
    std::string result;
    FILE* pipe = popen((cmd + " 2>/dev/null").c_str(), "r");
    if (!pipe) return "";
    char buf[256];
    while (fgets(buf, sizeof(buf), pipe)) result += buf;
    pclose(pipe);
    return trim(result);
}

static bool cmdExists(const std::string& cmd) {
    static std::unordered_map<std::string, bool> cache;
    auto it = cache.find(cmd);
    if (it != cache.end()) return it->second;

    std::string paths[] = {
        "/usr/bin/" + cmd,
        "/usr/local/bin/" + cmd,
        "/bin/" + cmd,
        "/snap/bin/" + cmd
    };
    for (const auto& p : paths) {
        if (access(p.c_str(), X_OK) == 0) {
            cache[cmd] = true;
            return true;
        }
    }
    cache[cmd] = false;
    return false;
}

}

namespace hmon::plugins::database {

std::vector<DbInfo> collectDatabases(DatabasePluginCtx* ctx) {
    if (ctx) {
        auto elapsed = std::chrono::steady_clock::now() - ctx->last_cache_time;
        if (!ctx->cached_result.empty() &&
            std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() < DatabasePluginCtx::TTL_SECONDS) {
            return ctx->cached_result;
        }
    }

    std::vector<DbInfo> result;

    /* PostgreSQL */
    if (cmdExists("pg_isready")) {
        DbInfo db;
        db.type = "postgresql";
        std::string ready = runCmd("pg_isready -q 2>&1; echo $?");
        db.status = (ready == "0") ? "running" : "stopped";
        if (db.status == "running") {
            std::string conns = runCmd("psql -tAc \"SELECT count(*) FROM pg_stat_activity WHERE state='active';\" 2>/dev/null");
            if (!conns.empty()) {
                try { db.active_connections = std::stoi(conns); } catch (...) {}
            }
            std::string max_conns = runCmd("psql -tAc \"SHOW max_connections;\" 2>/dev/null");
            if (!max_conns.empty()) {
                try { db.max_connections = std::stoi(max_conns); } catch (...) {}
            }
            std::string uptime = runCmd("psql -tAc \"SELECT extract(epoch from now() - pg_postmaster_start_time())::bigint;\" 2>/dev/null");
            if (!uptime.empty()) {
                try { db.uptime_seconds = std::stoll(uptime); } catch (...) {}
            }
            db.version = runCmd("psql -tAc \"SELECT version();\" 2>/dev/null");
            if (db.version.size() > 40) db.version = db.version.substr(0, 40) + "...";
        }
        result.push_back(std::move(db));
    }

    /* MySQL/MariaDB */
    if (cmdExists("mysqladmin")) {
        DbInfo db;
        db.type = "mysql";
        std::string ping = runCmd("mysqladmin ping 2>/dev/null");
        db.status = (ping.find("alive") != std::string::npos) ? "running" : "stopped";
        if (db.status == "running") {
            std::string status = runCmd("mysqladmin status 2>/dev/null");
            /* Format: Uptime: 12345  Threads: 3  Questions: 123456  Slow queries: 0  Opens: 123  Flush tables: 1  Open tables: 100  Queries per second avg: 12.345 */
            std::istringstream iss(status);
            std::string token;
            while (iss >> token) {
                if (token == "Threads:") { iss >> db.active_connections; }
            }
            std::string max_conns = runCmd("mysql -tAc \"SHOW VARIABLES LIKE 'max_connections';\" 2>/dev/null | tail -1");
            /* Parse max_connections value */
            std::istringstream iss2(max_conns);
            std::string k, v;
            iss2 >> k >> v;
            if (!v.empty()) {
                try { db.max_connections = std::stoi(v); } catch (...) {}
            }
            std::string uptime_str = runCmd("mysql -tAc \"SHOW STATUS LIKE 'Uptime';\" 2>/dev/null | tail -1");
            std::istringstream iss3(uptime_str);
            iss3 >> k >> v;
            if (!v.empty()) {
                try { db.uptime_seconds = std::stoll(v); } catch (...) {}
            }
            db.version = runCmd("mysql -tAc \"SELECT VERSION();\" 2>/dev/null");
        }
        result.push_back(std::move(db));
    }

    /* Redis */
    if (cmdExists("redis-cli")) {
        DbInfo db;
        db.type = "redis";
        std::string ping = runCmd("redis-cli ping 2>/dev/null");
        db.status = (ping == "PONG") ? "running" : "stopped";
        if (db.status == "running") {
            std::string info = runCmd("redis-cli info clients 2>/dev/null");
            std::string line;
            std::istringstream iss_clients(info);
            while (std::getline(iss_clients, line)) {
                std::string l = trim(line);
                if (l.rfind("connected_clients:", 0) == 0) {
                    try { db.active_connections = std::stoi(l.substr(20)); } catch (...) {}
                }
            }
            std::string info_server = runCmd("redis-cli info server 2>/dev/null");
            std::istringstream iss_server(info_server);
            while (std::getline(iss_server, line)) {
                std::string l = trim(line);
                if (l.rfind("redis_version:", 0) == 0) {
                    db.version = l.substr(14);
                }
                if (l.rfind("uptime_in_seconds:", 0) == 0) {
                    try { db.uptime_seconds = std::stoll(l.substr(18)); } catch (...) {}
                }
            }
            std::string max_clients = runCmd("redis-cli config get maxclients 2>/dev/null");
            std::istringstream iss(max_clients);
            std::string k, v;
            iss >> k >> v;
            if (!v.empty()) {
                try { db.max_connections = std::stoi(v); } catch (...) {}
            }
        }
        result.push_back(std::move(db));
    }

    /* MongoDB */
    if (cmdExists("mongosh")) {
        DbInfo db;
        db.type = "mongodb";
        std::string ping = runCmd("mongosh --quiet --eval \"db.adminCommand({ping:1}).ok\" 2>/dev/null");
        db.status = (ping == "1") ? "running" : "stopped";
        if (db.status == "running") {
            std::string conns = runCmd("mongosh --quiet --eval \"db.serverStatus().connections.current\" 2>/dev/null");
            if (!conns.empty()) {
                try { db.active_connections = std::stoi(conns); } catch (...) {}
            }
            std::string max_conns = runCmd("mongosh --quiet --eval \"db.serverStatus().connections.available\" 2>/dev/null");
            if (!max_conns.empty()) {
                try { db.max_connections = db.active_connections + std::stoi(max_conns); } catch (...) {}
            }
            std::string uptime = runCmd("mongosh --quiet --eval \"db.serverStatus().uptime\" 2>/dev/null");
            if (!uptime.empty()) {
                try { db.uptime_seconds = static_cast<int64_t>(std::stod(uptime)); } catch (...) {}
            }
            db.version = runCmd("mongosh --quiet --eval \"db.version()\" 2>/dev/null");
        }
        result.push_back(std::move(db));
    }

    if (ctx) {
        ctx->cached_result = result;
        ctx->last_cache_time = std::chrono::steady_clock::now();
    }
    return result;
}

}
