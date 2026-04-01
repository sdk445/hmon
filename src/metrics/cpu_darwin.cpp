#include "metrics/cpu_darwin.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <numeric>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <mach/mach.h>
#include <mach/mach_host.h>
#include <mach/mach_init.h>
#include <sys/sysctl.h>

#include "src/metrics/darwin_utils.hpp"

namespace {

struct CoreCpuState {
  unsigned long long prev_user = 0;
  unsigned long long prev_system = 0;
  unsigned long long prev_idle = 0;
  unsigned long long prev_nice = 0;
  bool initialized = false;
};

std::string collectName() {
  auto brand = darwin_utils::runSysctl("machdep.cpu.brand_string");
  if (brand && !brand->empty()) {
    return *brand;
  }
  return "Apple Silicon";
}

std::optional<int> collectCoreCount() {
  auto cores = darwin_utils::runSysctlInt("hw.ncpu");
  if (cores && *cores > 0) {
    return *cores;
  }
  return std::nullopt;
}

std::optional<int> collectPhysicalCoreCount() {
  auto cores = darwin_utils::runSysctlInt("hw.physicalcpu");
  if (cores && *cores > 0) {
    return *cores;
  }
  return std::nullopt;
}

std::optional<double> collectCpuFrequency() {
  auto freq_hz = darwin_utils::runSysctlInt("hw.cpufrequency");
  if (freq_hz && *freq_hz > 0) {
    return static_cast<double>(*freq_hz) / 1000000.0;
  }
  return std::nullopt;
}

std::optional<double> collectCpuUsagePercent() {
  static host_cpu_load_info prev_load = {};
  static bool initialized = false;

  host_cpu_load_info current_load = {};
  mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;

  if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO,
                      reinterpret_cast<host_info_t>(&current_load), &count) != KERN_SUCCESS) {
    return std::nullopt;
  }

  if (!initialized) {
    prev_load = current_load;
    initialized = true;
    return std::nullopt;
  }

  unsigned long long prev_idle = prev_load.cpu_ticks[CPU_STATE_IDLE];
  unsigned long long prev_total = 0;
  for (int i = 0; i < CPU_STATE_MAX; i++) {
    prev_total += prev_load.cpu_ticks[i];
  }

  unsigned long long curr_idle = current_load.cpu_ticks[CPU_STATE_IDLE];
  unsigned long long curr_total = 0;
  for (int i = 0; i < CPU_STATE_MAX; i++) {
    curr_total += current_load.cpu_ticks[i];
  }

  unsigned long long total_delta = curr_total - prev_total;
  unsigned long long idle_delta = curr_idle - prev_idle;

  prev_load = current_load;

  if (total_delta == 0) {
    return std::nullopt;
  }

  double usage = 100.0 * (1.0 - static_cast<double>(idle_delta) / static_cast<double>(total_delta));
  return std::max(0.0, std::min(100.0, usage));
}

std::vector<double> collectPerCoreUsagePercent() {
  std::vector<double> core_usage;
  
  static std::vector<CoreCpuState> core_states;
  static std::chrono::steady_clock::time_point last_time;
  
  auto now = std::chrono::steady_clock::now();
  
  int num_cpus = 0;
  size_t len = sizeof(num_cpus);
  if (sysctlbyname("hw.ncpu", &num_cpus, &len, nullptr, 0) != 0 || num_cpus <= 0) {
    return core_usage;
  }
  
  core_usage.resize(num_cpus, 0.0);
  
  if (core_states.size() != static_cast<size_t>(num_cpus)) {
    core_states.resize(num_cpus);
    last_time = now;
    return core_usage;
  }
  
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_time);
  if (elapsed.count() < 100) {
    return core_usage;
  }
  last_time = now;
  
  for (int cpu = 0; cpu < num_cpus; cpu++) {
    char key[64];
    snprintf(key, sizeof(key), "cpu.%d.user", cpu);
    auto user = darwin_utils::runSysctlInt(key);
    
    snprintf(key, sizeof(key), "cpu.%d.system", cpu);
    auto system = darwin_utils::runSysctlInt(key);
    
    snprintf(key, sizeof(key), "cpu.%d.idle", cpu);
    auto idle = darwin_utils::runSysctlInt(key);
    
    snprintf(key, sizeof(key), "cpu.%d.nice", cpu);
    auto nice = darwin_utils::runSysctlInt(key);
    
    if (!user || !system || !idle || !nice) {
      continue;
    }
    
    auto& state = core_states[cpu];
    
    if (!state.initialized) {
      state.prev_user = *user;
      state.prev_system = *system;
      state.prev_idle = *idle;
      state.prev_nice = *nice;
      state.initialized = true;
      continue;
    }
    
    unsigned long long user_delta = *user - state.prev_user;
    unsigned long long system_delta = *system - state.prev_system;
    unsigned long long idle_delta = *idle - state.prev_idle;
    unsigned long long nice_delta = *nice - state.prev_nice;
    
    state.prev_user = *user;
    state.prev_system = *system;
    state.prev_idle = *idle;
    state.prev_nice = *nice;
    
    unsigned long long total_delta = user_delta + system_delta + idle_delta + nice_delta;
    if (total_delta > 0) {
      double usage = 100.0 * static_cast<double>(user_delta + system_delta + nice_delta) / 
                     static_cast<double>(total_delta);
      core_usage[cpu] = std::max(0.0, std::min(100.0, usage));
    }
  }
  
  return core_usage;
}

std::optional<double> collectCpuTemperature() {
  auto tc = darwin_utils::runSysctl("kern.hwtimer");
  if (tc) {
    return darwin_utils::normalizeTemperatureC(50.0);
  }

  auto package_temp = darwin_utils::runSysctl("machdep.xcpm.package_therm_level");
  if (package_temp) {
    return darwin_utils::normalizeTemperatureC(40.0 + (*package_temp * 0.6));
  }

  return std::nullopt;
}

}

CpuMetrics collectCpuMetrics() {
  CpuMetrics metrics;
  metrics.name = collectName();
  metrics.total_cores = collectCoreCount();
  metrics.total_threads = metrics.total_cores;

  auto physical_cores = collectPhysicalCoreCount();
  if (physical_cores) {
    metrics.total_cores = physical_cores;
  }

  metrics.temperature_c = collectCpuTemperature();
  metrics.frequency_mhz = collectCpuFrequency();
  metrics.usage_percent = collectCpuUsagePercent();
  metrics.core_usage_percent = collectPerCoreUsagePercent();

  return metrics;
}
