#include "process_collector.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

std::string trim(const std::string& s) {
    auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string trimCommand(const std::string& cmd, size_t max = 50) {
    if (cmd.size() <= max) return cmd;
    if (max <= 3) return cmd.substr(0, max);
    return cmd.substr(0, max - 3) + "...";
}

std::unordered_map<int, double> collectGpuProcessUsage() {
    std::unordered_map<int, double> result;
    FILE* pipe = popen("nvidia-smi pmon -c 1 2>/dev/null | tail -n +3 | awk '{print $2, $4}'", "r");
    if (!pipe) return result;

    char buf[256];
    while (fgets(buf, sizeof(buf), pipe)) {
        int pid = 0;
        std::string sm;
        std::istringstream iss(buf);
        if (!(iss >> pid >> sm) || pid <= 0 || sm == "-") continue;
        try { result[pid] = std::stod(sm); } catch (...) {}
    }
    pclose(pipe);
    return result;
}

std::vector<hmon::plugins::process::ProcessEntry> collectFromPs(const std::string& cmd) {
    std::vector<hmon::plugins::process::ProcessEntry> result;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return result;

    char buf[512];
    while (fgets(buf, sizeof(buf), pipe)) {
        hmon::plugins::process::ProcessEntry e;
        std::istringstream iss(buf);
        if (!(iss >> e.pid >> e.cpu_percent >> e.mem_percent)) continue;
        std::getline(iss >> std::ws, e.command);
        e.command = trimCommand(e.command);
        result.push_back(std::move(e));
    }
    pclose(pipe);
    return result;
}

void attachGpu(std::vector<hmon::plugins::process::ProcessEntry>* procs,
               const std::unordered_map<int, double>& gpu_usage) {
    if (!procs) return;
    for (auto& p : *procs) {
        auto it = gpu_usage.find(p.pid);
        if (it != gpu_usage.end()) p.gpu_percent = it->second;
    }
}

}

namespace hmon::plugins::process {

std::vector<ProcessEntry> collectTopProcesses(size_t limit, SortMode sort_mode, int lock_pid) {
    std::vector<ProcessEntry> result;
    if (limit == 0) return result;

    auto gpu_usage = collectGpuProcessUsage();

    if (lock_pid > 0) {
        result = collectFromPs("ps -o pid,%cpu,%mem,args -p " + std::to_string(lock_pid) + " --no-headers 2>/dev/null");
        attachGpu(&result, gpu_usage);
        return result;
    }

    if (sort_mode == SortMode::kGpu && !gpu_usage.empty()) {
        std::string pid_list;
        for (const auto& [pid, _] : gpu_usage) {
            if (!pid_list.empty()) pid_list += ",";
            pid_list += std::to_string(pid);
        }
        result = collectFromPs("ps -p " + pid_list + " -o pid,%cpu,%mem,args --no-headers 2>/dev/null");
        attachGpu(&result, gpu_usage);
        std::stable_sort(result.begin(), result.end(), [](const ProcessEntry& a, const ProcessEntry& b) {
            if (a.gpu_percent != b.gpu_percent) return a.gpu_percent > b.gpu_percent;
            if (a.cpu_percent != b.cpu_percent) return a.cpu_percent > b.cpu_percent;
            return a.pid < b.pid;
        });
        if (result.size() > limit) result.resize(limit);
        return result;
    }

    std::string sort_key = "%cpu";
    switch (sort_mode) {
        case SortMode::kMem: sort_key = "%mem"; break;
        case SortMode::kPid: sort_key = "pid"; break;
        default: sort_key = "%cpu"; break;
    }

    result = collectFromPs(
        "ps -eo pid,%cpu,%mem,args --sort=-" + sort_key + " --no-headers 2>/dev/null | head -" +
        std::to_string(limit));
    attachGpu(&result, gpu_usage);
    return result;
}

} /* namespace hmon::plugins::process */
