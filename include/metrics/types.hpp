#pragma once

#include <optional>
#include <string>
#include <vector>

struct CpuMetrics {
  std::string name;
  std::optional<int> total_cores;
  std::optional<int> total_threads;
  std::optional<double> temperature_c;
  std::optional<double> frequency_mhz;
  std::optional<double> usage_percent;
};

struct RamMetrics {
  std::optional<long long> total_kb;
  std::optional<long long> available_kb;
};

struct DiskMetrics {
  std::string mount_point = "/";
  std::optional<unsigned long long> total_bytes;
  std::optional<unsigned long long> free_bytes;
};

struct GpuMetrics {
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
};

struct Snapshot {
  CpuMetrics cpu;
  RamMetrics ram;
  DiskMetrics disk;
  std::vector<GpuMetrics> gpus;
};
