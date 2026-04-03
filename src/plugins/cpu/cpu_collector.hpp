#pragma once

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numeric>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace hmon::plugins::cpu {

struct CpuPluginCtx {
    unsigned long long prev_idle = 0;
    unsigned long long prev_total = 0;
    bool cpu_usage_initialized = false;

    struct CoreState {
        unsigned long long user = 0;
        unsigned long long nice = 0;
        unsigned long long system = 0;
        unsigned long long idle = 0;
        unsigned long long iowait = 0;
        unsigned long long irq = 0;
        unsigned long long softirq = 0;
        unsigned long long steal = 0;
        bool initialized = false;
    };
    std::vector<CoreState> core_states;
};

std::string collectName();
std::optional<int> collectCoreCount();
std::optional<int> collectThreadCount();
std::optional<double> collectTemperature();
std::optional<double> collectFrequency();
std::optional<double> collectUsagePercent(CpuPluginCtx* ctx);
std::vector<double> collectPerCoreUsagePercent(CpuPluginCtx* ctx);

}
