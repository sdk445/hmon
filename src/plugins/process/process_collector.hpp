#pragma once

#include <string>
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
};

std::vector<ProcessEntry> collectTopProcesses(size_t limit, SortMode sort_mode, int lock_pid);

}
