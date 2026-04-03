#include "process_collector.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace {

static long getPageSize() {
    return sysconf(_SC_PAGESIZE);
}

static long getClockTicks() {
    return sysconf(_SC_CLK_TCK);
}

static long readTotalMemKb() {
    std::ifstream f("/proc/meminfo");
    if (!f) return 0;
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("MemTotal:", 0) == 0) {
            long val = 0;
            std::istringstream iss(line.substr(9));
            if (iss >> val) return val;
        }
    }
    return 0;
}

static bool isPidDir(const char* name) {
    if (*name == '\0' || *name == '.') return false;
    for (const char* p = name; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
    }
    return true;
}

static std::string readCmdline(int pid) {
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%d/comm", pid);
    std::ifstream comm(path);
    if (comm) {
        std::string line;
        std::getline(comm, line);
        if (!line.empty()) return line;
    }

    std::snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";

    std::string cmdline;
    char buf[512];
    while (f.read(buf, sizeof(buf))) {
        cmdline.append(buf, static_cast<size_t>(f.gcount()));
    }
    cmdline.append(buf, static_cast<size_t>(f.gcount()));

    if (cmdline.empty()) return "";

    for (char& c : cmdline) {
        if (c == '\0') c = ' ';
    }
    if (!cmdline.empty() && cmdline.back() == ' ') cmdline.pop_back();

    size_t slash = cmdline.rfind('/');
    if (slash != std::string::npos) {
        cmdline = cmdline.substr(slash + 1);
    }
    if (cmdline.size() > 150) cmdline = cmdline.substr(0, 147) + "...";
    return cmdline;
}

static std::string trim(const std::string& s) {
    const auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    const auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static std::string runCommand(const char* cmd) {
    std::string result;
    FILE* pipe = popen(cmd, "r");
    if (!pipe) return result;

    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result += buffer;
    }
    pclose(pipe);
    return result;
}

static std::unordered_map<int, double> readGpuUsageByPid() {
    std::unordered_map<int, double> usage_by_pid;
    const std::string output = runCommand("nvidia-smi pmon -c 1 2>/dev/null");
    if (output.empty()) return usage_by_pid;

    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;

        std::istringstream iss(line);
        int gpu = 0;
        int pid = 0;
        std::string type;
        std::string sm_util;
        std::string mem_util;
        std::string enc_util;
        std::string dec_util;
        std::string command;
        if (!(iss >> gpu >> pid >> type >> sm_util >> mem_util >> enc_util >> dec_util >> command)) {
            continue;
        }
        if (pid <= 0 || sm_util == "-") continue;

        try {
            usage_by_pid[pid] = std::stod(sm_util);
        } catch (...) {
        }
    }
    return usage_by_pid;
}

struct ProcInfo {
    int pid;
    uint64_t utime;
    uint64_t stime;
    uint64_t rss_pages;
    std::string command;
};

static std::vector<ProcInfo> readAllProcesses() {
    std::vector<ProcInfo> result;
    DIR* dir = opendir("/proc");
    if (!dir) return result;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (!isPidDir(entry->d_name)) continue;

        int pid = 0;
        try { pid = std::stoi(entry->d_name); } catch (...) { continue; }
        if (pid <= 0) continue;

        char stat_path[64];
        std::snprintf(stat_path, sizeof(stat_path), "/proc/%d/stat", pid);
        std::ifstream sf(stat_path);
        if (!sf) continue;
        std::string stat_line;
        if (!std::getline(sf, stat_line)) continue;
        sf.close();

        size_t rp = stat_line.rfind(')');
        if (rp == std::string::npos || rp + 2 >= stat_line.size()) continue;

        std::istringstream iss(stat_line.substr(rp + 2));
        char state;
        int ppid, pgrp, session, tty_nr, tpgid;
        unsigned int flags;
        unsigned long minflt, cminflt, majflt, cmajflt;
        uint64_t utime = 0, stime = 0;
        long cutime, cstime, priority, nice, num_threads, itrealvalue;
        unsigned long long starttime;
        unsigned long long vsize = 0;
        long rss = 0;
        iss >> state >> ppid >> pgrp >> session >> tty_nr >> tpgid
            >> flags >> minflt >> cminflt >> majflt >> cmajflt
            >> utime >> stime >> cutime >> cstime >> priority >> nice
            >> num_threads >> itrealvalue >> starttime >> vsize >> rss;

        if (rss <= 0) continue;

        std::string cmd = readCmdline(pid);
        if (cmd.empty() || cmd[0] == '[') continue;

        ProcInfo pi;
        pi.pid = pid;
        pi.utime = utime;
        pi.stime = stime;
        pi.rss_pages = static_cast<uint64_t>(rss);
        pi.command = std::move(cmd);
        result.push_back(std::move(pi));
    }
    closedir(dir);
    return result;
}

}

namespace hmon::plugins::process {

std::vector<ProcessEntry> collectTopProcesses(ProcessPluginCtx* ctx, size_t limit, SortMode sort_mode, int lock_pid) {
    std::vector<ProcessEntry> result;
    if (limit == 0) return result;

    long page_size = getPageSize();
    long clock_ticks = getClockTicks();
    if (clock_ticks <= 0) clock_ticks = 100;
    long total_mem = ctx ? ctx->total_mem_kb : 0;
    if (total_mem <= 0) total_mem = readTotalMemKb();
    if (ctx) ctx->total_mem_kb = total_mem;

    auto procs = readAllProcesses();

    if (lock_pid > 0) {
        auto it = std::find_if(procs.begin(), procs.end(),
                               [lock_pid](const ProcInfo& p) { return p.pid == lock_pid; });
        if (it == procs.end()) return result;

        ProcessEntry e;
        e.pid = it->pid;
        e.command = it->command;
        long rss_kb = static_cast<long>(it->rss_pages) * page_size / 1024;
        e.mem_percent = total_mem > 0 ? 100.0 * static_cast<double>(rss_kb) / static_cast<double>(total_mem) : 0.0;
        if (ctx) {
            auto gpu_it = ctx->gpu_percent_by_pid.find(it->pid);
            if (gpu_it != ctx->gpu_percent_by_pid.end()) {
                e.gpu_percent = gpu_it->second;
            }
        }

        if (ctx && !ctx->prev_utime.empty()) {
            auto ut = ctx->prev_utime.find(it->pid);
            auto st = ctx->prev_stime.find(it->pid);
            if (ut != ctx->prev_utime.end() && st != ctx->prev_stime.end()) {
                uint64_t cpu_delta = (it->utime - ut->second) + (it->stime - st->second);
                auto now = std::chrono::steady_clock::now();
                double elapsed_s = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - ctx->prev_time).count() / 1000.0;
                if (elapsed_s > 0) {
                    e.cpu_percent = 100.0 * static_cast<double>(cpu_delta) / static_cast<double>(clock_ticks) / elapsed_s;
                }
            }
        }
        e.cpu_percent = std::max(0.0, std::min(100.0, e.cpu_percent));

        result.push_back(std::move(e));
        if (ctx) {
            ctx->prev_utime.clear();
            ctx->prev_stime.clear();
            ctx->prev_utime[it->pid] = it->utime;
            ctx->prev_stime[it->pid] = it->stime;
            ctx->prev_time = std::chrono::steady_clock::now();
        }
        return result;
    }

    if (ctx && !ctx->prev_utime.empty()) {
        auto now = std::chrono::steady_clock::now();
        double elapsed_s = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - ctx->prev_time).count() / 1000.0;
        double cpu_factor = (elapsed_s > 0) ? 100.0 / static_cast<double>(clock_ticks) / elapsed_s : 0.0;

        for (const auto& pi : procs) {
            ProcessEntry e;
            e.pid = pi.pid;
            e.command = pi.command;
            long rss_kb = static_cast<long>(pi.rss_pages) * page_size / 1024;
            e.mem_percent = total_mem > 0 ? 100.0 * static_cast<double>(rss_kb) / static_cast<double>(total_mem) : 0.0;
            auto gpu_it = ctx->gpu_percent_by_pid.find(pi.pid);
            if (gpu_it != ctx->gpu_percent_by_pid.end()) {
                e.gpu_percent = gpu_it->second;
            }

            auto ut = ctx->prev_utime.find(pi.pid);
            auto st = ctx->prev_stime.find(pi.pid);
            if (ut != ctx->prev_utime.end() && st != ctx->prev_stime.end()) {
                uint64_t cpu_delta = (pi.utime - ut->second) + (pi.stime - st->second);
                e.cpu_percent = cpu_factor * static_cast<double>(cpu_delta);
            }
            e.cpu_percent = std::max(0.0, std::min(100.0, e.cpu_percent));
            result.push_back(std::move(e));
        }
    } else {
        for (const auto& pi : procs) {
            ProcessEntry e;
            e.pid = pi.pid;
            e.command = pi.command;
            long rss_kb = static_cast<long>(pi.rss_pages) * page_size / 1024;
            e.mem_percent = total_mem > 0 ? 100.0 * static_cast<double>(rss_kb) / static_cast<double>(total_mem) : 0.0;
            if (ctx) {
                auto gpu_it = ctx->gpu_percent_by_pid.find(pi.pid);
                if (gpu_it != ctx->gpu_percent_by_pid.end()) {
                    e.gpu_percent = gpu_it->second;
                }
            }
            e.cpu_percent = 0.0;
            result.push_back(std::move(e));
        }
    }

    switch (sort_mode) {
        case SortMode::kGpu:
            std::stable_sort(result.begin(), result.end(),
                             [](const ProcessEntry& a, const ProcessEntry& b) { return a.gpu_percent > b.gpu_percent; });
            break;
        case SortMode::kMem:
            std::stable_sort(result.begin(), result.end(),
                             [](const ProcessEntry& a, const ProcessEntry& b) { return a.mem_percent > b.mem_percent; });
            break;
        case SortMode::kPid:
            std::stable_sort(result.begin(), result.end(),
                             [](const ProcessEntry& a, const ProcessEntry& b) { return a.pid < b.pid; });
            break;
        default:
            std::stable_sort(result.begin(), result.end(),
                             [](const ProcessEntry& a, const ProcessEntry& b) { return a.cpu_percent > b.cpu_percent; });
            break;
    }

    if (result.size() > limit) result.resize(limit);

    if (ctx) {
        ctx->prev_utime.clear();
        ctx->prev_stime.clear();
        ctx->gpu_percent_by_pid = readGpuUsageByPid();
        for (const auto& pi : procs) {
            ctx->prev_utime[pi.pid] = pi.utime;
            ctx->prev_stime[pi.pid] = pi.stime;
        }
        ctx->prev_time = std::chrono::steady_clock::now();
    }
    return result;
}

}
