#include "system_collector.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ifaddrs.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <netinet/in.h>
#include <optional>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::string trim(const std::string& s) {
    auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::optional<unsigned long long> readULL(const fs::path& p) {
    std::ifstream f(p);
    if (!f) return std::nullopt;
    unsigned long long v;
    if (f >> v) return v;
    return std::nullopt;
}

std::optional<std::string> readFirstLine(const fs::path& p) {
    std::ifstream f(p);
    if (!f) return std::nullopt;
    std::string line;
    if (std::getline(f, line)) return trim(line);
    return std::nullopt;
}

bool isCpuDir(const std::string& name) {
    if (name.size() <= 3 || name.rfind("cpu", 0) != 0) return false;
    return std::all_of(name.begin() + 3, name.end(),
                       [](unsigned char c) { return std::isdigit(c) != 0; });
}

}

namespace hmon::plugins::system {

std::optional<long long> collectRamTotalKb() {
    std::ifstream f("/proc/meminfo");
    if (!f) return std::nullopt;
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("MemTotal:", 0) == 0) {
            std::istringstream iss(line.substr(9));
            long long kb;
            if (iss >> kb) return kb;
        }
    }
    return std::nullopt;
}

std::optional<long long> collectRamAvailableKb() {
    std::ifstream f("/proc/meminfo");
    if (!f) return std::nullopt;
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("MemAvailable:", 0) == 0) {
            std::istringstream iss(line.substr(13));
            long long kb;
            if (iss >> kb) return kb;
        }
    }
    return std::nullopt;
}

std::optional<double> getSwapUsagePercent() {
    std::ifstream f("/proc/meminfo");
    if (!f) return std::nullopt;
    std::string line;
    long long total = 0, free = 0;
    bool has_total = false, has_free = false;
    while (std::getline(f, line)) {
        if (line.rfind("SwapTotal:", 0) == 0) {
            std::istringstream iss(line.substr(10));
            if (iss >> total) has_total = true;
        } else if (line.rfind("SwapFree:", 0) == 0) {
            std::istringstream iss(line.substr(9));
            if (iss >> free) has_free = true;
        }
    }
    if (!has_total || total <= 0) return std::nullopt;
    long long used = total - free;
    return 100.0 * static_cast<double>(used) / static_cast<double>(total);
}

std::optional<long long> getSwapTotalKb() {
    std::ifstream f("/proc/meminfo");
    if (!f) return std::nullopt;
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("SwapTotal:", 0) == 0) {
            std::istringstream iss(line.substr(10));
            long long total;
            if (iss >> total) return total;
        }
    }
    return std::nullopt;
}

std::optional<long long> getSwapFreeKb() {
    std::ifstream f("/proc/meminfo");
    if (!f) return std::nullopt;
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("SwapFree:", 0) == 0) {
            std::istringstream iss(line.substr(9));
            long long free;
            if (iss >> free) return free;
        }
    }
    return std::nullopt;
}

std::string detectRootDevice() {
    std::ifstream mounts("/proc/self/mounts");
    if (!mounts) return "sda";
    std::string line;
    while (std::getline(mounts, line)) {
        std::istringstream iss(line);
        std::string dev, mp, fstype;
        if (iss >> dev >> mp >> fstype && mp == "/") {
            std::string d = dev;
            size_t slash = d.rfind('/');
            if (slash != std::string::npos) d = d.substr(slash + 1);
            if (d.find("nvme") != std::string::npos) {
                size_t p = d.find('p');
                if (p != std::string::npos) d = d.substr(0, p);
            } else {
                size_t end = d.find_first_of("0123456789");
                if (end != std::string::npos) d = d.substr(0, end);
            }
            return d;
        }
    }
    return "sda";
}

std::optional<unsigned long long> collectDiskTotalBytes(const std::string& mount) {
    struct statvfs st;
    if (statvfs(mount.c_str(), &st) != 0) return std::nullopt;
    return static_cast<unsigned long long>(st.f_frsize) * st.f_blocks;
}

std::optional<unsigned long long> collectDiskFreeBytes(const std::string& mount) {
    struct statvfs st;
    if (statvfs(mount.c_str(), &st) != 0) return std::nullopt;
    return static_cast<unsigned long long>(st.f_frsize) * st.f_bfree;
}

std::string detectActiveInterface() {
    struct ifaddrs* addrs = nullptr;
    if (getifaddrs(&addrs) != 0) return "";

    std::string best;
    for (struct ifaddrs* ifa = addrs; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) continue;
        if (ifa->ifa_addr->sa_family != AF_INET) continue;
        std::string name = ifa->ifa_name;
        if (name == "lo") continue;
        if (name.find("docker") == 0 || name.find("veth") == 0) continue;
        if (best.empty() || name.rfind("eth", 0) == 0 || name.rfind("en", 0) == 0) {
            best = name;
        }
    }
    freeifaddrs(addrs);
    return best;
}

std::optional<double> collectRxKbps(SystemPluginCtx* ctx) {
    struct ifaddrs* addrs = nullptr;
    if (getifaddrs(&addrs) != 0) return std::nullopt;

    unsigned long long rx_bytes = 0;
    bool found = false;

    if (ctx->active_interface.empty()) {
        ctx->active_interface = detectActiveInterface();
    }

    for (struct ifaddrs* ifa = addrs; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_name || std::string(ifa->ifa_name) != ctx->active_interface) continue;
        if (ifa->ifa_flags & IFF_LOOPBACK) continue;
        if (ifa->ifa_flags & IFF_RUNNING) {
            rx_bytes = ifa->ifa_data ? reinterpret_cast<struct rtnl_link_stats*>(ifa->ifa_data)->rx_bytes : 0;
            found = true;
        }
        break;
    }
    freeifaddrs(addrs);

    if (!found) {
        /* Fallback: parse /proc/net/dev */
        std::ifstream f("/proc/net/dev");
        if (!f) return std::nullopt;
        std::string line;
        while (std::getline(f, line)) {
            if (line.find(ctx->active_interface + ":") == std::string::npos) continue;
            std::istringstream iss(line.substr(line.find(':') + 1));
            if (iss >> rx_bytes) found = true;
            break;
        }
    }

    if (!found) return std::nullopt;

    auto now = std::chrono::steady_clock::now();
    if (!ctx->net_initialized) {
        ctx->prev_rx_bytes = rx_bytes;
        ctx->prev_net_time = now;
        ctx->net_initialized = true;
        return 0.0;
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - ctx->prev_net_time).count();
    if (elapsed <= 0) return 0.0;

    double delta = static_cast<double>(rx_bytes - ctx->prev_rx_bytes);
    ctx->prev_rx_bytes = rx_bytes;
    ctx->prev_net_time = now;

    return (delta / 1024.0) / (static_cast<double>(elapsed) / 1000.0);
}

std::optional<double> collectTxKbps(SystemPluginCtx* ctx) {
    struct ifaddrs* addrs = nullptr;
    if (getifaddrs(&addrs) != 0) return std::nullopt;

    unsigned long long tx_bytes = 0;
    bool found = false;

    if (ctx->active_interface.empty()) {
        ctx->active_interface = detectActiveInterface();
    }

    for (struct ifaddrs* ifa = addrs; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_name || std::string(ifa->ifa_name) != ctx->active_interface) continue;
        if (ifa->ifa_flags & IFF_LOOPBACK) continue;
        if (ifa->ifa_flags & IFF_RUNNING) {
            tx_bytes = ifa->ifa_data ? reinterpret_cast<struct rtnl_link_stats*>(ifa->ifa_data)->tx_bytes : 0;
            found = true;
        }
        break;
    }
    freeifaddrs(addrs);

    if (!found) {
        std::ifstream f("/proc/net/dev");
        if (!f) return std::nullopt;
        std::string line;
        while (std::getline(f, line)) {
            if (line.find(ctx->active_interface + ":") == std::string::npos) continue;
            std::istringstream iss(line.substr(line.find(':') + 1));
            unsigned long long rx;
            if (iss >> rx >> rx >> rx >> rx >> rx >> rx >> rx >> rx >> tx_bytes) found = true;
            break;
        }
    }

    if (!found) return std::nullopt;

    auto now = std::chrono::steady_clock::now();
    if (!ctx->net_initialized) {
        ctx->prev_tx_bytes = tx_bytes;
        ctx->prev_net_time = now;
        ctx->net_initialized = true;
        return 0.0;
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - ctx->prev_net_time).count();
    if (elapsed <= 0) return 0.0;

    double delta = static_cast<double>(tx_bytes - ctx->prev_tx_bytes);
    ctx->prev_tx_bytes = tx_bytes;
    ctx->prev_net_time = now;

    return (delta / 1024.0) / (static_cast<double>(elapsed) / 1000.0);
}

std::string currentTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    struct tm tm_buf;
    localtime_r(&t, &tm_buf);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm_buf);
    return buf;
}

std::string hostName() {
    struct utsname u;
    if (uname(&u) == 0) return u.nodename;
    return "unknown";
}

std::string humanBytes(unsigned long long bytes) {
    const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    int level = 0;
    double value = static_cast<double>(bytes);
    while (value >= 1024.0 && level < 4) {
        value /= 1024.0;
        ++level;
    }
    char buf[64];
    if (level == 0) {
        std::snprintf(buf, sizeof(buf), "%llu %s", bytes, units[level]);
    } else {
        std::snprintf(buf, sizeof(buf), "%.2f %s", value, units[level]);
    }
    return buf;
}

} /* namespace hmon::plugins::system */
