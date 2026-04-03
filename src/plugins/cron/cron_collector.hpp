#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace hmon::plugins::cron {

struct CronJob {
    std::string schedule;
    std::string user;
    std::string command;
    std::string source;
};

struct CronPluginCtx {
    std::vector<CronJob> jobs;
    std::vector<CronJob> cached_result;
    std::chrono::steady_clock::time_point last_cache_time;
    static constexpr int TTL_SECONDS = 60;
};

std::vector<CronJob> collectCronJobs(CronPluginCtx* ctx);

}
