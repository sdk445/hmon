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
  std::vector<double> core_user_pct;
  std::vector<double> core_system_pct;
  std::vector<double> core_idle_pct;
  std::vector<double> core_iowait_pct;
  std::vector<double> core_irq_pct;
  std::vector<double> core_softirq_pct;
  std::vector<double> core_steal_pct;
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

struct DiskIoDevice {
  std::string name;
  double read_kbps = 0.0;
  double write_kbps = 0.0;
  double busy_percent = 0.0;
  uint64_t read_ops = 0;
  uint64_t write_ops = 0;
};

struct DiskIoMetrics {
  std::vector<DiskIoDevice> devices;
};

struct NetConnStats {
  int established = 0;
  int syn_sent = 0;
  int syn_recv = 0;
  int fin_wait1 = 0;
  int fin_wait2 = 0;
  int time_wait = 0;
  int close = 0;
  int close_wait = 0;
  int last_ack = 0;
  int listen = 0;
  int closing = 0;
};

struct MemInfoDetailed {
  std::optional<long long> buffers_kb;
  std::optional<long long> cached_kb;
  std::optional<long long> shared_kb;
  std::optional<long long> slab_kb;
  std::optional<long long> sreclaimable_kb;
  std::optional<long long> active_kb;
  std::optional<long long> inactive_kb;
  std::optional<long long> dirty_kb;
  std::optional<long long> writeback_kb;
  std::optional<long long> hugepages_total_kb;
  std::optional<long long> mapped_kb;
  std::optional<long long> page_tables_kb;
  std::optional<long long> nfs_unstable_kb;
  std::optional<long long> bounce_kb;
};

struct SystemStats {
  double load_avg_1 = 0.0;
  double load_avg_5 = 0.0;
  double load_avg_15 = 0.0;
  int procs_running = 0;
  int procs_blocked = 0;
  int64_t uptime_seconds = 0;
  int64_t total_processes = 0;
  uint64_t context_switches = 0;
  uint64_t interrupts = 0;
  uint64_t softirqs = 0;
  uint64_t forks = 0;
  uint64_t fd_open = 0;
  uint64_t fd_max = 0;
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
  DiskIoMetrics disk_io;
  NetworkMetrics network;
  NetConnStats net_conn;
  MemInfoDetailed mem_detailed;
  SystemStats sys_stats;
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
