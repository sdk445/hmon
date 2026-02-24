#include "metrics/gpu.hpp"

#include <algorithm>
#include <fstream>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "metrics/linux_utils.hpp"
#include "metrics/sensors_fallback.hpp"

namespace {
namespace fs = std::filesystem;

std::string vendorNameFromId(const std::optional<std::string>& vendor_id) {
  if (!vendor_id) {
    return "Unknown";
  }
  const std::string lower = linux_utils::toLower(*vendor_id);
  if (lower == "0x10de") {
    return "NVIDIA";
  }
  if (lower == "0x1002") {
    return "AMD";
  }
  if (lower == "0x8086") {
    return "Intel";
  }
  return "Vendor " + *vendor_id;
}

std::optional<double> readActiveAmdClockMhz(const fs::path& pp_dpm_sclk_file) {
  std::ifstream file(pp_dpm_sclk_file);
  if (!file) {
    return std::nullopt;
  }

  const std::regex pattern(R"(:\s*([0-9]+(?:\.[0-9]+)?)\s*[Mm][Hh][Zz].*\*)");
  std::string line;
  while (std::getline(file, line)) {
    std::smatch match;
    if (std::regex_search(line, match, pattern)) {
      try {
        return std::stod(match[1].str());
      } catch (...) {
        return std::nullopt;
      }
    }
  }
  return std::nullopt;
}

std::vector<GpuMetrics> collectGpusFromNvidiaSmi() {
  std::vector<GpuMetrics> gpus;
  const std::string command =
      "nvidia-smi --query-gpu=name,temperature.gpu,clocks.sm,utilization.gpu,power.draw,"
      "memory.used,memory.total "
      "--format=csv,noheader,nounits 2>/dev/null";
  const std::string output = linux_utils::runCommand(command);
  if (linux_utils::trim(output).empty()) {
    return gpus;
  }

  std::stringstream stream(output);
  std::string line;
  while (std::getline(stream, line)) {
    if (linux_utils::trim(line).empty()) {
      continue;
    }

    auto fields = linux_utils::splitByChar(line, ',');
    if (fields.size() < 7) {
      continue;
    }

    GpuMetrics gpu;
    gpu.name = fields[0];
    gpu.source = "nvidia-smi";
    gpu.temperature_c = linux_utils::parseOptionalDouble(fields[1]);
    gpu.core_clock_mhz = linux_utils::parseOptionalDouble(fields[2]);
    gpu.utilization_percent = linux_utils::parseOptionalDouble(fields[3]);
    gpu.power_w = linux_utils::parseOptionalDouble(fields[4]);
    gpu.memory_used_mib = linux_utils::parseOptionalDouble(fields[5]);
    gpu.memory_total_mib = linux_utils::parseOptionalDouble(fields[6]);
    if (gpu.memory_used_mib && gpu.memory_total_mib && *gpu.memory_total_mib > 0.0) {
      gpu.memory_utilization_percent = 100.0 * (*gpu.memory_used_mib) / (*gpu.memory_total_mib);
    }
    gpus.push_back(gpu);
  }

  return gpus;
}

std::vector<GpuMetrics> collectGpusFromSysfs() {
  std::vector<GpuMetrics> gpus;
  const fs::path drm_base("/sys/class/drm");
  if (!fs::exists(drm_base)) {
    return gpus;
  }

  for (const auto& card_path : linux_utils::listDirEntries(drm_base)) {
    const std::string card_name = card_path.filename().string();
    if (!linux_utils::startsWith(card_name, "card") || card_name.find('-') != std::string::npos) {
      continue;
    }

    const fs::path device_path = card_path / "device";
    if (!fs::exists(device_path)) {
      continue;
    }

    GpuMetrics gpu;
    gpu.source = "sysfs";
    gpu.name = card_name + " (" + vendorNameFromId(linux_utils::readFirstLine(device_path / "vendor")) + ")";

    const fs::path hwmon_path = device_path / "hwmon";
    if (fs::exists(hwmon_path)) {
      for (const auto& sensor_path : linux_utils::listDirEntries(hwmon_path)) {
        if (!fs::is_directory(sensor_path)) {
          continue;
        }

        if (!gpu.power_w) {
          gpu.power_w = linux_utils::readHwmonPowerWatts(sensor_path);
        }

        for (const auto& file_path : linux_utils::listDirEntries(sensor_path)) {
          const std::string filename = file_path.filename().string();

          if (!gpu.temperature_c && linux_utils::startsWith(filename, "temp") &&
              linux_utils::endsWith(filename, "_input")) {
            const auto milli_c = linux_utils::readLongLong(file_path);
            if (milli_c) {
              gpu.temperature_c = static_cast<double>(*milli_c) / 1000.0;
            }
          }
        }
      }
    }

    if (const auto mhz = linux_utils::readLongLong(device_path / "gt_cur_freq_mhz"); mhz && *mhz > 0) {
      gpu.core_clock_mhz = static_cast<double>(*mhz);
    } else {
      gpu.core_clock_mhz = readActiveAmdClockMhz(device_path / "pp_dpm_sclk");
    }

    if (const auto raw_util = linux_utils::readLongLong(device_path / "gpu_busy_percent"); raw_util) {
      gpu.utilization_percent = linux_utils::normalizePercent(*raw_util);
    }

    const auto vram_used_bytes =
        linux_utils::readFirstExistingLongLong({device_path / "mem_info_vram_used", device_path / "mem_info_vis_vram_used"});
    const auto vram_total_bytes = linux_utils::readFirstExistingLongLong(
        {device_path / "mem_info_vram_total", device_path / "mem_info_vis_vram_total"});
    if (vram_used_bytes && vram_total_bytes && *vram_total_bytes > 0) {
      gpu.memory_used_mib = static_cast<double>(*vram_used_bytes) / (1024.0 * 1024.0);
      gpu.memory_total_mib = static_cast<double>(*vram_total_bytes) / (1024.0 * 1024.0);
      if (gpu.memory_total_mib && *gpu.memory_total_mib > 0.0) {
        gpu.memory_utilization_percent = 100.0 * (*gpu.memory_used_mib) / (*gpu.memory_total_mib);
      }
    }

    const auto fallback = collectSensorsFallbackMetrics();
    if (!gpu.power_w) {
      gpu.power_w = fallback.gpu_power_w;
    }

    if (
      gpu.temperature_c || gpu.core_clock_mhz ||
      gpu.utilization_percent || gpu.power_w || 
      gpu.memory_used_mib || gpu.memory_total_mib
      ) {
      gpus.push_back(gpu);
    }
  }

  return gpus;
}
} 

std::vector<GpuMetrics> collectGpus() {
  auto nvidia = collectGpusFromNvidiaSmi();
  auto sysfs = collectGpusFromSysfs();

  if (!nvidia.empty()) {
    std::vector<const GpuMetrics*> supplements;
    for (const auto& gpu : sysfs) {
      if (linux_utils::toLower(gpu.name).find("nvidia") != std::string::npos) {
        supplements.push_back(&gpu);
      }
    }
    if (supplements.empty()) {
      for (const auto& gpu : sysfs) {
        supplements.push_back(&gpu);
      }
    }

    const auto merge_limit = std::min(nvidia.size(), supplements.size());
    for (size_t i = 0; i < merge_limit; ++i) {
      auto& base = nvidia[i];
      const auto& extra = *supplements[i];

      if (!base.temperature_c) {
        base.temperature_c = extra.temperature_c;
      }
      if (!base.core_clock_mhz) {
        base.core_clock_mhz = extra.core_clock_mhz;
      }
      
      if (!base.utilization_percent) {
        base.utilization_percent = extra.utilization_percent;
      }
      if (!base.power_w) {
        base.power_w = extra.power_w;
      }
      if (!base.memory_used_mib) {
        base.memory_used_mib = extra.memory_used_mib;
      }
      if (!base.memory_total_mib) {
        base.memory_total_mib = extra.memory_total_mib;
      }
      if (!base.memory_utilization_percent) {
        base.memory_utilization_percent = extra.memory_utilization_percent;
      }
    }

    const auto fallback = collectSensorsFallbackMetrics();
    for (auto& gpu : nvidia) {
    
      if (!gpu.power_w) {
        gpu.power_w = fallback.gpu_power_w;
      }
      if (!gpu.memory_utilization_percent && gpu.memory_used_mib && gpu.memory_total_mib &&
          *gpu.memory_total_mib > 0.0) {
        gpu.memory_utilization_percent = 100.0 * (*gpu.memory_used_mib) / (*gpu.memory_total_mib);
      }
    }

    return nvidia;
  }

  return sysfs;
}
