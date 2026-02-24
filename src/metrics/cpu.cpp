#include "metrics/cpu.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <limits>
#include <numeric>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "metrics/linux_utils.hpp"
#include "metrics/sensors_fallback.hpp"

namespace {
namespace fs = std::filesystem;

bool isCpuDirectoryName(const std::string& name) {
  if (!linux_utils::startsWith(name, "cpu") || name.size() <= 3) {
    return false;
  }
  return std::all_of(name.begin() + 3, name.end(),
                     [](unsigned char c) { return std::isdigit(c) != 0; });
}

bool hwmonNameLooksCpuSensor(const std::string& lower_name) {
  return lower_name.find("k10temp") != std::string::npos ||
         lower_name.find("coretemp") != std::string::npos ||
         lower_name.find("zenpower") != std::string::npos ||
         lower_name.find("cpu") != std::string::npos;
}

bool hwmonLabelLooksCpuTemp(const std::string& lower_label) {
  return lower_label.find("cpu") != std::string::npos ||
         lower_label.find("package") != std::string::npos ||
         lower_label.find("tctl") != std::string::npos ||
         lower_label.find("tdie") != std::string::npos ||
         lower_label.find("die") != std::string::npos;
}

bool containsAlphabeticChar(const std::string& value) {
  return std::any_of(value.begin(), value.end(), [](unsigned char c) { return std::isalpha(c) != 0; });
}

std::optional<int> parsePositiveInt(const std::string& value) {
  const std::string cleaned = linux_utils::trim(value);
  if (cleaned.empty()) {
    return std::nullopt;
  }
  try {
    size_t consumed = 0;
    const long parsed = std::stol(cleaned, &consumed);
    if (consumed != cleaned.size() || parsed <= 0 || parsed > std::numeric_limits<int>::max()) {
      return std::nullopt;
    }
    return static_cast<int>(parsed);
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<double> collectCpuTemperatureFromHwmon() {
  const fs::path hwmon_base("/sys/class/hwmon");
  if (!fs::exists(hwmon_base)) {
    return std::nullopt;
  }

  std::optional<double> preferred_max;
  std::optional<double> fallback_max;

  for (const auto& hwmon : linux_utils::listDirEntries(hwmon_base)) {
    const std::string chip_name = linux_utils::toLower(linux_utils::readFirstLine(hwmon / "name").value_or(""));
    const bool cpu_chip = hwmonNameLooksCpuSensor(chip_name);

    for (const auto& file_path : linux_utils::listDirEntries(hwmon)) {
      const std::string filename = file_path.filename().string();
      if (!linux_utils::startsWith(filename, "temp") || !linux_utils::endsWith(filename, "_input")) {
        continue;
      }

      const auto raw = linux_utils::readLongLong(file_path);
      if (!raw) {
        continue;
      }
      const auto celsius = linux_utils::normalizeTemperatureC(*raw);
      if (!celsius) {
        continue;
      }

      std::string label_name = filename;
      label_name.replace(label_name.size() - std::string("_input").size(), std::string("_input").size(), "_label");
      const std::string label = linux_utils::toLower(linux_utils::readFirstLine(hwmon / label_name).value_or(""));
      const bool cpu_label = hwmonLabelLooksCpuTemp(label);

      if (cpu_label) {
        if (!preferred_max || *celsius > *preferred_max) {
          preferred_max = *celsius;
        }
      } else if (cpu_chip) {
        if (!fallback_max || *celsius > *fallback_max) {
          fallback_max = *celsius;
        }
      }
    }
  }

  if (preferred_max) {
    return preferred_max;
  }
  return fallback_max;
}

std::optional<double> collectCpuTemperature() {
  const fs::path thermal_base("/sys/class/thermal");
  std::optional<double> preferred_max;
  std::optional<double> fallback_max;

  if (fs::exists(thermal_base)) {
    for (const auto& entry : linux_utils::listDirEntries(thermal_base)) {
      const std::string zone = entry.filename().string();
      if (!linux_utils::startsWith(zone, "thermal_zone")) {
        continue;
      }

      const auto raw_temp = linux_utils::readLongLong(entry / "temp");
      if (!raw_temp) {
        continue;
      }

      const auto celsius = linux_utils::normalizeTemperatureC(*raw_temp);
      if (!celsius) {
        continue;
      }

      const std::string type = linux_utils::toLower(linux_utils::readFirstLine(entry / "type").value_or(""));
      const bool preferred = type.find("cpu") != std::string::npos ||
                             type.find("package") != std::string::npos ||
                             type.find("x86_pkg_temp") != std::string::npos ||
                             type.find("tctl") != std::string::npos ||
                             type.find("tdie") != std::string::npos;

      if (preferred) {
        if (!preferred_max || *celsius > *preferred_max) {
          preferred_max = *celsius;
        }
      } else if (!fallback_max || *celsius > *fallback_max) {
        fallback_max = *celsius;
      }
    }
  }

  if (preferred_max) {
    return preferred_max;
  }
  if (fallback_max) {
    return fallback_max;
  }
  return collectCpuTemperatureFromHwmon();
}

std::optional<double> collectCpuFrequency() {
  std::vector<double> mhz_values;
  const fs::path cpu_base("/sys/devices/system/cpu");

  if (fs::exists(cpu_base)) {
    for (const auto& entry : linux_utils::listDirEntries(cpu_base)) {
      const std::string name = entry.filename().string();
      if (!isCpuDirectoryName(name)) {
        continue;
      }
      const auto khz = linux_utils::readLongLong(entry / "cpufreq" / "scaling_cur_freq");
      if (khz && *khz > 0) {
        mhz_values.push_back(static_cast<double>(*khz) / 1000.0);
      }
    }
  }

  if (mhz_values.empty()) {
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string line;
    while (std::getline(cpuinfo, line)) {
      if (!linux_utils::startsWith(line, "cpu MHz")) {
        continue;
      }
      const auto colon = line.find(':');
      if (colon == std::string::npos) {
        continue;
      }
      const auto parsed = linux_utils::parseOptionalDouble(line.substr(colon + 1));
      if (parsed && *parsed > 0.0) {
        mhz_values.push_back(*parsed);
      }
    }
  }

  if (mhz_values.empty()) {
    return std::nullopt;
  }

  const double sum = std::accumulate(mhz_values.begin(), mhz_values.end(), 0.0);
  return sum / static_cast<double>(mhz_values.size());
}

std::optional<double> collectCpuUsagePercent() {
  std::ifstream stat("/proc/stat");
  if (!stat) {
    return std::nullopt;
  }

  std::string cpu_tag;
  unsigned long long user = 0;
  unsigned long long nice = 0;
  unsigned long long system = 0;
  unsigned long long idle = 0;
  unsigned long long iowait = 0;
  unsigned long long irq = 0;
  unsigned long long softirq = 0;
  unsigned long long steal = 0;

  stat >> cpu_tag >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal;
  if (cpu_tag != "cpu") {
    return std::nullopt;
  }

  const unsigned long long idle_ticks = idle + iowait;
  const unsigned long long total_ticks = user + nice + system + idle + iowait + irq + softirq + steal;

  struct CpuUsageState {
    unsigned long long prev_idle = 0;
    unsigned long long prev_total = 0;
    bool initialized = false;
  };
  static CpuUsageState state;

  if (!state.initialized || total_ticks < state.prev_total || idle_ticks < state.prev_idle) {
    state.prev_idle = idle_ticks;
    state.prev_total = total_ticks;
    state.initialized = true;
    return std::nullopt;
  }

  const unsigned long long total_delta = total_ticks - state.prev_total;
  const unsigned long long idle_delta = idle_ticks - state.prev_idle;

  state.prev_idle = idle_ticks;
  state.prev_total = total_ticks;

  if (total_delta == 0) {
    return std::nullopt;
  }

  const double usage = 100.0 * (1.0 - static_cast<double>(idle_delta) / static_cast<double>(total_delta));
  return std::max(0.0, std::min(100.0, usage));
}

}
std::optional<fs::path> findCpuRaplEnergyPath() {
  const fs::path base("/sys/class/powercap");
  if (!fs::exists(base)) {
    return std::nullopt;
  }

  int best_score = -1;
  std::optional<fs::path> best_path;

  for (const auto& entry : linux_utils::listDirEntries(base)) {
    if (!fs::is_directory(entry)) {
      continue;
    }
    const fs::path energy_file = entry / "energy_uj";
    if (!fs::exists(energy_file)) {
      continue;
    }

    const std::string domain_name = linux_utils::toLower(linux_utils::readFirstLine(entry / "name").value_or(""));
    int score = 0;
    if (domain_name.find("package") != std::string::npos) {
      score += 100;
    }
    if (domain_name.find("cpu") != std::string::npos) {
      score += 60;
    }
    if (domain_name.find("psys") != std::string::npos) {
      score += 30;
    }
    if (score > best_score) {
      best_score = score;
      best_path = energy_file;
    }
  }

  return best_path;
}

std::string collectName() {
  std::ifstream cpuinfo("/proc/cpuinfo");
  if (!cpuinfo) {
    return "Unknown CPU";
  }

  std::optional<std::string> model_fallback;
  std::optional<std::string> processor_fallback;
  std::string line;
  while (std::getline(cpuinfo, line)) {
    const auto colon = line.find(':');
    if (colon == std::string::npos) {
      continue;
    }

    const std::string key = linux_utils::toLower(linux_utils::trim(line.substr(0, colon)));
    const std::string value = linux_utils::trim(line.substr(colon + 1));
    if (value.empty()) {
      continue;
    }

    if (key == "model name" || key == "cpu model" || key == "hardware") {
      return value;
    }

    if (key == "model" && containsAlphabeticChar(value) && !model_fallback) {
      model_fallback = value;
    }

    if (key == "processor") {
      if (containsAlphabeticChar(value) && !processor_fallback) {
        processor_fallback = value;
      }
    }
  }

  if (model_fallback) {
    return *model_fallback;
  }
  if (processor_fallback) {
    return *processor_fallback;
  }
  return "Unknown CPU";
}

std::optional<int> collectThreadCount() {
  const fs::path cpu_base("/sys/devices/system/cpu");
  int thread_count = 0;

  if (fs::exists(cpu_base)) {
    for (const auto& entry : linux_utils::listDirEntries(cpu_base)) {
      if (isCpuDirectoryName(entry.filename().string())) {
        ++thread_count;
      }
    }
  }

  if (thread_count > 0) {
    return thread_count;
  }

  std::ifstream cpuinfo("/proc/cpuinfo");
  if (!cpuinfo) {
    return std::nullopt;
  }

  std::string line;
  while (std::getline(cpuinfo, line)) {
    const auto colon = line.find(':');
    if (colon == std::string::npos) {
      continue;
    }
    const std::string key = linux_utils::toLower(linux_utils::trim(line.substr(0, colon)));
    if (key == "processor") {
      ++thread_count;
    }
  }

  if (thread_count > 0) {
    return thread_count;
  }
  return std::nullopt;
}

std::optional<int> collectCoreCount() {
  const fs::path cpu_base("/sys/devices/system/cpu");
  std::set<std::string> unique_cores;

  if (fs::exists(cpu_base)) {
    for (const auto& entry : linux_utils::listDirEntries(cpu_base)) {
      if (!isCpuDirectoryName(entry.filename().string())) {
        continue;
      }

      const auto core_id = linux_utils::readFirstLine(entry / "topology" / "core_id");
      if (!core_id) {
        continue;
      }
      const std::string package_id =
          linux_utils::readFirstLine(entry / "topology" / "physical_package_id").value_or("0");
      unique_cores.insert(package_id + ":" + *core_id);
    }
  }

  if (!unique_cores.empty()) {
    return static_cast<int>(unique_cores.size());
  }

  std::ifstream cpuinfo("/proc/cpuinfo");
  if (!cpuinfo) {
    return std::nullopt;
  }

  std::set<std::string> unique_cpuinfo_cores;
  std::set<std::string> physical_ids;
  std::optional<std::string> block_physical_id;
  std::optional<std::string> block_core_id;
  int cpu_cores_per_socket = 0;

  const auto flush_block = [&]() {
    if (block_core_id) {
      unique_cpuinfo_cores.insert(block_physical_id.value_or("0") + ":" + *block_core_id);
    }
    block_physical_id.reset();
    block_core_id.reset();
  };

  std::string line;
  while (std::getline(cpuinfo, line)) {
    if (linux_utils::trim(line).empty()) {
      flush_block();
      continue;
    }

    const auto colon = line.find(':');
    if (colon == std::string::npos) {
      continue;
    }

    const std::string key = linux_utils::toLower(linux_utils::trim(line.substr(0, colon)));
    const std::string value = linux_utils::trim(line.substr(colon + 1));
    if (value.empty()) {
      continue;
    }

    if (key == "physical id") {
      block_physical_id = value;
      physical_ids.insert(value);
    } else if (key == "core id") {
      block_core_id = value;
    } else if (key == "cpu cores") {
      const auto parsed = parsePositiveInt(value);
      if (parsed) {
        cpu_cores_per_socket = std::max(cpu_cores_per_socket, *parsed);
      }
    }
  }
  flush_block();

  if (!unique_cpuinfo_cores.empty()) {
    return static_cast<int>(unique_cpuinfo_cores.size());
  }

  if (cpu_cores_per_socket > 0) {
    const int sockets = std::max(1, static_cast<int>(physical_ids.size()));
    return cpu_cores_per_socket * sockets;
  }

  return std::nullopt;
}

CpuMetrics collectCpuMetrics() {
  CpuMetrics metrics;
  metrics.name = collectName();
  metrics.total_cores = collectCoreCount();
  metrics.total_threads = collectThreadCount();
  if (metrics.total_cores && metrics.total_threads && *metrics.total_cores > *metrics.total_threads) {
    metrics.total_cores = metrics.total_threads;
  }
  metrics.temperature_c = collectCpuTemperature();
  metrics.frequency_mhz = collectCpuFrequency();
  metrics.usage_percent = collectCpuUsagePercent();
  return metrics;
}
