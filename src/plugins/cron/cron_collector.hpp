#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hmon::plugins::cron {

struct CronJob {
    std::string schedule;
    std::string user;
    std::string command;
    std::string source;  /* /etc/crontab, /etc/cron.d/..., user crontab */
};

struct CronPluginCtx {
    std::vector<CronJob> jobs;
};

std::vector<CronJob> collectCronJobs(CronPluginCtx* ctx);

}
