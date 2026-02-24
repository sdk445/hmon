#include "metrics/system.hpp"

#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>

RamMetrics collectRam() {
  RamMetrics metrics;
  std::ifstream meminfo("/proc/meminfo");
  if (!meminfo) {
    return metrics;
  }

  std::string key;
  long long value = 0;
  std::string unit;

  while (meminfo >> key >> value >> unit) {
    if (key == "MemTotal:") {
      metrics.total_kb = value;
    } else if (key == "MemAvailable:") {
      metrics.available_kb = value;
    }
  }

  return metrics;
}

DiskMetrics collectDisk(const std::string& mount_point) {
  DiskMetrics metrics;
  metrics.mount_point = mount_point;

  struct statvfs stat {};
  if (statvfs(mount_point.c_str(), &stat) != 0) {
    return metrics;
  }

  const unsigned long long total =
      static_cast<unsigned long long>(stat.f_blocks) * static_cast<unsigned long long>(stat.f_frsize);
  const unsigned long long free =
      static_cast<unsigned long long>(stat.f_bavail) * static_cast<unsigned long long>(stat.f_frsize);

  metrics.total_bytes = total;
  metrics.free_bytes = free;
  return metrics;
}

std::string currentTimestamp() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t raw = std::chrono::system_clock::to_time_t(now);
  std::tm local_tm {};
  localtime_r(&raw, &local_tm);
  char buffer[64];
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &local_tm);
  return buffer;
}

std::string hostName() {
  std::array<char, 256> buffer {};
  if (gethostname(buffer.data(), buffer.size()) == 0) {
    buffer.back() = '\0';
    return std::string(buffer.data());
  }
  return "unknown";
}

std::string humanBytes(unsigned long long bytes) {
  static const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
  double value = static_cast<double>(bytes);
  size_t index = 0;
  while (value >= 1024.0 && index < 4) {
    value /= 1024.0;
    ++index;
  }

  std::ostringstream out;
  if (value >= 100.0 || index == 0) {
    out << std::fixed << std::setprecision(0);
  } else {
    out << std::fixed << std::setprecision(1);
  }
  out << value << ' ' << units[index];
  return out.str();
}
