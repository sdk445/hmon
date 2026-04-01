#include "metrics/system_darwin.hpp"

#include <sys/mount.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>

#include "src/metrics/darwin_utils.hpp"

#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <mach/mach.h>

std::optional<double> getSwapUsagePercent() {
  struct xsw_usage totals;
  size_t size = sizeof(totals);
  
  if (sysctlbyname("vm.swapusage", &totals, &size, nullptr, 0) != 0) {
    return std::nullopt;
  }
  
  if (totals.xsu_total <= 0) {
    return std::nullopt;
  }
  
  double used_pct = 100.0 * static_cast<double>(totals.xsu_used) / static_cast<double>(totals.xsu_total);
  return std::max(0.0, std::min(100.0, used_pct));
}

RamMetrics collectRam() {
  RamMetrics metrics;

  auto phys_mem = darwin_utils::runSysctlInt("hw.memsize");
  if (!phys_mem) {
    return metrics;
  }

  vm_size_t page_size = 0;
  mach_port_t mach_port = mach_host_self();
  vm_statistics64_data_t vm_stats;
  mach_msg_type_number_t count = sizeof(vm_stats) / sizeof(natural_t);

  if (host_page_size(mach_port, &page_size) == KERN_SUCCESS &&
      host_statistics64(mach_port, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm_stats),
                        &count) == KERN_SUCCESS) {
    unsigned long long total_kb = static_cast<unsigned long long>(*phys_mem) / 1024ULL;
    unsigned long long available_pages = vm_stats.free_count + vm_stats.inactive_count;
    unsigned long long available_kb = (available_pages * page_size) / 1024ULL;

    metrics.total_kb = total_kb;
    metrics.available_kb = available_kb;
  }

  return metrics;
}

DiskMetrics collectDisk(const std::string& mount_point) {
  DiskMetrics metrics;
  metrics.mount_point = mount_point;

  struct statfs* statfs_buf = nullptr;
  int num_mounts = getmntinfo(&statfs_buf, MNT_NOWAIT);

  if (num_mounts <= 0 || !statfs_buf) {
    return metrics;
  }

  for (int i = 0; i < num_mounts; i++) {
    if (mount_point == statfs_buf[i].f_mntonname) {
      unsigned long long total = statfs_buf[i].f_blocks * statfs_buf[i].f_bsize;
      unsigned long long free = statfs_buf[i].f_bavail * statfs_buf[i].f_bsize;

      metrics.total_bytes = total;
      metrics.free_bytes = free;
      break;
    }
  }

  freemntlist(statfs_buf);
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
  auto hostname = darwin_utils::runSysctl("kern.hostname");
  if (hostname) {
    return *hostname;
  }

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

namespace {

struct NetworkStats {
  unsigned long long rx_bytes = 0;
  unsigned long long tx_bytes = 0;
};

std::optional<NetworkStats> readInterfaceStats(const std::string& interface) {
  struct ifaddrs* ifap = nullptr;
  if (getifaddrs(&ifap) != 0) {
    return std::nullopt;
  }

  NetworkStats stats;
  bool found = false;

  for (struct ifaddrs* ifa = ifap; ifa; ifa = ifa->ifa_next) {
    if (ifa->ifa_name != interface) {
      continue;
    }
    if (ifa->ifa_data == nullptr) {
      continue;
    }

    struct if_data* ifd = reinterpret_cast<struct if_data*>(ifa->ifa_data);
    stats.rx_bytes = ifd->ifi_ibytes;
    stats.tx_bytes = ifd->ifi_obytes;
    found = true;
    break;
  }

  freeifaddrs(ifap);

  if (!found) {
    return std::nullopt;
  }

  return stats;
}

std::string findPrimaryInterface() {
  struct ifaddrs* ifap = nullptr;
  if (getifaddrs(&ifap) != 0) {
    return "lo0";
  }

  std::string best_interface;
  unsigned long long best_rx = 0;

  for (struct ifaddrs* ifa = ifap; ifa; ifa = ifa->ifa_next) {
    if (ifa->ifa_name == nullptr || ifa->ifa_data == nullptr) {
      continue;
    }

    std::string name = ifa->ifa_name;
    if (name == "lo0" || name.find("docker") != std::string::npos ||
        name.find("veth") != std::string::npos || name.find("bridge") != std::string::npos) {
      continue;
    }

    struct if_data* ifd = reinterpret_cast<struct if_data*>(ifa->ifa_data);
    if (ifd->ifi_ibytes > best_rx) {
      best_rx = ifd->ifi_ibytes;
      best_interface = name;
    }
  }

  freeifaddrs(ifap);
  return best_interface.empty() ? "lo0" : best_interface;
}

}

NetworkMetrics collectNetwork() {
  NetworkMetrics metrics;

  static std::string interface = findPrimaryInterface();
  static std::optional<NetworkStats> prev_stats;
  static std::chrono::steady_clock::time_point prev_time;

  metrics.interface = interface;

  const auto current_stats = readInterfaceStats(interface);
  const auto now = std::chrono::steady_clock::now();

  if (!current_stats) {
    interface = findPrimaryInterface();
    prev_stats.reset();
    return metrics;
  }

  if (!prev_stats) {
    prev_stats = current_stats;
    prev_time = now;
    return metrics;
  }

  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - prev_time);
  const double elapsed_seconds = static_cast<double>(elapsed.count()) / 1000.0;

  if (elapsed_seconds > 0.0) {
    const unsigned long long rx_delta = current_stats->rx_bytes - prev_stats->rx_bytes;
    const unsigned long long tx_delta = current_stats->tx_bytes - prev_stats->tx_bytes;

    metrics.rx_kbps = static_cast<double>(rx_delta) / 1024.0 / elapsed_seconds;
    metrics.tx_kbps = static_cast<double>(tx_delta) / 1024.0 / elapsed_seconds;
  }

  prev_stats = current_stats;
  prev_time = now;

  return metrics;
}
