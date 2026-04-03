#pragma once

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace hmon::plugins::system {

struct SystemPluginCtx {
    /* Network state */
    unsigned long long prev_rx_bytes = 0;
    unsigned long long prev_tx_bytes = 0;
    bool net_initialized = false;
    std::chrono::steady_clock::time_point prev_net_time;
    std::string active_interface;

    /* Disk state */
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

std::string detectRootDevice();
std::optional<unsigned long long> collectDiskTotalBytes(const std::string& mount);
std::optional<unsigned long long> collectDiskFreeBytes(const std::string& mount);

std::string detectActiveInterface();
std::optional<double> collectRxKbps(SystemPluginCtx* ctx);
std::optional<double> collectTxKbps(SystemPluginCtx* ctx);

std::string currentTimestamp();
std::string hostName();
std::string humanBytes(unsigned long long bytes);

}
