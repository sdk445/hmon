#include "metrics/sensors_fallback.hpp"

#include <chrono>
#include <sstream>
#include <string>

#include "metrics/linux_utils.hpp"

namespace {
bool lineHasToken(const std::string& lower_text, const std::string& token) {
  return lower_text.find(token) != std::string::npos;
}

bool hwmonNameLooksCpuSensor(const std::string& lower_name) {
  return lower_name.find("k10temp") != std::string::npos ||
         lower_name.find("coretemp") != std::string::npos ||
         lower_name.find("zenpower") != std::string::npos ||
         lower_name.find("cpu") != std::string::npos;
}

bool hwmonNameLooksBoardSensor(const std::string& lower_name) {
  return linux_utils::startsWith(lower_name, "nct") || linux_utils::startsWith(lower_name, "it") ||
         linux_utils::startsWith(lower_name, "f718") || linux_utils::startsWith(lower_name, "w83") ||
         lower_name.find("asus") != std::string::npos || lower_name.find("gigabyte") != std::string::npos;
}

bool hwmonNameLooksGpuSensor(const std::string& lower_name) {
  return lower_name.find("amdgpu") != std::string::npos || lower_name.find("nvidia") != std::string::npos ||
         lower_name.find("nouveau") != std::string::npos || lower_name.find("radeon") != std::string::npos;
}
}  // namespace

SensorsFallbackMetrics collectSensorsFallbackMetrics() {
  struct Cache {
    std::chrono::steady_clock::time_point last_read{};
    SensorsFallbackMetrics data;
    bool initialized = false;
  };
  static Cache cache;

  const auto now = std::chrono::steady_clock::now();
  if (cache.initialized &&
      std::chrono::duration_cast<std::chrono::milliseconds>(now - cache.last_read).count() < 900) {
    return cache.data;
  }

  SensorsFallbackMetrics result;
  int best_cpu_fan_score = -1;
  int best_cpu_power_score = -1;
  int best_gpu_fan_score = -1;
  int best_gpu_power_score = -1;

  const std::string output = linux_utils::runCommand("sensors 2>/dev/null");
  std::string chip_name;
  std::stringstream stream(output);
  std::string raw_line;

  while (std::getline(stream, raw_line)) {
    const std::string line = linux_utils::trim(raw_line);
    if (line.empty()) {
      continue;
    }

    const bool is_chip_header = !raw_line.empty() && raw_line[0] != ' ' && raw_line[0] != '\t' &&
                                line.find(':') == std::string::npos;
    if (is_chip_header) {
      chip_name = linux_utils::toLower(line);
      continue;
    }

    const std::string lower_line = linux_utils::toLower(line);

    if (lineHasToken(lower_line, "rpm")) {
      const auto rpm = linux_utils::extractFirstNumber(lower_line);
      if (rpm && *rpm > 0.0) {
        int cpu_score = 0;
        int gpu_score = 0;

        if (lineHasToken(lower_line, "cpu")) {
          cpu_score += 120;
        }
        if (lineHasToken(lower_line, "fan")) {
          cpu_score += 10;
          gpu_score += 10;
        }
        if (lineHasToken(lower_line, "pump")) {
          cpu_score -= 40;
        }
        if (lineHasToken(lower_line, "gpu")) {
          gpu_score += 120;
        }
        if (hwmonNameLooksGpuSensor(chip_name)) {
          gpu_score += 60;
        }
        if (hwmonNameLooksBoardSensor(chip_name)) {
          cpu_score += 25;
        }
        if (hwmonNameLooksCpuSensor(chip_name)) {
          cpu_score += 20;
        }

        if (cpu_score > best_cpu_fan_score) {
          best_cpu_fan_score = cpu_score;
          result.cpu_fan_rpm = *rpm;
        }
        if (gpu_score > best_gpu_fan_score) {
          best_gpu_fan_score = gpu_score;
          result.gpu_fan_rpm = *rpm;
        }
      }
    }

    const auto watts = linux_utils::extractWattsFromText(lower_line);
    if (watts) {
      int cpu_score = 0;
      int gpu_score = 0;

      if (lineHasToken(lower_line, "cpu") || lineHasToken(lower_line, "package") ||
          lineHasToken(lower_line, "ppt") || lineHasToken(lower_line, "svi2") ||
          lineHasToken(lower_line, "socket")) {
        cpu_score += 120;
      }
      if (lineHasToken(chip_name, "k10temp") || lineHasToken(chip_name, "coretemp") ||
          lineHasToken(chip_name, "zenpower") || lineHasToken(chip_name, "fam15h_power") ||
          lineHasToken(chip_name, "rapl")) {
        cpu_score += 60;
      }

      if (lineHasToken(lower_line, "gpu")) {
        gpu_score += 120;
      }
      if (hwmonNameLooksGpuSensor(chip_name)) {
        gpu_score += 60;
      }

      if (cpu_score > best_cpu_power_score) {
        best_cpu_power_score = cpu_score;
        result.cpu_power_w = *watts;
      }
      if (gpu_score > best_gpu_power_score) {
        best_gpu_power_score = gpu_score;
        result.gpu_power_w = *watts;
      }
    }
  }

  cache.last_read = now;
  cache.data = result;
  cache.initialized = true;
  return result;
}
