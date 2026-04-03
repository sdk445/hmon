#pragma once

#include <cstdint>
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
  std::vector<double> core_usage_percent;
};

struct RamMetrics {
  std::optional<long long> total_kb;
  std::optional<long long> available_kb;
};

struct SwapMetrics {
  std::optional<long long> total_kb;
  std::optional<long long> free_kb;
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
  std::vector<double> gpu_core_usage_percent;
};

struct NetworkMetrics {
  std::string interface;
  std::optional<double> rx_kbps;
  std::optional<double> tx_kbps;
};

struct ProcessInfo {
  int pid = 0;
  std::string name;
  double cpu_percent = 0.0;
  double mem_percent = 0.0;
  double gpu_percent = 0.0;
  std::string command;
};

struct DockerContainer {
  std::string name;
  std::string image;
  std::string state;
  double cpu_percent = 0.0;
  uint64_t mem_usage = 0;
  uint64_t mem_limit = 0;
  double mem_percent = 0.0;
  double net_rx_bps = 0.0;
  double net_tx_bps = 0.0;
  uint64_t net_rx_total = 0;
  uint64_t net_tx_total = 0;
  double blk_read_bps = 0.0;
  double blk_write_bps = 0.0;
  int pids_current = 0;
};

struct ListeningPort {
  uint16_t port = 0;
  std::string proto;
  std::string addr;
  int pid = -1;
  std::string process;
};

struct ServiceInfo {
  std::string name;
  std::string state;
  std::string sub_state;
  std::string description;
};

struct DbInfo {
  std::string type;
  std::string status;
  int active_connections = 0;
  int max_connections = 0;
  int64_t uptime_seconds = 0;
  std::string version;
};

struct WebServerInfo {
  std::string type;
  std::string status;
  int active_connections = 0;
  double requests_per_sec = 0.0;
  int64_t total_requests = 0;
};

struct CronJob {
  std::string schedule;
  std::string user;
  std::string command;
  std::string source;
};

struct Snapshot {
  CpuMetrics cpu;
  RamMetrics ram;
  SwapMetrics swap;
  DiskMetrics disk;
  NetworkMetrics network;
  std::vector<GpuMetrics> gpus;
  std::vector<ProcessInfo> processes;
  std::vector<DockerContainer> docker_containers;
  bool docker_loading = false;
  std::vector<ListeningPort> ports;
  std::vector<ServiceInfo> services;
  std::vector<DbInfo> databases;
  std::vector<WebServerInfo> webservers;
  std::vector<CronJob> cron_jobs;
};
