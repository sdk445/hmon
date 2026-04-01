#include "metrics/gpu_darwin.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>
#include <sstream>
#include <regex>

#include "src/metrics/darwin_utils.hpp"

namespace {

std::vector<GpuMetrics> collectAppleGPUInfo() {
  std::vector<GpuMetrics> gpus;
  
  const std::string output = darwin_utils::runCommand(
      "ioreg -r -c 'IOAccelerator' -w 0 2>/dev/null");
  
  GpuMetrics gpu;
  gpu.name = "Apple GPU";
  gpu.source = "IOKit";
  
  std::regex util_regex(R"(\"PerformanceStatistics\".*?\"GPU Utilization\".*?=\s*([0-9]+))");
  std::smatch match;
  if (std::regex_search(output, match, util_regex)) {
    double util = std::stod(match[1].str());
    gpu.utilization_percent = util;
    gpu.gpu_core_usage_percent = {util};
  }
  
  std::regex vram_regex(R"(\"vram\(total\|active\)\".*?=\s*([0-9]+))");
  if (std::regex_search(output, match, vram_regex)) {
    double vram_bytes = std::stod(match[1].str());
    gpu.memory_total_mib = vram_bytes / (1024.0 * 1024.0);
  }
  
  gpus.push_back(gpu);
  return gpus;
}

}

std::vector<GpuMetrics> collectGpus() {
  return collectAppleGPUInfo();
}
