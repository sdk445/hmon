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

std::optional<std::string> readDriverName(const fs::path& device_path) {
  const fs::path driver_link = device_path / "driver";
  if (fs::exists(driver_link)) {
    std::error_code ec;
    const fs::path target = fs::read_symlink(driver_link, ec);
    if (!ec) {
      const std::string name = target.filename().string();
      if (!name.empty()) {
        return name;
      }
    }
  }

  std::ifstream uevent_file(device_path / "uevent");
  if (!uevent_file) {
    return std::nullopt;
  }

  std::string line;
  while (std::getline(uevent_file, line)) {
    if (linux_utils::startsWith(line, "DRIVER=")) {
      const std::string driver = linux_utils::trim(line.substr(7));
      if (!driver.empty()) {
        return driver;
      }
      break;
    }
  }
  return std::nullopt;
}

bool isDisplayClassDevice(const fs::path& device_path) {
  const auto class_code = linux_utils::readFirstLine(device_path / "class");
  if (!class_code) {
    return true;
  }
  const std::string lower = linux_utils::toLower(*class_code);
  return linux_utils::startsWith(lower, "0x03");
}

std::optional<bool> detectCardInUse(const fs::path& drm_base, const fs::path& card_path, const fs::path& device_path) {
  const std::string card_prefix = card_path.filename().string() + "-";
  bool saw_connector_status = false;
  for (const auto& entry_path : linux_utils::listDirEntries(drm_base)) {
    const std::string entry_name = entry_path.filename().string();
    if (!linux_utils::startsWith(entry_name, card_prefix)) {
      continue;
    }
    if (entry_name.find("render") != std::string::npos) {
      continue;
    }

    const auto connector_status = linux_utils::readFirstLine(entry_path / "status");
    if (!connector_status) {
      continue;
    }

    saw_connector_status = true;
    if (linux_utils::toLower(*connector_status) == "connected") {
      return true;
    }
  }

  if (saw_connector_status) {
    return false;
  }

  const auto boot_vga = linux_utils::readLongLong(device_path / "boot_vga");
  if (boot_vga) {
    return *boot_vga == 1;
  }

  return std::nullopt;
}

int telemetryScore(const GpuMetrics& gpu) {
  int score = 0;
  if (gpu.temperature_c) {
    score += 2;
  }
  if (gpu.core_clock_mhz) {
    score += 2;
  }
  if (gpu.utilization_percent) {
    score += 3;
  }
  if (gpu.power_w) {
    score += 2;
  }
  if (gpu.memory_used_mib || gpu.memory_total_mib) {
    score += 2;
  }
  if (gpu.memory_utilization_percent) {
    ++score;
  }
  return score;
}

bool gpuLooksNvidia(const GpuMetrics& gpu) {
  const std::string lower_name = linux_utils::toLower(gpu.name);
  const std::string lower_source = linux_utils::toLower(gpu.source);
  return lower_name.find("nvidia") != std::string::npos || lower_source.find("nvidia") != std::string::npos;
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

  const auto fallback = collectSensorsFallbackMetrics();
  for (const auto& card_path : linux_utils::listDirEntries(drm_base)) {
    const std::string card_name = card_path.filename().string();
    if (!linux_utils::startsWith(card_name, "card") || card_name.find('-') != std::string::npos) {
      continue;
    }

    const fs::path device_path = card_path / "device";
    if (!fs::exists(device_path) || !isDisplayClassDevice(device_path)) {
      continue;
    }

    const auto vendor_id = linux_utils::readFirstLine(device_path / "vendor");
    const auto driver_name = readDriverName(device_path);

    GpuMetrics gpu;
    gpu.source = driver_name ? ("sysfs/" + *driver_name) : "sysfs";
    gpu.name = card_name + " (" + vendorNameFromId(vendor_id) + ")";
    gpu.in_use = detectCardInUse(drm_base, card_path, device_path);

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

    if (!gpu.power_w) {
      gpu.power_w = fallback.gpu_power_w;
    }

    gpus.push_back(gpu);
  }

  std::stable_sort(gpus.begin(), gpus.end(), [](const GpuMetrics& lhs, const GpuMetrics& rhs) {
    const int lhs_score = telemetryScore(lhs);
    const int rhs_score = telemetryScore(rhs);
    if (lhs_score != rhs_score) {
      return lhs_score > rhs_score;
    }
    return lhs.name < rhs.name;
  });

  return gpus;
}
}

std::vector<GpuMetrics> collectGpus() {
  auto nvidia = collectGpusFromNvidiaSmi();
  auto sysfs = collectGpusFromSysfs();

  if (!nvidia.empty()) {
    std::vector<bool> sysfs_used(sysfs.size(), false);
    std::vector<size_t> nvidia_sysfs_indices;
    nvidia_sysfs_indices.reserve(sysfs.size());
    for (size_t i = 0; i < sysfs.size(); ++i) {
      if (gpuLooksNvidia(sysfs[i])) {
        nvidia_sysfs_indices.push_back(i);
      }
    }

    size_t next_nvidia_index = 0;
    size_t next_any_index = 0;
    for (auto& base : nvidia) {
      const GpuMetrics* extra = nullptr;

      while (next_nvidia_index < nvidia_sysfs_indices.size()) {
        const size_t idx = nvidia_sysfs_indices[next_nvidia_index++];
        if (!sysfs_used[idx]) {
          sysfs_used[idx] = true;
          extra = &sysfs[idx];
          break;
        }
      }

      if (!extra) {
        while (next_any_index < sysfs.size()) {
          const size_t idx = next_any_index++;
          if (!sysfs_used[idx]) {
            sysfs_used[idx] = true;
            extra = &sysfs[idx];
            break;
          }
        }
      }

      if (!extra) {
        continue;
      }

      if (!base.temperature_c) {
        base.temperature_c = extra->temperature_c;
      }
      if (!base.core_clock_mhz) {
        base.core_clock_mhz = extra->core_clock_mhz;
      }

      if (!base.utilization_percent) {
        base.utilization_percent = extra->utilization_percent;
      }
      if (!base.power_w) {
        base.power_w = extra->power_w;
      }
      if (!base.memory_used_mib) {
        base.memory_used_mib = extra->memory_used_mib;
      }
      if (!base.memory_total_mib) {
        base.memory_total_mib = extra->memory_total_mib;
      }
      if (!base.memory_utilization_percent) {
        base.memory_utilization_percent = extra->memory_utilization_percent;
      }
      if (!base.in_use.has_value() && extra->in_use.has_value()) {
        base.in_use = extra->in_use;
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

    for (size_t i = 0; i < sysfs.size(); ++i) {
      if (!sysfs_used[i]) {
        nvidia.push_back(sysfs[i]);
      }
    }

    std::stable_sort(nvidia.begin(), nvidia.end(), [](const GpuMetrics& lhs, const GpuMetrics& rhs) {
      const int lhs_score = telemetryScore(lhs);
      const int rhs_score = telemetryScore(rhs);
      if (lhs_score != rhs_score) {
        return lhs_score > rhs_score;
      }
      return lhs.name < rhs.name;
    });

    return nvidia;
  }

  return sysfs;
}
