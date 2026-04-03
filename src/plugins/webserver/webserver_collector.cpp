#include "webserver_collector.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

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

static bool systemdServiceActive(const std::string& name) {
    std::vector<fs::path> cgroup_paths = {
        fs::path("/sys/fs/cgroup/system.slice") / (name + ".service"),
        fs::path("/sys/fs/cgroup/systemd/system.slice") / (name + ".service"),
    };
    for (const auto& p : cgroup_paths) {
        if (fs::exists(p)) {
            fs::path procs = p / "cgroup.procs";
            std::ifstream f(procs);
            if (f) {
                std::string line;
                if (std::getline(f, line) && !trim(line).empty()) return true;
            }
        }
    }
    return false;
}

static std::string httpGet(const std::string& url, int timeout_ms = 1000) {
    char cmd[512];
    std::snprintf(cmd, sizeof(cmd), "curl -s --max-time %d %s 2>/dev/null", timeout_ms / 1000, url.c_str());
    return runCmd(cmd);
}

}

namespace hmon::plugins::webserver {

std::vector<WebServerInfo> collectWebServers(WebServerPluginCtx* ctx) {
    if (ctx) {
        auto elapsed = std::chrono::steady_clock::now() - ctx->last_cache_time;
        if (!ctx->cached_result.empty() &&
            std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() < WebServerPluginCtx::TTL_SECONDS) {
            return ctx->cached_result;
        }
    }

    std::vector<WebServerInfo> result;

    if (cmdExists("nginx") || systemdServiceActive("nginx")) {
        WebServerInfo ws;
        ws.type = "nginx";
        ws.status = systemdServiceActive("nginx") ? "running" : "stopped";
        if (ws.status == "running") {
            std::string status = httpGet("http://127.0.0.1/nginx_status");
            if (status.empty()) status = httpGet("http://127.0.0.1:8080/nginx_status");
            if (!status.empty() && status.find("Active connections") != std::string::npos) {
                std::istringstream iss(status);
                std::string line;
                while (std::getline(iss, line)) {
                    line = trim(line);
                    if (line.rfind("Active connections:", 0) == 0) {
                        try { ws.active_connections = std::stoi(line.substr(19)); } catch (...) {}
                    }
                    if (line.find(" ") != std::string::npos && line.find(":") == std::string::npos) {
                        std::istringstream iss2(line);
                        int64_t accepts, handled, requests;
                        if (iss2 >> accepts >> handled >> requests) {
                            ws.total_requests = requests;
                            if (ctx && ctx->prev_total_requests > 0) {
                                ws.requests_per_sec = static_cast<double>(requests - ctx->prev_total_requests);
                            }
                            ctx->prev_total_requests = requests;
                        }
                    }
                }
            }
        }
        result.push_back(std::move(ws));
    }

    if (cmdExists("apache2") || cmdExists("httpd") ||
        systemdServiceActive("apache2") || systemdServiceActive("httpd")) {
        WebServerInfo ws;
        ws.type = "apache";
        ws.status = (systemdServiceActive("apache2") || systemdServiceActive("httpd")) ? "running" : "stopped";
        if (ws.status == "running") {
            std::string status = httpGet("http://127.0.0.1/server-status?auto");
            if (status.empty()) status = httpGet("http://127.0.0.1:8080/server-status?auto");
            if (!status.empty()) {
                std::istringstream iss(status);
                std::string line;
                while (std::getline(iss, line)) {
                    line = trim(line);
                    if (line.rfind("Total Accesses:", 0) == 0) {
                        try { ws.total_requests = std::stoll(line.substr(15)); } catch (...) {}
                    }
                    if (line.rfind("BusyWorkers:", 0) == 0) {
                        try { ws.active_connections = std::stoi(line.substr(12)); } catch (...) {}
                    }
                    if (line.rfind("ReqPerSec:", 0) == 0) {
                        try { ws.requests_per_sec = std::stod(line.substr(10)); } catch (...) {}
                    }
                }
                if (ctx && ctx->prev_total_requests > 0 && ws.total_requests > ctx->prev_total_requests) {
                    ws.requests_per_sec = static_cast<double>(ws.total_requests - ctx->prev_total_requests);
                }
                ctx->prev_total_requests = ws.total_requests;
            }
        }
        result.push_back(std::move(ws));
    }

    if (ctx) {
        ctx->cached_result = result;
        ctx->last_cache_time = std::chrono::steady_clock::now();
    }
    return result;
}

}
