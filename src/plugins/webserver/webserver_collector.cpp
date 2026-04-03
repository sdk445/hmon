#include "webserver_collector.hpp"

#include <cstdio>
#include <sstream>
#include <string>
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
    return runCmd("which " + cmd + " 2>/dev/null").size() > 0;
}

static std::string httpGet(const std::string& url, int timeout_ms = 1000) {
    char cmd[512];
    std::snprintf(cmd, sizeof(cmd), "curl -s --max-time %d %s 2>/dev/null", timeout_ms / 1000, url.c_str());
    return runCmd(cmd);
}

}

namespace hmon::plugins::webserver {

std::vector<WebServerInfo> collectWebServers(WebServerPluginCtx* ctx) {
    std::vector<WebServerInfo> result;

    /* Nginx */
    if (cmdExists("nginx") || runCmd("systemctl is-active nginx 2>/dev/null") == "active") {
        WebServerInfo ws;
        ws.type = "nginx";
        ws.status = (runCmd("systemctl is-active nginx 2>/dev/null") == "active") ? "running" : "stopped";
        if (ws.status == "running") {
            /* Try stub_status endpoint */
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
                    /* Lines: server accepts handled requests */
                    /* Then: N N N where N is total requests */
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
            /* Get uptime from systemctl */
            std::string uptime_str = runCmd("systemctl show nginx --property=ActiveEnterTimestamp 2>/dev/null");
            /* Just mark as running, detailed uptime would need more parsing */
        }
        result.push_back(std::move(ws));
    }

    /* Apache */
    if (cmdExists("apache2") || cmdExists("httpd") ||
        runCmd("systemctl is-active apache2 2>/dev/null") == "active" ||
        runCmd("systemctl is-active httpd 2>/dev/null") == "active") {
        WebServerInfo ws;
        ws.type = "apache";
        ws.status = (runCmd("systemctl is-active apache2 2>/dev/null") == "active" ||
                     runCmd("systemctl is-active httpd 2>/dev/null") == "active") ? "running" : "stopped";
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

    return result;
}

}
