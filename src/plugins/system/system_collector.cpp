#include "system_collector.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
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
    bool has_total = false;
    while (std::getline(f, line)) {
        if (line.rfind("SwapTotal:", 0) == 0) {
            std::istringstream iss(line.substr(10));
            if (iss >> total) has_total = true;
        } else if (line.rfind("SwapFree:", 0) == 0) {
            std::istringstream iss(line.substr(9));
            (void)(iss >> free);
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
    if (!ctx->rx_initialized) {
        ctx->prev_rx_bytes = rx_bytes;
        ctx->prev_rx_time = now;
        ctx->rx_initialized = true;
        return 0.0;
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - ctx->prev_rx_time).count();
    if (elapsed <= 0) return 0.0;

    double delta = static_cast<double>(rx_bytes - ctx->prev_rx_bytes);
    ctx->prev_rx_bytes = rx_bytes;
    ctx->prev_rx_time = now;

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
    if (!ctx->tx_initialized) {
        ctx->prev_tx_bytes = tx_bytes;
        ctx->prev_tx_time = now;
        ctx->tx_initialized = true;
        return 0.0;
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - ctx->prev_tx_time).count();
    if (elapsed <= 0) return 0.0;

    double delta = static_cast<double>(tx_bytes - ctx->prev_tx_bytes);
    ctx->prev_tx_bytes = tx_bytes;
    ctx->prev_tx_time = now;

    return (delta / 1024.0) / (static_cast<double>(elapsed) / 1000.0);
}

std::vector<DiskIoSample> parseDiskStats() {
    std::vector<DiskIoSample> result;
    std::ifstream f("/proc/diskstats");
    if (!f) return result;

    std::string line;
    while (std::getline(f, line)) {
        std::istringstream iss(line);
        unsigned int major, minor;
        std::string name;
        uint64_t rd_ios, rd_merges, rd_sectors, rd_ms;
        uint64_t wr_ios, wr_merges, wr_sectors, wr_ms;
        uint64_t io_in_progress, io_ms, weighted_io_ms;

        if (!(iss >> major >> minor >> name >> rd_ios >> rd_merges >> rd_sectors >> rd_ms
                    >> wr_ios >> wr_merges >> wr_sectors >> wr_ms
                    >> io_in_progress >> io_ms >> weighted_io_ms)) {
            continue;
        }

        DiskIoSample sample;
        sample.name = name;
        sample.sectors_read = rd_sectors;
        sample.sectors_written = wr_sectors;
        sample.read_ops = rd_ios;
        sample.write_ops = wr_ios;
        sample.io_ticks_ms = io_ms;
        result.push_back(std::move(sample));
    }
    return result;
}

std::vector<DiskIoSample> collectDiskIoDelta(SystemPluginCtx* ctx) {
    auto samples = parseDiskStats();
    auto now = std::chrono::steady_clock::now();

    if (!ctx->disk_io_initialized) {
        ctx->prev_disk_samples = samples;
        ctx->prev_disk_io_time = now;
        ctx->disk_io_initialized = true;
        return {};
    }

    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - ctx->prev_disk_io_time).count();
    if (elapsed_ms <= 0) return {};
    const double elapsed_sec = static_cast<double>(elapsed_ms) / 1000.0;

    std::vector<DiskIoSample> deltas;
    for (auto& cur : samples) {
        for (auto& prev : ctx->prev_disk_samples) {
            if (cur.name != prev.name) continue;

            double read_kb = static_cast<double>(cur.sectors_read - prev.sectors_read) * 512.0 / 1024.0;
            double write_kb = static_cast<double>(cur.sectors_written - prev.sectors_written) * 512.0 / 1024.0;
            uint64_t busy_ticks = cur.io_ticks_ms - prev.io_ticks_ms;
            double busy_pct = std::min(100.0, (static_cast<double>(busy_ticks) / elapsed_ms) * 100.0);
            double read_kbps = read_kb / elapsed_sec;
            double write_kbps = write_kb / elapsed_sec;

            DiskIoSample delta;
            delta.name = cur.name;
            delta.sectors_read = cur.sectors_read - prev.sectors_read;
            delta.sectors_written = cur.sectors_written - prev.sectors_written;
            delta.io_ticks_ms = busy_ticks;
            delta.read_ops = cur.read_ops - prev.read_ops;
            delta.write_ops = cur.write_ops - prev.write_ops;
            delta.read_kbps = read_kbps;
            delta.write_kbps = write_kbps;
            delta.busy_percent = busy_pct;
            deltas.push_back(std::move(delta));
            break;
        }
    }

    ctx->prev_disk_samples = std::move(samples);
    ctx->prev_disk_io_time = now;
    return deltas;
}

NetConnStats collectNetConnStats() {
    NetConnStats result;
    static const std::vector<std::string> paths = {
        "/proc/net/tcp", "/proc/net/tcp6"
    };

    for (const auto& path : paths) {
        std::ifstream f(path);
        if (!f) continue;
        std::string line;
        std::getline(f, line);
        while (std::getline(f, line)) {
            std::istringstream iss(line);
            std::string slot, local, remote, state_str;
            if (!(iss >> slot >> local >> remote >> state_str)) continue;
            unsigned long state = std::stoul(state_str, nullptr, 16);
            switch (state) {
                case 0x01: ++result.established; break;
                case 0x02: ++result.syn_sent; break;
                case 0x03: ++result.syn_recv; break;
                case 0x04: ++result.fin_wait1; break;
                case 0x05: ++result.fin_wait2; break;
                case 0x06: ++result.time_wait; break;
                case 0x07: ++result.close; break;
                case 0x08: ++result.close_wait; break;
                case 0x09: ++result.last_ack; break;
                case 0x0A: ++result.listen; break;
                case 0x0B: ++result.closing; break;
            }
        }
    }
    return result;
}

MemInfoDetailed collectMemInfoDetailed() {
    MemInfoDetailed result;
    std::ifstream f("/proc/meminfo");
    if (!f) return result;

    std::string line;
    while (std::getline(f, line)) {
        std::istringstream iss(line);
        std::string key;
        long long val;
        std::string unit;
        if (iss >> key >> val) {
            key.pop_back();
            if (key == "Buffers") result.buffers_kb = val;
            else if (key == "Cached") result.cached_kb = val;
            else if (key == "Shmem") result.shared_kb = val;
            else if (key == "SReclaimable") result.sreclaimable_kb = val;
            else if (key == "Slab") result.slab_kb = val;
            else if (key == "Active") result.active_kb = val;
            else if (key == "Inactive") result.inactive_kb = val;
            else if (key == "Dirty") result.dirty_kb = val;
            else if (key == "Writeback") result.writeback_kb = val;
            else if (key == "HugePages_Total") result.hugepages_total_kb = val;
            else if (key == "Mapped") result.mapped_kb = val;
            else if (key == "PageTables") result.page_tables_kb = val;
            else if (key == "NFS_Unstable") result.nfs_unstable_kb = val;
            else if (key == "Bounce") result.bounce_kb = val;
        }
    }
    return result;
}

SystemStats collectSystemStats() {
    SystemStats result;

    std::ifstream la("/proc/loadavg");
    if (la) {
        la >> result.load_avg_1 >> result.load_avg_5 >> result.load_avg_15
           >> result.procs_running >> result.procs_blocked;
    }

    std::ifstream up("/proc/uptime");
    if (up) {
        double uptime_f;
        if (up >> uptime_f) result.uptime_seconds = static_cast<int64_t>(uptime_f);
    }

    std::ifstream stat("/proc/stat");
    if (stat) {
        std::string line;
        while (std::getline(stat, line)) {
            std::istringstream iss(line);
            std::string key;
            if (!(iss >> key)) continue;
            if (key == "ctxt") { iss >> result.context_switches; }
            else if (key == "intr") {
                iss >> result.interrupts;
            }
            else if (key == "softirq") {
                iss >> result.softirqs;
            }
            else if (key == "processes") { iss >> result.forks; }
        }
    }

    std::ifstream fnr("/proc/sys/fs/file-nr");
    if (fnr) {
        fnr >> result.fd_open >> result.fd_max;
    }

    return result;
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
