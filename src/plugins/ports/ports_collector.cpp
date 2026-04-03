#include "ports_collector.hpp"

#include <algorithm>
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
    char buf[512];
    while (fgets(buf, sizeof(buf), pipe)) result += buf;
    pclose(pipe);
    return result;
}

static std::string findProcessForPort(uint16_t port) {
    /* Try lsof first — most reliable for process names */
    std::string out = runCmd("lsof -i :" + std::to_string(port) + " -sTCP:LISTEN -t 2>/dev/null");
    std::string pid_str = trim(out);
    if (!pid_str.empty()) {
        /* Got PID(s), read /proc/[pid]/comm */
        std::istringstream iss(pid_str);
        std::string pid;
        if (iss >> pid) {
            std::string comm = runCmd("cat /proc/" + pid + "/comm");
            std::string name = trim(comm);
            if (!name.empty()) return name;
            /* Fallback: read cmdline */
            std::string cmdline = runCmd("cat /proc/" + pid + "/cmdline");
            if (!cmdline.empty()) {
                /* cmdline is null-separated, take first part */
                size_t null_pos = cmdline.find('\0');
                if (null_pos != std::string::npos) cmdline = cmdline.substr(0, null_pos);
                /* Extract basename */
                size_t slash = cmdline.rfind('/');
                if (slash != std::string::npos) cmdline = cmdline.substr(slash + 1);
                if (!cmdline.empty()) return cmdline;
            }
        }
    }

    /* Fallback: ss with process info */
    out = runCmd("ss -tlnp sport = :" + std::to_string(port) + " 2>/dev/null");
    size_t users = out.find("users:((");
    if (users != std::string::npos) {
        size_t name_start = users + 8;
        size_t name_end = out.find('"', name_start);
        if (name_end != std::string::npos) return out.substr(name_start, name_end - name_start);
    }

    /* Fallback: check if it's a Docker container port mapping */
    out = runCmd("docker ps --format '{{.Names}} {{.Ports}}' 2>/dev/null");
    if (!out.empty()) {
        std::string port_str = std::to_string(port);
        std::istringstream iss(out);
        std::string line;
        while (std::getline(iss, line)) {
            /* Format: container_name 0.0.0.0:3000->3000/tcp, :::3000->3000/tcp */
            size_t space = line.find(' ');
            if (space == std::string::npos) continue;
            std::string name = line.substr(0, space);
            std::string ports = line.substr(space + 1);
            if (ports.find(":" + port_str + "->") != std::string::npos) {
                return name;
            }
        }
    }

    return "";
}

}

namespace hmon::plugins::ports {

std::vector<ListeningPort> collectListeningPorts(PortsPluginCtx* /*ctx*/) {
    std::vector<ListeningPort> result;

    /* Use ss to get all listening TCP/UDP ports — no root needed */
    std::string output = runCmd("ss -tlnp 2>/dev/null");
    if (!output.empty()) {
        std::istringstream iss(output);
        std::string line;
        std::getline(iss, line); /* skip header */
        while (std::getline(iss, line)) {
            line = trim(line);
            if (line.empty()) continue;

            ListeningPort lp;
            lp.proto = "tcp";

            std::istringstream lis(line);
            std::string state, rq, sq, local;
            lis >> state >> rq >> sq >> local;
            if (local.empty()) continue;

            size_t colon = local.rfind(':');
            if (colon == std::string::npos) continue;

            std::string port_str = local.substr(colon + 1);
            try { lp.port = static_cast<uint16_t>(std::stoi(port_str)); } catch (...) { continue; }
            lp.local_addr = local.substr(0, colon);

            /* Try ss process info first */
            size_t users = line.find("users:((");
            if (users != std::string::npos) {
                size_t name_start = users + 8;
                size_t name_end = line.find('"', name_start);
                if (name_end != std::string::npos) {
                    lp.process = line.substr(name_start, name_end - name_start);
                }
            }

            /* If ss didn't give us a process name, try lsof */
            if (lp.process.empty()) {
                lp.process = findProcessForPort(lp.port);
            }

            result.push_back(std::move(lp));
        }
    }

    /* UDP listening ports */
    output = runCmd("ss -ulnp 2>/dev/null");
    if (!output.empty()) {
        std::istringstream iss(output);
        std::string line;
        std::getline(iss, line); /* skip header */
        while (std::getline(iss, line)) {
            line = trim(line);
            if (line.empty()) continue;

            ListeningPort lp;
            lp.proto = "udp";

            std::istringstream lis(line);
            std::string state, rq, sq, local;
            lis >> state >> rq >> sq >> local;
            if (local.empty()) continue;

            size_t colon = local.rfind(':');
            if (colon == std::string::npos) continue;

            std::string port_str = local.substr(colon + 1);
            try { lp.port = static_cast<uint16_t>(std::stoi(port_str)); } catch (...) { continue; }
            lp.local_addr = local.substr(0, colon);

            size_t users = line.find("users:((");
            if (users != std::string::npos) {
                size_t name_start = users + 8;
                size_t name_end = line.find('"', name_start);
                if (name_end != std::string::npos) {
                    lp.process = line.substr(name_start, name_end - name_start);
                }
            }

            if (lp.process.empty()) {
                lp.process = findProcessForPort(lp.port);
            }

            result.push_back(std::move(lp));
        }
    }

    std::sort(result.begin(), result.end(),
              [](const ListeningPort& a, const ListeningPort& b) { return a.port < b.port; });

    return result;
}

}
