#pragma once

#include <filesystem>
#include <fstream>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace hmon::plugins::gpu {

struct GpuPluginCtx {};

struct GpuInfo {
    std::string name;
    std::string source;
    std::optional<bool> in_use;
    std::optional<double> temperature_c;
    std::optional<double> core_clock_mhz;
    std::optional<double> utilization_percent;
    std::optional<double> power_w;
    std::optional<double> memory_used_mib;
    std::optional<double> memory_total_mib;
    std::optional<double> memory_utilization_percent;
    std::vector<double> gpu_core_usage_percent;
};

std::vector<GpuInfo> collectGpus();

}
