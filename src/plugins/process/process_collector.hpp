#pragma once

#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>

namespace hmon::plugins::process {

enum class SortMode { kCpu, kMem, kGpu, kPid };

struct ProcessEntry {
    int pid = 0;
    double cpu_percent = 0.0;
    double mem_percent = 0.0;
    double gpu_percent = 0.0;
    std::string command;
};

struct ProcessPluginCtx {
    SortMode sort_mode = SortMode::kCpu;
    int lock_pid = -1;
    std::unordered_map<int, uint64_t> prev_utime;
    std::unordered_map<int, uint64_t> prev_stime;
    std::unordered_map<int, double> gpu_percent_by_pid;
    std::chrono::steady_clock::time_point prev_time;
    long total_mem_kb = 0;
};

std::vector<ProcessEntry> collectTopProcesses(ProcessPluginCtx* ctx, size_t limit, SortMode sort_mode, int lock_pid);

}
