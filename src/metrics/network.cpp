#include "metrics/network.hpp"

#include <fstream>
#include <sstream>
#include <string>
#include <chrono>
#include <optional>
#include <vector>

struct NetworkStats {
  unsigned long long rx_bytes = 0;
  unsigned long long tx_bytes = 0;
};

static std::optional<NetworkStats> readNetworkStats(const std::string& interface) {
  std::ifstream netdev("/proc/net/dev");
  if (!netdev) {
    return std::nullopt;
  }

  std::string line;
  while (std::getline(netdev, line)) {
    // Skip header lines
    if (line.find(':') == std::string::npos) {
      continue;
    }

    std::istringstream iss(line);
    std::string iface;
    char colon;
    iss >> iface >> colon;
    
    if (iface != interface) {
      continue;
    }

    NetworkStats stats;
    // Format: interface: rx_bytes rx_packets rx_errs rx_drop rx_fifo rx_frame rx_compressed rx_multicast
    //                    tx_bytes tx_packets tx_errs tx_drop tx_fifo tx_colls tx_carrier tx_compressed
    unsigned long long value;
    for (int i = 0; i < 8; ++i) {
      if (!(iss >> value)) {
        return std::nullopt;
      }
      if (i == 0) stats.rx_bytes = value;
    }
    for (int i = 0; i < 8; ++i) {
      if (!(iss >> value)) {
        return std::nullopt;
      }
      if (i == 0) stats.tx_bytes = value;
    }
    return stats;
  }

  return std::nullopt;
}

static std::string findPrimaryInterface() {
  std::ifstream netdev("/proc/net/dev");
  if (!netdev) {
    return "lo";
  }

  std::string best_interface;
  unsigned long long best_rx = 0;

  std::string line;
  while (std::getline(netdev, line)) {
    if (line.find(':') == std::string::npos) {
      continue;
    }

    std::istringstream iss(line);
    std::string iface;
    char colon;
    iss >> iface >> colon;

    if (iface == "lo" || iface.find("docker") != std::string::npos ||
        iface.find("veth") != std::string::npos || iface.find("br-") != std::string::npos) {
      continue;
    }

    unsigned long long rx_bytes = 0;
    iss >> rx_bytes;

    if (rx_bytes > best_rx) {
      best_rx = rx_bytes;
      best_interface = iface;
    }
  }

  return best_interface.empty() ? "lo" : best_interface;
}

NetworkMetrics collectNetwork() {
  NetworkMetrics metrics;
  
  static std::string interface = findPrimaryInterface();
  static std::optional<NetworkStats> prev_stats;
  static std::chrono::steady_clock::time_point prev_time;

  metrics.interface = interface;

  const auto current_stats = readNetworkStats(interface);
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

    // Convert to KB/s
    metrics.rx_kbps = static_cast<double>(rx_delta) / 1024.0 / elapsed_seconds;
    metrics.tx_kbps = static_cast<double>(tx_delta) / 1024.0 / elapsed_seconds;
  }

  prev_stats = current_stats;
  prev_time = now;

  return metrics;
}
