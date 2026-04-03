#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hmon::plugins::docker {

struct ContainerStats {
    std::string id;
    std::string name;
    std::string image;
    std::string state;

    double cpu_percent = 0.0;
    uint64_t cpu_usage_total = 0;
    uint64_t system_cpu_usage = 0;
    uint64_t prev_cpu_usage_total = 0;
    uint64_t prev_system_cpu_usage = 0;
    bool cpu_initialized = false;
    int online_cpus = 0;

    uint64_t mem_usage = 0;
    uint64_t mem_limit = 0;
    uint64_t mem_cache = 0;
    double mem_percent = 0.0;

    uint64_t net_rx_bytes = 0;
    uint64_t net_tx_bytes = 0;
    double net_rx_bps = 0.0;
    double net_tx_bps = 0.0;
    uint64_t prev_net_rx = 0;
    uint64_t prev_net_tx = 0;
    bool net_initialized = false;

    uint64_t blk_read_bytes = 0;
    uint64_t blk_write_bytes = 0;
    double blk_read_bps = 0.0;
    double blk_write_bps = 0.0;
    uint64_t prev_blk_read = 0;
    uint64_t prev_blk_write = 0;
    bool blk_initialized = false;

    int pids_current = 0;

    /* Cumulative totals (persist across collect calls). */
    uint64_t net_rx_total = 0;
    uint64_t net_tx_total = 0;
};

struct DockerPluginCtx {
    std::string socket_path;

    mutable std::mutex data_mutex;
    std::vector<ContainerStats> containers;

    std::thread worker;
    std::atomic<bool> running{false};
    std::atomic<bool> has_data{false};

    static constexpr int CACHE_INTERVAL_MS = 500;
};

void startBackgroundCollector(DockerPluginCtx* ctx);
void stopBackgroundCollector(DockerPluginCtx* ctx);

}
