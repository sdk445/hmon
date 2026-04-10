#pragma once

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace hmon::plugins::system {

struct DiskIoSample {
  std::string name;
  uint64_t sectors_read;
  uint64_t sectors_written;
  uint64_t io_ticks_ms;
  uint64_t read_ops;
  uint64_t write_ops;
  double read_kbps = 0.0;
  double write_kbps = 0.0;
  double busy_percent = 0.0;
};

struct SystemPluginCtx {

    unsigned long long prev_rx_bytes = 0;
    unsigned long long prev_tx_bytes = 0;
    bool rx_initialized = false;
    bool tx_initialized = false;
    std::chrono::steady_clock::time_point prev_rx_time;
    std::chrono::steady_clock::time_point prev_tx_time;
    std::string active_interface;

    std::vector<DiskIoSample> prev_disk_samples;
    bool disk_io_initialized = false;
    std::chrono::steady_clock::time_point prev_disk_io_time;

    unsigned long long prev_read_sectors = 0;
    unsigned long long prev_write_sectors = 0;
    unsigned long long prev_io_time = 0;
    bool disk_initialized = false;
    std::chrono::steady_clock::time_point prev_disk_time;
    std::string root_device;
};

std::optional<long long> collectRamTotalKb();
std::optional<long long> collectRamAvailableKb();
std::optional<double> getSwapUsagePercent();
std::optional<long long> getSwapTotalKb();
std::optional<long long> getSwapFreeKb();

std::string detectRootDevice();
std::optional<unsigned long long> collectDiskTotalBytes(const std::string& mount);
std::optional<unsigned long long> collectDiskFreeBytes(const std::string& mount);

std::string detectActiveInterface();
std::optional<double> collectRxKbps(SystemPluginCtx* ctx);
std::optional<double> collectTxKbps(SystemPluginCtx* ctx);

std::vector<DiskIoSample> parseDiskStats();
std::vector<DiskIoSample> collectDiskIoDelta(SystemPluginCtx* ctx);

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
  long long buffers_kb = 0;
  long long cached_kb = 0;
  long long shared_kb = 0;
  long long slab_kb = 0;
  long long sreclaimable_kb = 0;
  long long active_kb = 0;
  long long inactive_kb = 0;
  long long dirty_kb = 0;
  long long writeback_kb = 0;
  long long hugepages_total_kb = 0;
  long long mapped_kb = 0;
  long long page_tables_kb = 0;
  long long nfs_unstable_kb = 0;
  long long bounce_kb = 0;
};

struct SystemStats {
  double load_avg_1 = 0;
  double load_avg_5 = 0;
  double load_avg_15 = 0;
  int procs_running = 0;
  int procs_blocked = 0;
  int64_t uptime_seconds = 0;
  uint64_t context_switches = 0;
  uint64_t interrupts = 0;
  uint64_t softirqs = 0;
  uint64_t forks = 0;
  uint64_t fd_open = 0;
  uint64_t fd_max = 0;
};

NetConnStats collectNetConnStats();
MemInfoDetailed collectMemInfoDetailed();
SystemStats collectSystemStats();

std::string currentTimestamp();
std::string hostName();
std::string humanBytes(unsigned long long bytes);

}
