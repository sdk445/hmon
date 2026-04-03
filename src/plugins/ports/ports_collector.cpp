#include "ports_collector.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <limits.h>
#include <pwd.h>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

struct SocketEntry {
    uint16_t port;
    std::string proto;
    std::string local_addr;
    uint64_t inode;
    uint32_t uid;
};

static std::string decodeIpv4(const std::string& hex_addr) {
    if (hex_addr.size() != 8) return "";
    unsigned int a, b, c, d;
    if (sscanf(hex_addr.c_str(), "%02x%02x%02x%02x", &a, &b, &c, &d) != 4) return "";
    return std::to_string(a) + "." + std::to_string(b) + "." + std::to_string(c) + "." + std::to_string(d);
}

static std::string decodeIpv6(const std::string& hex_addr) {
    if (hex_addr.size() != 32) return "";
    char buf[INET6_ADDRSTRLEN];
    struct in6_addr addr6;
    for (int i = 0; i < 4; ++i) {
        std::string chunk = hex_addr.substr(i * 8, 8);
        unsigned int part;
        if (sscanf(chunk.c_str(), "%08x", &part) != 1) return "";
        reinterpret_cast<uint32_t*>(&addr6.s6_addr)[i] = htonl(part);
    }
    if (inet_ntop(AF_INET6, &addr6, buf, sizeof(buf))) return buf;
    return "";
}

static uint16_t decodePort(const std::string& hex_port) {
    unsigned int port = 0;
    if (sscanf(hex_port.c_str(), "%04X", &port) != 1) return 0;
    return static_cast<uint16_t>(port);
}

static std::vector<SocketEntry> parseProcNetFile(const std::string& path, const std::string& proto) {
    std::vector<SocketEntry> result;
    std::ifstream f(path);
    if (!f) return result;
    std::string line;
    std::getline(f, line);
    while (std::getline(f, line)) {
        std::istringstream iss(line);
        std::string sl, local, remote, st, txrx, trtm, retrnsmt, uid_str, timeout;
        std::string inode_str;
        iss >> sl >> local >> remote >> st >> txrx >> trtm >> retrnsmt >> uid_str >> timeout >> inode_str;
        if (proto == "tcp" && st != "0A") continue;
        if (proto == "tcp6" && st != "0A") continue;
        if (proto == "udp" && st != "07") continue;
        if (proto == "udp6" && st != "07") continue;
        size_t colon = local.rfind(':');
        if (colon == std::string::npos) continue;
        std::string addr_hex = local.substr(0, colon);
        std::string port_hex = local.substr(colon + 1);
        SocketEntry se;
        se.port = decodePort(port_hex);
        if (se.port == 0) continue;
        se.proto = proto;
        if (addr_hex.size() == 8) {
            se.local_addr = decodeIpv4(addr_hex);
            se.proto = (proto == "tcp6" || proto == "tcp") ? "tcp" : "udp";
        } else {
            se.local_addr = decodeIpv6(addr_hex);
        }
        try { se.inode = std::stoull(inode_str); } catch (...) { se.inode = 0; }
        try { se.uid = static_cast<uint32_t>(std::stoul(uid_str)); } catch (...) { se.uid = 0; }
        if (!se.local_addr.empty()) result.push_back(std::move(se));
    }
    return result;
}

static std::unordered_map<uint64_t, std::string> buildInodeMap() {
    std::unordered_map<uint64_t, std::string> result;
    DIR* proc = opendir("/proc");
    if (!proc) return result;

    struct dirent* entry;
    while ((entry = readdir(proc)) != nullptr) {
        if (entry->d_type != DT_DIR) continue;
        bool is_pid = true;
        for (const char* p = entry->d_name; *p; ++p) {
            if (*p < '0' || *p > '9') { is_pid = false; break; }
        }
        if (!is_pid || entry->d_name[0] == '\0') continue;

        int pid = std::stoi(entry->d_name);
        char fd_path[256];
        std::snprintf(fd_path, sizeof(fd_path), "/proc/%d/fd", pid);
        DIR* fd_dir = opendir(fd_path);
        if (!fd_dir) continue;

        std::string process_name;
        struct dirent* fd_entry;
        while ((fd_entry = readdir(fd_dir)) != nullptr) {
            if (fd_entry->d_type != DT_LNK) continue;
            char link_path[PATH_MAX];
            std::snprintf(link_path, sizeof(link_path), "/proc/%d/fd/%s", pid, fd_entry->d_name);
            char target[PATH_MAX];
            ssize_t len = readlink(link_path, target, sizeof(target) - 1);
            if (len <= 0) continue;
            target[len] = '\0';
            if (std::strncmp(target, "socket:[", 8) != 0) continue;
            char* end_bracket = std::strchr(target + 8, ']');
            if (!end_bracket) continue;
            *end_bracket = '\0';
            uint64_t inode = 0;
            try { inode = std::stoull(target + 8); } catch (...) { continue; }
            if (inode == 0 || result.count(inode)) continue;

            if (process_name.empty()) {
                char comm_path[256];
                std::snprintf(comm_path, sizeof(comm_path), "/proc/%d/comm", pid);
                std::ifstream comm(comm_path);
                if (comm) std::getline(comm, process_name);
            }
            if (!process_name.empty()) {
                result[inode] = process_name;
            }
        }
        closedir(fd_dir);
    }
    closedir(proc);
    return result;
}

static std::string uidToName(uint32_t uid) {
    struct passwd* pw = getpwuid(uid);
    if (pw && pw->pw_name) return pw->pw_name;
    return std::to_string(uid);
}

}

namespace hmon::plugins::ports {

std::vector<ListeningPort> collectListeningPorts(PortsPluginCtx* ctx) {
    if (ctx) {
        auto elapsed = std::chrono::steady_clock::now() - ctx->last_cache_time;
        if (!ctx->cached_result.empty() &&
            std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() < PortsPluginCtx::TTL_SECONDS) {
            return ctx->cached_result;
        }
    }

    std::vector<ListeningPort> result;

    std::vector<std::pair<std::string, std::string>> files = {
        {"/proc/net/tcp", "tcp"},
        {"/proc/net/tcp6", "tcp6"},
        {"/proc/net/udp", "udp"},
        {"/proc/net/udp6", "udp6"}
    };

    std::vector<SocketEntry> all_sockets;
    for (const auto& [path, proto] : files) {
        auto entries = parseProcNetFile(path, proto);
        all_sockets.insert(all_sockets.end(), entries.begin(), entries.end());
    }

    auto inode_map = buildInodeMap();

    std::unordered_set<std::string> seen;
    for (auto& se : all_sockets) {
        std::string proto_base = (se.proto == "tcp" || se.proto == "tcp6") ? "tcp" : "udp";
        std::string key = std::to_string(se.port) + ":" + proto_base;
        if (seen.count(key)) continue;
        seen.insert(key);

        ListeningPort lp;
        lp.port = se.port;
        lp.proto = se.proto;
        lp.local_addr = se.local_addr;

        if (se.inode > 0) {
            auto it = inode_map.find(se.inode);
            if (it != inode_map.end()) {
                lp.process = it->second;
            }
        }

        if (lp.process.empty() && se.uid > 0) {
            lp.process = uidToName(se.uid);
        } else if (lp.process.empty() && se.uid == 0) {
            lp.process = "root";
        }

        result.push_back(std::move(lp));
    }

    // Attempt to resolve 'root' ports to docker containers
    std::unordered_map<uint16_t, std::string> docker_ports;
    FILE* pipe = popen("docker ps --format '{{.Names}}|{{.Ports}}' 2>/dev/null", "r");
    if (pipe) {
        char buf[256];
        while (fgets(buf, sizeof(buf), pipe)) {
            std::string line(buf);
            size_t bar = line.find('|');
            if (bar != std::string::npos) {
                std::string name = line.substr(0, bar);
                std::string ports = line.substr(bar + 1);
                std::istringstream iss(ports);
                std::string token;
                while (std::getline(iss, token, ',')) {
                    size_t arrow = token.find("->");
                    if (arrow != std::string::npos) {
                        size_t colon = token.rfind(':', arrow);
                        if (colon != std::string::npos) {
                            try {
                                uint16_t dport = static_cast<uint16_t>(std::stoi(token.substr(colon + 1, arrow - colon - 1)));
                                docker_ports[dport] = name;
                            } catch (...) {}
                        }
                    }
                }
            }
        }
        pclose(pipe);
    }

    for (auto& lp : result) {
        if (lp.process == "root" || lp.process.empty()) {
            if (docker_ports.count(lp.port)) {
                lp.process = docker_ports[lp.port];
            }
        }
    }

    std::sort(result.begin(), result.end(),
              [](const ListeningPort& a, const ListeningPort& b) { return a.port < b.port; });

    if (ctx) {
        ctx->cached_result = result;
        ctx->last_cache_time = std::chrono::steady_clock::now();
    }
    return result;
}

}
